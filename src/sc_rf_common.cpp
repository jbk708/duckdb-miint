#include "sc_rf_common.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace sc_rf {

namespace {

void ReleaseNothing(ArrowArray *array) {
	array->release = nullptr;
}
void ReleaseNothingSchema(ArrowSchema *schema) {
	schema->release = nullptr;
}

} // namespace

void BuildTargets(TargetArray &t, bool classification) {
	t.schema.format = classification ? "u" : "g";
	t.schema.name = nullptr;
	t.schema.metadata = nullptr;
	t.schema.flags = 0;
	t.schema.n_children = 0;
	t.schema.children = nullptr;
	t.schema.dictionary = nullptr;
	t.schema.release = ReleaseNothingSchema;
	t.schema.private_data = nullptr;

	int64_t length = 0;
	int64_t n_buffers = 0;
	if (classification) {
		length = static_cast<int64_t>(t.labels.size());
		t.offsets.reserve(t.labels.size() + 1);
		int32_t cursor = 0;
		t.offsets.push_back(cursor);
		for (const auto &s : t.labels) {
			t.chars.insert(t.chars.end(), s.begin(), s.end());
			cursor += static_cast<int32_t>(s.size());
			t.offsets.push_back(cursor);
		}
		t.buffers[1] = t.offsets.data();
		t.buffers[2] = t.chars.empty() ? nullptr : t.chars.data();
		n_buffers = 3;
	} else {
		length = static_cast<int64_t>(t.numbers.size());
		t.buffers[1] = t.numbers.data();
		n_buffers = 2;
	}
	// buffers[0] stays NULL: no nulls, which sc requires (null_count must be 0).
	t.array.length = length;
	t.array.null_count = 0;
	t.array.offset = 0;
	t.array.n_buffers = n_buffers;
	t.array.n_children = 0;
	t.array.buffers = t.buffers;
	t.array.children = nullptr;
	t.array.dictionary = nullptr;
	t.array.release = ReleaseNothing;
	t.array.private_data = nullptr;
}

//! Fill `params` with sklearn's defaults for the task.
//!
//! Classifier: gini, max_features='sqrt'. Regressor: squared_error,
//! max_features=1.0 (all). Both: 100 trees, unbounded depth, min_samples_split=2,
//! min_samples_leaf=1, bootstrap=True, max_samples=None.
void ApplyDefaults(sc_rf_params_t &params, bool classification) {
	params.criterion = classification ? SC_CRITERION_GINI : SC_CRITERION_SQUARED_ERROR;
	params.max_depth = 0; // sklearn's None
	params.max_features.kind = classification ? SC_MAX_FEATURES_SQRT : SC_MAX_FEATURES_ALL;
	params.min_samples_split.kind = SC_MIN_SAMPLES_COUNT;
	params.min_samples_split.count = 2;
	params.min_samples_leaf.kind = SC_MIN_SAMPLES_COUNT;
	params.min_samples_leaf.count = 1;
	params.min_weight_fraction_leaf = 0.0;
	params.min_impurity_decrease = 0.0;
	params.n_estimators = 100;
	params.bootstrap = true;
	params.max_samples.kind = SC_MAX_SAMPLES_ALL;
	params.random_state = 0;
	params.optimize_feature_selection = false;
	params.rfe_step = 0.0;
	params.parameter_tuning = false;
	params.cv = 0;
	params.n_threads = 0;
}

int64_t ParseNEstimators(const Value &v, const char *caller) {
	const auto n = v.GetValue<int64_t>();
	if (n <= 0) {
		throw InvalidInputException("%s: n_estimators must be > 0 (got %lld)", caller, (int64_t)n);
	}
	if (n > kMaxNEstimators) {
		throw InvalidInputException("%s: n_estimators must be <= %lld (got %lld)", caller, (int64_t)kMaxNEstimators,
		                            (int64_t)n);
	}
	return n;
}

//! Is this value an SQL integer rather than a float?
//!
//! sklearn's int/float distinction is load-bearing for the tagged unions --
//! `max_features=10` means ten features, `max_features=0.3` means thirty
//! percent -- and SQL's literal types carry exactly that information, so the
//! named parameters take ANY and dispatch on what the user actually wrote.
bool IsIntegerValue(const Value &v) {
	switch (v.type().id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return true;
	default:
		return false;
	}
}

void ParseMaxFeatures(const Value &v, const char *caller, sc_max_features_t &out) {
	if (v.type().id() == LogicalTypeId::VARCHAR) {
		const auto s = v.GetValue<string>();
		if (StringUtil::CIEquals(s, "sqrt")) {
			out.kind = SC_MAX_FEATURES_SQRT;
		} else if (StringUtil::CIEquals(s, "log2")) {
			out.kind = SC_MAX_FEATURES_LOG2;
		} else if (StringUtil::CIEquals(s, "all") || StringUtil::CIEquals(s, "none")) {
			out.kind = SC_MAX_FEATURES_ALL;
		} else {
			throw InvalidInputException("%s: max_features must be 'sqrt', 'log2', 'all', a fraction in (0, 1], "
			                            "or a positive integer (got '%s')",
			                            caller, s);
		}
		return;
	}
	if (IsIntegerValue(v)) {
		const auto n = v.GetValue<int64_t>();
		if (n <= 0) {
			throw InvalidInputException("%s: max_features count must be > 0 (got %lld)", caller, (int64_t)n);
		}
		out.kind = SC_MAX_FEATURES_COUNT;
		out.count = static_cast<uint64_t>(n);
		return;
	}
	const auto f = v.GetValue<double>();
	if (!(f > 0.0 && f <= 1.0)) {
		throw InvalidInputException("%s: max_features fraction must be in (0, 1] (got %g)", caller, f);
	}
	out.kind = SC_MAX_FEATURES_FRACTION;
	out.fraction = f;
}

void ParseMinSamples(const Value &v, const char *name, uint64_t min_count, const char *caller, sc_min_samples_t &out) {
	if (IsIntegerValue(v)) {
		const auto n = v.GetValue<int64_t>();
		if (n < static_cast<int64_t>(min_count)) {
			throw InvalidInputException("%s: %s count must be >= %llu (got %lld)", caller, name, (uint64_t)min_count,
			                            (int64_t)n);
		}
		out.kind = SC_MIN_SAMPLES_COUNT;
		out.count = static_cast<uint64_t>(n);
		return;
	}
	const auto f = v.GetValue<double>();
	if (!(f > 0.0 && f <= 1.0)) {
		throw InvalidInputException("%s: %s fraction must be in (0, 1] (got %g)", caller, name, f);
	}
	out.kind = SC_MIN_SAMPLES_FRACTION;
	out.fraction = f;
}

void ParseMaxSamples(const Value &v, const char *caller, sc_max_samples_t &out) {
	if (v.type().id() == LogicalTypeId::VARCHAR) {
		const auto s = v.GetValue<string>();
		if (!StringUtil::CIEquals(s, "all") && !StringUtil::CIEquals(s, "none")) {
			throw InvalidInputException("%s: max_samples must be 'all', a fraction in (0, 1], or a positive "
			                            "integer (got '%s')",
			                            caller, s);
		}
		out.kind = SC_MAX_SAMPLES_ALL;
		return;
	}
	if (IsIntegerValue(v)) {
		const auto n = v.GetValue<int64_t>();
		if (n <= 0) {
			throw InvalidInputException("%s: max_samples count must be > 0 (got %lld)", caller, (int64_t)n);
		}
		out.kind = SC_MAX_SAMPLES_COUNT;
		out.count = static_cast<uint64_t>(n);
		return;
	}
	const auto f = v.GetValue<double>();
	if (!(f > 0.0 && f <= 1.0)) {
		throw InvalidInputException("%s: max_samples fraction must be in (0, 1] (got %g)", caller, f);
	}
	out.kind = SC_MAX_SAMPLES_FRACTION;
	out.fraction = f;
}

void ParseCriterion(const Value &v, bool classification, const char *caller, sc_criterion_t &out) {
	const auto s = v.GetValue<string>();
	// sc rejects a criterion that does not match the task, but catching it here
	// names both the criterion and the function the user called.
	if (classification) {
		if (StringUtil::CIEquals(s, "gini")) {
			out = SC_CRITERION_GINI;
		} else if (StringUtil::CIEquals(s, "entropy")) {
			out = SC_CRITERION_ENTROPY;
		} else {
			throw InvalidInputException("%s_classifier: criterion must be 'gini' or 'entropy' (got '%s')", caller, s);
		}
		return;
	}
	if (StringUtil::CIEquals(s, "squared_error")) {
		out = SC_CRITERION_SQUARED_ERROR;
		return;
	}
	throw InvalidInputException("%s_regressor: criterion must be 'squared_error' (got '%s')", caller, s);
}

//! Work out which metadata column holds the target.
//!
//! With one candidate there is nothing to choose, so choose it -- `(sample_id,
//! month)` needs no configuration. With several the answer is genuinely unknown,
//! and guessing would train on the wrong variable without any error: a
//! `(sample_id, age, bmi, delivery_mode)` table has three equally plausible
//! targets and picking one silently would be the worst outcome.
//!
//! This replaces a hardcoded default of "value", which happened to suit
//! long-format metadata and quietly failed for everything else.
std::string ResolveTargetColumn(Connection &conn, const std::string &relation, const char *caller) {
	const auto q = KeywordHelper::WriteOptionallyQuoted(relation);
	// LIMIT 0 binds the relation and returns its schema without scanning it.
	auto probe = conn.Query("SELECT * FROM " + q + " LIMIT 0");
	if (probe->HasError()) {
		throw InvalidInputException("%s: metadata relation '%s' could not be read: %s", caller, relation,
		                            probe->GetError());
	}

	std::vector<std::string> candidates;
	bool has_sample_id = false;
	for (const auto &name : probe->names) {
		if (StringUtil::CIEquals(name, kSampleIdColumn)) {
			has_sample_id = true;
			continue; // if sample_id, dont push to candidates
		}
		candidates.push_back(name);
	}
	if (!has_sample_id) {
		throw InvalidInputException("%s: metadata relation '%s' has no 'sample_id' column", caller, relation);
	}
	if (candidates.empty()) {
		throw InvalidInputException(
		    "%s: metadata relation '%s' has only a sample_id column; it needs a target column too", caller, relation);
	}
	if (candidates.size() == 1) {
		return candidates[0];
	}

	std::string list;
	for (size_t i = 0; i < candidates.size(); i++) {
		list += (i ? ", " : "") + ("'" + candidates[i] + "'");
	}
	throw InvalidInputException(
	    "%s: Metadata relation '%s' has more than one column that could be the target.\n"
	    "  Candidates : [%s]\n\n"
	    "Remedy:\n"
	    "  Name the variable you are modelling, so the wrong one cannot be picked silently:\n"
	    "    SELECT * FROM %s_regressor('counts', '%s',\n"
	    "                                   target_column := 'one_of_the_above',\n"
	    "                                   name := 'my_model');\n"
	    "  A metadata relation with exactly one non-sample_id column needs no target_column at all.",
	    caller, relation, list, caller, relation);
}

} // namespace sc_rf
} // namespace duckdb
