#include "sc_rf_common.hpp"

#include <cmath>
#include <string>
#include <vector>
#include "miint_named_parameter.hpp"

namespace duckdb {
namespace sc_rf {

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
//! sklearn's int/float distinction decides the tagged unions --
//! `max_features=10` means ten features, `max_features=0.3` means thirty
//! percent -- and SQL's literal types carry exactly that information, so the
//! named parameters take ANY and dispatch on what the user wrote.
bool IsIntegerValue(const Value &v) {
	return v.type().IsIntegral();
}

//! Read a fraction, which must be written as one. GetValue<double> converts
//! other types too -- `true` reads as 1.0 -- so they are refused rather than
//! taken for a number.
double ReadFraction(const Value &v, const char *name, const char *accepted, const char *caller) {
	// sc floors the count a fraction resolves to, and a FLOAT holds 0.7 as
	// 0.69999999: 0.7 of 10 features would become 6.
	if (v.type().id() == LogicalTypeId::FLOAT) {
		throw InvalidInputException(
		    "%s: %s is a FLOAT, which holds %s only approximately and can round the count down; "
		    "pass a DOUBLE or a literal",
		    caller, name, v.ToString());
	}
	if (v.type().IsFloating() || v.type().id() == LogicalTypeId::DECIMAL) {
		return v.GetValue<double>();
	}
	throw InvalidInputException("%s: %s must be %s, got a %s", caller, name, accepted, v.type().ToString());
}

constexpr const char *kMaxFeaturesAccepted =
    "'sqrt', 'log2', 'all', 'none', a fraction in (0, 1], or a positive integer";
constexpr const char *kMaxSamplesAccepted = "'all', 'none', a fraction in (0, 1], or a positive integer";

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
			throw InvalidInputException("%s: max_features must be %s (got '%s')", caller, kMaxFeaturesAccepted, s);
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
	const auto f = ReadFraction(v, "max_features", kMaxFeaturesAccepted, caller);
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
	const auto f = ReadFraction(v, name, "a count or a fraction", caller);
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
			throw InvalidInputException("%s: max_samples must be %s (got '%s')", caller, kMaxSamplesAccepted, s);
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
	const auto f = ReadFraction(v, "max_samples", kMaxSamplesAccepted, caller);
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
			throw InvalidInputException("%s: criterion must be 'gini' or 'entropy' (got '%s')", caller, s);
		}
		return;
	}
	if (StringUtil::CIEquals(s, "squared_error")) {
		out = SC_CRITERION_SQUARED_ERROR;
		return;
	}
	throw InvalidInputException("%s: criterion must be 'squared_error' (got '%s')", caller, s);
}

bool ParseForestParameter(const std::string &k, const Value &v, bool classification, const char *caller,
                          sc_rf_params_t &params) {
	if (StringUtil::CIEquals(k, "n_estimators")) {
		params.n_estimators = ParseNEstimators(v, caller);
	} else if (StringUtil::CIEquals(k, "random_state")) {
		params.random_state = static_cast<uint64_t>(v.GetValue<int64_t>());
	} else if (StringUtil::CIEquals(k, "max_depth")) {
		// <= 0 is sklearn's None (unbounded), which sc reads the same way.
		params.max_depth = v.GetValue<int64_t>();
	} else if (StringUtil::CIEquals(k, "criterion")) {
		ParseCriterion(v, classification, caller, params.criterion);
	} else if (StringUtil::CIEquals(k, "max_features")) {
		ParseMaxFeatures(v, caller, params.max_features);
	} else if (StringUtil::CIEquals(k, "min_samples_split")) {
		ParseMinSamples(v, "min_samples_split", 2, caller, params.min_samples_split);
	} else if (StringUtil::CIEquals(k, "min_samples_leaf")) {
		ParseMinSamples(v, "min_samples_leaf", 1, caller, params.min_samples_leaf);
	} else if (StringUtil::CIEquals(k, "min_weight_fraction_leaf")) {
		// sc checks both of these too, but only once the fit runs, after the
		// whole table has been scanned.
		params.min_weight_fraction_leaf = v.GetValue<double>();
		if (!(params.min_weight_fraction_leaf >= 0.0 && params.min_weight_fraction_leaf <= 0.5)) {
			throw InvalidInputException("%s: min_weight_fraction_leaf must be in [0, 0.5] (got %s)", caller,
			                            Value::DOUBLE(params.min_weight_fraction_leaf).ToString());
		}
	} else if (StringUtil::CIEquals(k, "min_impurity_decrease")) {
		params.min_impurity_decrease = v.GetValue<double>();
		if (!(std::isfinite(params.min_impurity_decrease) && params.min_impurity_decrease >= 0.0)) {
			throw InvalidInputException("%s: min_impurity_decrease must be finite and >= 0 (got %s)", caller,
			                            Value::DOUBLE(params.min_impurity_decrease).ToString());
		}
	} else if (StringUtil::CIEquals(k, "bootstrap")) {
		params.bootstrap = v.GetValue<bool>();
	} else if (StringUtil::CIEquals(k, "max_samples")) {
		ParseMaxSamples(v, caller, params.max_samples);
	} else {
		return false;
	}
	return true;
}

void RegisterTrainingParameters(TableFunction &fn) {
	AddNamedParameter(fn, "target_column", LogicalType::VARCHAR);
	AddNamedParameter(fn, "threads", LogicalType::INTEGER);
	AddNamedParameter(fn, "n_estimators", LogicalType::BIGINT);
	AddNamedParameter(fn, "random_state", LogicalType::BIGINT);
	AddNamedParameter(fn, "max_depth", LogicalType::BIGINT);
	AddNamedParameter(fn, "criterion", LogicalType::VARCHAR);
	AddNamedParameter(fn, "min_weight_fraction_leaf", LogicalType::DOUBLE);
	AddNamedParameter(fn, "min_impurity_decrease", LogicalType::DOUBLE);
	AddNamedParameter(fn, "bootstrap", LogicalType::BOOLEAN);
	// ANY, not a fixed type: these are sklearn's tagged unions, where an integer
	// means a count and a float a fraction. SQL literal types carry that
	// distinction already, so the parsers dispatch on what the user wrote.
	AddNamedParameter(fn, "max_features", LogicalType::ANY);
	AddNamedParameter(fn, "min_samples_split", LogicalType::ANY);
	AddNamedParameter(fn, "min_samples_leaf", LogicalType::ANY);
	AddNamedParameter(fn, "max_samples", LogicalType::ANY);
}

void ValidateForestParameters(const sc_rf_params_t &params, const char *caller) {
	// sklearn rejects max_samples without bootstrap rather than silently ignoring
	// it; sc has no opinion, so catch it here where the message can be specific.
	if (!params.bootstrap && params.max_samples.kind != SC_MAX_SAMPLES_ALL) {
		throw InvalidInputException("%s: max_samples is only meaningful with bootstrap := true", caller);
	}
}

//! Work out which metadata column holds the target.
//!
//! With one candidate there is nothing to choose, so choose it -- `(sample_id,
//! month)` needs no configuration. With several the answer is unknown, and
//! guessing would train on the wrong variable without any error: a
//! `(sample_id, age, bmi, delivery_mode)` table has three plausible targets.
//!
//! A named column is checked here too, so a typo fails when the query is
//! planned rather than after the data has been scanned.
std::string ResolveTargetColumn(Connection &conn, const std::string &relation, const std::string &requested,
                                const char *caller) {
	const auto q = SQLIdentifier::ToString(relation);
	// LIMIT 0 binds the relation and returns its schema without scanning it.
	auto probe = conn.Query("SELECT * FROM " + q + " LIMIT 0");
	if (probe->HasError()) {
		throw InvalidInputException("%s: metadata relation '%s' could not be read: %s", caller, relation,
		                            probe->GetError());
	}

	std::vector<std::string> candidates;
	bool has_sample_id = false;
	for (const auto &name : probe->GetNames()) {
		if (name == kSampleIdColumn) {
			has_sample_id = true;
			continue; // if sample_id, dont push to candidates
		}
		candidates.push_back(name.GetIdentifierName());
	}
	if (!has_sample_id) {
		throw InvalidInputException("%s: metadata relation '%s' has no 'sample_id' column", caller, relation);
	}
	if (candidates.empty()) {
		throw InvalidInputException(
		    "%s: metadata relation '%s' has only a sample_id column; it needs a target column too", caller, relation);
	}
	std::string list;
	for (size_t i = 0; i < candidates.size(); i++) {
		list += (i ? ", " : "") + ("'" + candidates[i] + "'");
	}
	if (!requested.empty()) {
		for (const auto &c : candidates) {
			if (StringUtil::CIEquals(c, requested)) {
				return requested;
			}
		}
		throw InvalidInputException("%s: metadata relation '%s' has no column '%s' to use as the target.\n"
		                            "  Candidates : [%s]",
		                            caller, relation, requested, list);
	}
	if (candidates.size() == 1) {
		return candidates[0];
	}
	throw InvalidInputException(
	    "%s: Metadata relation '%s' has more than one column that could be the target.\n"
	    "  Candidates : [%s]\n\n"
	    "Remedy:\n"
	    "  Pass target_column := 'one_of_the_above' to %s, so the wrong one cannot be picked silently.\n"
	    "  A metadata relation with exactly one non-sample_id column needs no target_column at all.",
	    caller, relation, list, caller);
}

} // namespace sc_rf
} // namespace duckdb
