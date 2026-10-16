#pragma once

#include "sc_common.hpp"
#include "sc_coo_builder.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/keyword_helper.hpp"

#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "sc.h"

namespace duckdb {
namespace sc_rf {

//! The metadata column holding the sample id. Everything else is a candidate
//! target.
constexpr const char *kSampleIdColumn = "sample_id";

//! What fitting and cross-validating both need to know about their inputs.
//!
//! Both read the same two relations -- COO triplets, and (sample_id, target) --
//! validate them the same way and fit the same forests, so the helpers below
//! take this instead of either function's own bind data, which inherits it.
//!
//! `caller` is the SQL function's name, so an error names the function the user
//! called.
struct ScTrainingInput {
	string data_relation;
	string metadata_relation;
	//! Resolved at bind time: the caller's `target_column :=`, or the metadata
	//! relation's single non-sample_id column.
	string target_column;
	const char *caller = "fit";
	//! The data relation's id types, probed at bind.
	miint::ScIdTypes id_types;
};

//! Fill `params` with sklearn's defaults for the task.
void ApplyDefaults(sc_rf_params_t &params, bool classification);

//! The most trees a forest may have, 1000x the default. sklearn has no limit,
//! but sc reserves the whole forest before growing a tree, and a count the
//! allocator grants lazily then grows trees until memory runs out.
constexpr int64_t kMaxNEstimators = 100000;

//! Parse `n_estimators`: 1 to kMaxNEstimators.
int64_t ParseNEstimators(const Value &v, const char *caller);

// Parse one named parameter's SQL value into its sc tagged union. `caller` names
// the SQL function in any error.
void ParseMaxFeatures(const Value &v, const char *caller, sc_max_features_t &out);
void ParseMinSamples(const Value &v, const char *name, uint64_t min_count, const char *caller, sc_min_samples_t &out);
void ParseMaxSamples(const Value &v, const char *caller, sc_max_samples_t &out);
void ParseCriterion(const Value &v, bool classification, const char *caller, sc_criterion_t &out);

//! Parse `k` into `params` if it is a forest parameter, which fit and
//! cross-validation share, and return false for any other name. One copy, so
//! the two functions cannot drift apart.
bool ParseForestParameter(const std::string &k, const Value &v, bool classification, const char *caller,
                          sc_rf_params_t &params);

//! Register the named parameters fit and cross-validation share: the training
//! input's and the forest's. One list, so neither function can refuse a
//! parameter the other accepts.
void RegisterTrainingParameters(TableFunction &fn);

//! Checks that span forest parameters, run once all of them are parsed.
void ValidateForestParameters(const sc_rf_params_t &params, const char *caller);

//! Work out which metadata column holds the target, or check the one the caller
//! named (`requested`, empty when none was).
std::string ResolveTargetColumn(Connection &conn, const std::string &relation, const std::string &requested,
                                const char *caller);

// Templates: defined here because the target type varies by task -- Utf8 labels
// for a classifier, Float64 values for a regressor.

//! Scan the metadata relation into a sample_id -> target map, rejecting NULLs
//! and duplicate labels. The target is cast to the SQL type matching `T`:
//! VARCHAR labels for a classifier, DOUBLE values for a regressor.
template <class T>
std::unordered_map<std::string, T> ScanTargets(Connection &conn, const ScTrainingInput &bind) {
	static_assert(std::is_same<T, std::string>::value || std::is_same<T, double>::value,
	              "targets are VARCHAR labels or DOUBLE values");
	const char *cast = std::is_same<T, double>::value ? "DOUBLE" : "VARCHAR";
	const auto q = KeywordHelper::WriteOptionallyQuoted(bind.metadata_relation);
	const auto col = KeywordHelper::WriteOptionallyQuoted(bind.target_column);
	auto result = conn.Query(std::string("SELECT ") + kSampleIdColumn + ", " + col + "::" + cast + " FROM " + q);
	if (result->HasError()) {
		throw InvalidInputException("%s: metadata relation '%s' must expose (sample_id, %s) castable to %s: %s",
		                            bind.caller, bind.metadata_relation, bind.target_column, cast, result->GetError());
	}
	std::unordered_map<std::string, T> targets;
	while (auto chunk = result->Fetch()) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			auto s = chunk->data[0].GetValue(row);
			auto t = chunk->data[1].GetValue(row);
			if (s.IsNull() || t.IsNull()) {
				throw InvalidInputException("%s: NULL in metadata relation '%s' (sample_id and %s must be "
				                            "non-NULL)",
				                            bind.caller, bind.metadata_relation, bind.target_column);
			}
			const auto sample = s.ToString();
			// Two labels for one sample cannot be reconciled -- unlike duplicate
			// counts, there is no defensible way to combine them.
			if (!targets.emplace(sample, t.GetValue<T>()).second) {
				throw InvalidInputException(
				    "%s: sample '%s' has more than one %s in metadata relation '%s'; deduplicate it first", bind.caller,
				    sample, bind.target_column, bind.metadata_relation);
			}
		}
	}
	return targets;
}

//! Both relations must describe exactly the same samples. Report both
//! directions at once so one round trip fixes the whole mismatch.
template <class T>
void RequireSameSamples(const std::vector<std::string> &data_samples, const std::unordered_map<std::string, T> &targets,
                        const ScTrainingInput &bind) {
	// unlabelled: Sample is in Data, but missing from Metadata
	std::vector<std::string> unlabelled;
	for (const auto &s : data_samples) {
		if (targets.find(s) == targets.end()) {
			unlabelled.push_back(s);
		}
	}
	// uncounted: samples the metadata labels that have no counts.
	std::vector<std::string> uncounted;
	if (targets.size() + unlabelled.size() != data_samples.size()) {
		std::unordered_map<std::string, bool> present;
		for (const auto &s : data_samples) {
			present.emplace(s, true);
		}
		for (const auto &kv : targets) {
			if (present.find(kv.first) == present.end()) {
				uncounted.push_back(kv.first);
			}
		}
	}
	if (unlabelled.empty() && uncounted.empty()) {
		return;
	}

	auto sample_list = [](const std::vector<std::string> &v) {
		string out;
		const size_t shown = v.size() < 5 ? v.size() : 5;
		for (size_t i = 0; i < shown; i++) {
			out += (i ? ", " : "") + v[i];
		}
		if (v.size() > shown) {
			out += StringUtil::Format(", ... (+%llu more)", (uint64_t)(v.size() - shown));
		}
		return out;
	};
	string msg = StringUtil::Format("%s: The data and metadata relations describe different sample sets.\n"
	                                "  Every sample with counts must have exactly one label, and vice versa.",
	                                bind.caller);
	if (!unlabelled.empty()) {
		msg +=
		    StringUtil::Format("\n  %llu sample(s) in '%s' with no %s: %s", (uint64_t)unlabelled.size(),
		                       bind.data_relation.c_str(), bind.target_column.c_str(), sample_list(unlabelled).c_str());
	}
	if (!uncounted.empty()) {
		msg += StringUtil::Format("\n  %llu sample(s) in '%s' with no counts: %s", (uint64_t)uncounted.size(),
		                          bind.metadata_relation.c_str(), sample_list(uncounted).c_str());
	}
	// The views are named apart from the relations, which are often called
	// `counts` and `labels`.
	const auto data = KeywordHelper::WriteOptionallyQuoted(bind.data_relation);
	const auto metadata = KeywordHelper::WriteOptionallyQuoted(bind.metadata_relation);
	msg += StringUtil::Format(
	    "\n\nRemedy:\n"
	    "  Restrict both sides to the samples they share, then refit on shared_counts and shared_labels:\n"
	    "    CREATE VIEW shared AS SELECT DISTINCT sample_id FROM %s SEMI JOIN %s USING (sample_id);\n"
	    "    CREATE VIEW shared_counts AS SELECT * FROM %s SEMI JOIN shared USING (sample_id);\n"
	    "    CREATE VIEW shared_labels AS SELECT * FROM %s SEMI JOIN shared USING (sample_id);\n"
	    "  Or fix the upstream join if the mismatch is unexpected -- a truncated metadata\n"
	    "  export trains a model on fewer samples than you think.",
	    data, metadata, data, metadata);
	throw InvalidInputException(msg);
}

//! Scan a training input: its cells into a table, and its targets into
//! `targets` in the table's sample order, the order sc reads them in. That
//! order comes from the table's sorted sample ids, looked up through a hash,
//! never from a second ORDER BY, whose collation need not match std::string's
//! byte order.
template <class T>
std::unique_ptr<miint::ScCooTable> ScanTrainingInput(ClientContext &context, Connection &conn,
                                                     const ScTrainingInput &bind, std::vector<T> &targets) {
	// The metadata first: it is one row per sample, so a bad target fails before
	// the count scan rather than after it.
	const auto by_sample = ScanTargets<T>(conn, bind);

	miint::ScCooBuilder builder;
	miint::ScanCooRelation(context, conn, bind.data_relation, bind.caller, builder);
	if (builder.NumNonZeros() == 0) {
		throw InvalidInputException("%s: data relation '%s' produced no cells", bind.caller, bind.data_relation);
	}
	miint::RequireNoDuplicateCells(builder, bind.caller, bind.data_relation);

	auto table = builder.Finalize();
	RequireSameSamples(table->SampleIds(), by_sample, bind);
	targets.clear();
	targets.reserve(table->SampleIds().size());
	for (const auto &s : table->SampleIds()) {
		targets.push_back(by_sample.at(s));
	}
	return table;
}

} // namespace sc_rf
} // namespace duckdb
