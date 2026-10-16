#include "sc_cross_validate_function.hpp"

#include "catalog_utils.hpp"
#include "id_column_utils.hpp"
#include "sc_common.hpp"
#include "unifrac_function_common.hpp"
#include "sc_coo_builder.hpp"
#include "sc_rf_common.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"

#include <memory>
#include <string>
#include <vector>

#include "sc.h"

namespace duckdb {

namespace {

// Everything shared with fit -- input scanning, validation, forest-parameter
// parsing -- lives in sc_rf_common.
using sc_rf::ApplyDefaults;
using sc_rf::ParseForestParameter;
using sc_rf::RegisterTrainingParameters;
using sc_rf::ResolveTargetColumn;
using sc_rf::ScanTrainingInput;
using sc_rf::ScTrainingInput;
using sc_rf::ValidateForestParameters;

//! q2's `cv` default, and sklearn's for cross_val_score.
constexpr int64_t kDefaultCvFolds = 5;

struct ScCvData : public TableFunctionData, public ScTrainingInput {
	bool classification = false;
	//! sc's thread count, resolved from `threads` at bind.
	int32_t threads = 1;
	sc_rf_params_t params {};
};

struct ScCvGlobalState : public GlobalTableFunctionState {
	std::vector<std::string> sample_ids;
	//! OOF prediction per sample, in sample_ids order. One of the two is filled.
	std::vector<std::string> labels;
	std::vector<double> values;
	//! The targets the folds were scored against, same order. Already read to
	//! hand to sc, so carrying them costs nothing and spares every caller a join
	//! back to the metadata relation to score or plot the result.
	std::vector<std::string> actual_labels;
	std::vector<double> actual_values;
	//! OOF class probabilities, row-major n_samples x n_classes, in `classes`
	//! order. Empty for a regressor. Flat rather than one Value per sample so the
	//! rows are built only as they are emitted.
	std::vector<double> proba;
	size_t n_classes = 0;
	//! Constant for the whole run, so every row repeats them.
	Value classes;
	Value fold_scores;
	double mean_score = 0;
	double std_score = 0;

	size_t cursor = 0;
	bool done_setup = false;
};

unique_ptr<FunctionData> ScCvBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<string> &names, bool classification) {
	auto data = make_uniq<ScCvData>();
	data->caller = classification ? "cross_validate_classifier" : "cross_validate_regressor";
#ifdef __EMSCRIPTEN__
	// DuckDB-Wasm's Emscripten runtime mis-links one call cross-validation makes
	// (a function-signature trap), and the obvious workaround returns wrong
	// scores without an error. Refused here until #315 lands.
	throw NotImplementedException("%s is not yet available in DuckDB-Wasm "
	                              "(https://github.com/the-miint/duckdb-miint/issues/315). Run it in native DuckDB; "
	                              "fit_*, predict, predict_proba and shap work here.",
	                              data->caller);
#endif
	data->classification = classification;
	data->data_relation = input.inputs[0].GetValue<string>();
	data->metadata_relation = input.inputs[1].GetValue<string>();
	RejectCTERelationName(input, data->data_relation);
	RejectCTERelationName(input, data->metadata_relation);
	if (data->data_relation.empty() || data->metadata_relation.empty()) {
		throw InvalidInputException("%s: data and metadata relation names must not be empty", data->caller);
	}

	ApplyDefaults(data->params, classification);
	auto &params = data->params;
	// `cv` is what q2, sklearn's cross_val_score and sc's own C struct call this,
	// so it keeps working; `n_folds` says what the number actually is. Accepting
	// both but never silently resolving a disagreement.
	bool n_folds_given = false;
	bool cv_given = false;
	params.cv = kDefaultCvFolds;
	int32_t requested_threads = 0;
	for (auto &kv : input.named_parameters) {
		const auto &k = kv.first;
		const auto &v = kv.second;
		miint::RequireNotNull(k, v, data->caller);
		if (ParseForestParameter(k, v, classification, data->caller, params)) {
			continue;
		}
		if (StringUtil::CIEquals(k, "target_column")) {
			data->target_column = v.GetValue<string>();
		} else if (StringUtil::CIEquals(k, "n_folds") || StringUtil::CIEquals(k, "cv")) {
			(StringUtil::CIEquals(k, "cv") ? cv_given : n_folds_given) = true;
			params.cv = v.GetValue<int64_t>();
			// One fold has nothing to hold out: the model would be scored on the
			// data it was fit on, which is the thing cross-validation exists to
			// avoid.
			if (params.cv < 2) {
				throw InvalidInputException("%s: n_folds must be >= 2 (got %lld)", data->caller, (int64_t)params.cv);
			}
		} else if (StringUtil::CIEquals(k, "parameter_tuning")) {
			params.parameter_tuning = v.GetValue<bool>();
		} else if (StringUtil::CIEquals(k, "threads")) {
			requested_threads = v.GetValue<int32_t>();
		}
	}
	data->threads = unifrac_internal::ResolveThreadsParameter(context, requested_threads, data->caller);
	if (n_folds_given && cv_given) {
		throw InvalidInputException("%s: pass n_folds or cv, not both -- cv is the q2-sample-classifier (QIIME 2) and "
		                            "sklearn spelling of n_folds",
		                            data->caller);
	}
	{
		auto conn = MakeReadOnlyHelperConnection(context);
		data->target_column = ResolveTargetColumn(conn, data->metadata_relation, data->target_column, data->caller);
	}
	ValidateForestParameters(params, data->caller);

	// No model comes out, so there is nothing to name and nothing to store: the
	// per-fold forests exist only to be scored. The prediction column follows the
	// task, exactly as in predict.
	{
		auto conn = MakeReadOnlyHelperConnection(context);
		data->id_types = miint::ProbeIdTypes(conn, data->data_relation, data->caller);
	}
	names = {"sample_id", "prediction", "actual"};
	const auto target_type = classification ? LogicalType::VARCHAR : LogicalType::DOUBLE;
	return_types = {data->id_types.sample_id, target_type, target_type};
	if (classification) {
		// Positional, not a map: probabilities[i] is the probability of classes[i],
		// and `classes` is the same list on every row, so a whole column reads as
		// one matrix. The regressor has no classes to be probable, and the task is
		// fixed by which function was called, so its schema simply lacks these
		// rather than carrying two always-NULL columns.
		names.push_back("probabilities");
		return_types.push_back(LogicalType::LIST(LogicalType::DOUBLE));
		names.push_back("classes");
		return_types.push_back(LogicalType::LIST(LogicalType::VARCHAR));
	}
	for (const auto &n : {"metric", "mean_score", "std_score", "fold_scores"}) {
		names.push_back(n);
	}
	return_types.push_back(LogicalType::VARCHAR);
	return_types.push_back(LogicalType::DOUBLE);
	return_types.push_back(LogicalType::DOUBLE);
	return_types.push_back(LogicalType::LIST(LogicalType::DOUBLE));
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> ScCvInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ScCvGlobalState>();
}

//! Scan both relations, run the folds, and keep what the rows need.
void RunCrossValidation(ClientContext &context, const ScCvData &bind, ScCvGlobalState &gstate) {
	auto conn = MakeReadOnlyHelperConnection(context);

	miint::OwnedArrowArray targets;
	std::unique_ptr<miint::ScCooTable> table;
	if (bind.classification) {
		table = ScanTrainingInput(context, conn, bind, gstate.actual_labels);
		miint::ExportUtf8(*targets.array(), *targets.schema(), gstate.actual_labels);
	} else {
		table = ScanTrainingInput(context, conn, bind, gstate.actual_values);
		miint::ExportFloat64(*targets.array(), *targets.schema(), gstate.actual_values);
	}
	gstate.sample_ids = table->SampleIds();

	const auto n_samples = gstate.sample_ids.size();
	// Caught here rather than in sc, where the message would name a matrix rather
	// than the relation the user passed.
	if (static_cast<size_t>(bind.params.cv) > n_samples) {
		throw InvalidInputException("%s: n_folds is %lld but '%s' has only %llu samples; a fold cannot be empty",
		                            bind.caller, (int64_t)bind.params.cv, bind.data_relation, (uint64_t)n_samples);
	}

	miint::ScContext ctx;
	miint::OpenScContext(bind.threads, bind.caller, ctx);

	sc_cv_result_t res {};
	const auto st = sc_cross_validate(ctx.ptr, table->get(), targets.array(), targets.schema(), &bind.params, &res);
	// Every field, including the probabilities and classes a regressor's rows do
	// not use: unclaimed arrays would leak.
	miint::OwnedArrowArray predictions, proba, classes, fold_scores;
	miint::TakeArray(res.predictions, res.predictions_schema, predictions);
	miint::TakeArray(res.proba, res.proba_schema, proba);
	miint::TakeArray(res.classes, res.classes_schema, classes);
	miint::TakeArray(res.fold_scores, res.fold_scores_schema, fold_scores);
	if (st != SC_OK) {
		miint::ThrowSc(bind.caller, ctx.ptr, st);
	}

	if (bind.classification) {
		gstate.labels = predictions.ReadUtf8("sc_cross_validate predictions");
		// The OOF probability behind each prediction, which is what ROC curves,
		// calibration and "which samples was it least sure about" need. sc aligns
		// the class array to the probability columns; that order is carried into
		// every row rather than re-derived here.
		const auto class_labels = classes.ReadUtf8("sc_cross_validate classes");
		gstate.n_classes = class_labels.size();
		gstate.proba =
		    miint::ReadProbaMatrix(proba, gstate.n_classes, gstate.sample_ids.size(), "sc_cross_validate proba");
		duckdb::vector<Value> class_values;
		class_values.reserve(class_labels.size());
		for (const auto &c : class_labels) {
			class_values.push_back(Value(c));
		}
		gstate.classes = Value::LIST(LogicalType::VARCHAR, std::move(class_values));
	} else {
		gstate.values = predictions.ReadFloat64("sc_cross_validate predictions");
	}
	const auto scores = fold_scores.ReadFloat64("sc_cross_validate fold_scores");
	const auto n_predictions = bind.classification ? gstate.labels.size() : gstate.values.size();
	if (n_predictions != n_samples || scores.size() != static_cast<size_t>(bind.params.cv)) {
		throw InternalException("sc_cross_validate: %llu predictions and %llu fold scores for %llu samples over "
		                        "%lld folds",
		                        (uint64_t)n_predictions, (uint64_t)scores.size(), (uint64_t)n_samples,
		                        (int64_t)bind.params.cv);
	}
	duckdb::vector<Value> fold_values;
	fold_values.reserve(scores.size());
	for (const auto s : scores) {
		fold_values.push_back(Value::DOUBLE(s));
	}
	gstate.fold_scores = Value::LIST(LogicalType::DOUBLE, std::move(fold_values));
	gstate.mean_score = res.mean_score;
	gstate.std_score = res.std_score;
}

void ScCvExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &gstate = input.global_state->Cast<ScCvGlobalState>();
	const auto &bind = input.bind_data->Cast<ScCvData>();
	if (!gstate.done_setup) {
		gstate.done_setup = true;
		RunCrossValidation(context, bind, gstate);
	}

	const auto metric = bind.classification ? "accuracy" : "mse";
	const size_t total = gstate.sample_ids.size();
	idx_t n = 0;
	while (n < STANDARD_VECTOR_SIZE && gstate.cursor < total) {
		const auto i = gstate.cursor++;
		EmitIdCell(output.data[0], n, gstate.sample_ids[i], bind.id_types.sample_id);
		output.SetValue(1, n, bind.classification ? Value(gstate.labels[i]) : Value::DOUBLE(gstate.values[i]));
		output.SetValue(2, n,
		                bind.classification ? Value(gstate.actual_labels[i]) : Value::DOUBLE(gstate.actual_values[i]));
		// The probability columns exist only for a classifier, so everything after
		// them shifts by two.
		idx_t col = 3;
		if (bind.classification) {
			duckdb::vector<Value> row;
			row.reserve(gstate.n_classes);
			for (size_t c = 0; c < gstate.n_classes; c++) {
				row.push_back(Value::DOUBLE(gstate.proba[i * gstate.n_classes + c]));
			}
			output.SetValue(col++, n, Value::LIST(LogicalType::DOUBLE, std::move(row)));
			output.SetValue(col++, n, gstate.classes);
		}
		output.SetValue(col++, n, Value(metric));
		output.SetValue(col++, n, Value::DOUBLE(gstate.mean_score));
		output.SetValue(col++, n, Value::DOUBLE(gstate.std_score));
		output.SetValue(col, n, gstate.fold_scores);
		n++;
	}
	output.SetCardinality(n);
}

TableFunction MakeCvFunction(const char *name, table_function_bind_t bind) {
	TableFunction fn(name, {LogicalType::VARCHAR, LogicalType::VARCHAR}, ScCvExecute, bind, ScCvInitGlobal);
	RegisterTrainingParameters(fn);
	fn.named_parameters["n_folds"] = LogicalType::BIGINT;
	fn.named_parameters["cv"] = LogicalType::BIGINT;
	fn.named_parameters["parameter_tuning"] = LogicalType::BOOLEAN;
	return fn;
}

unique_ptr<FunctionData> ScCvClassifierBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	return ScCvBind(context, input, return_types, names, /*classification=*/true);
}
unique_ptr<FunctionData> ScCvRegressorBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	return ScCvBind(context, input, return_types, names, /*classification=*/false);
}

} // namespace

void ScCrossValidateFunction::Register(ExtensionLoader &loader) {
	loader.RegisterFunction(MakeCvFunction("cross_validate_classifier", ScCvClassifierBind));
	loader.RegisterFunction(MakeCvFunction("cross_validate_regressor", ScCvRegressorBind));
}

} // namespace duckdb
