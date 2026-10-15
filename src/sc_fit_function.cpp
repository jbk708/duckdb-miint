#include "sc_fit_function.hpp"

#include "catalog_utils.hpp"
#include "sc_coo_builder.hpp"
#include "sc_common.hpp"
#include "sc_rf_common.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/keyword_helper.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sc.h"

namespace duckdb {

namespace {

// Everything fit and cross-validate share -- input scanning, validation,
// forest-parameter parsing -- lives in sc_rf_common.
using sc_rf::ApplyDefaults;
using sc_rf::ParseForestParameter;
using sc_rf::ResolveTargetColumn;
using sc_rf::ScanTrainingInput;
using sc_rf::ScTrainingInput;
using sc_rf::ValidateForestParameters;

//! The two relations plus the forest parameters, resolved at bind time.
//!
//! Inherits ScTrainingInput for the relation names the shared helpers read.
struct ScFitData : public TableFunctionData, public ScTrainingInput {
	//! Required. A model is selected by name, so an unnamed one is reachable
	//! only by position or by whatever hyperparameters happen to differ.
	string name;
	//! The algorithm, echoed into the output so a row says what produced it.
	string model = "random_forest";
	bool classification = false;
	int32_t n_threads = 0;
	// Defaults are sklearn's RandomForest{Classifier,Regressor} defaults, so an
	// unparameterised call matches what a user would get from scikit-learn.
	// `criterion` and `max_features` differ by task, so they are resolved in
	// Bind once `classification` is known.
	sc_rf_params_t params {};
};

// ---------------------------------------------------------------------------
// Bind / Execute
// ---------------------------------------------------------------------------

unique_ptr<FunctionData> ScFitBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names, bool classification) {
	auto data = make_uniq<ScFitData>();
	data->classification = classification;
	data->caller = classification ? "fit_classifier" : "fit_regressor";
	data->data_relation = input.inputs[0].GetValue<string>();
	data->metadata_relation = input.inputs[1].GetValue<string>();
	RejectCTERelationName(input, data->data_relation);
	RejectCTERelationName(input, data->metadata_relation);
	if (data->data_relation.empty() || data->metadata_relation.empty()) {
		throw InvalidInputException("%s: data and metadata relation names must not be empty", data->caller);
	}

	ApplyDefaults(data->params, classification);
	auto &params = data->params;
	for (auto &kv : input.named_parameters) {
		const auto &k = kv.first;
		const auto &v = kv.second;
		if (v.IsNull()) {
			throw InvalidInputException("%s: named parameter '%s' must not be NULL", data->caller, k);
		}
		if (ParseForestParameter(k, v, classification, data->caller, params)) {
			continue;
		}
		if (StringUtil::CIEquals(k, "name")) {
			data->name = v.GetValue<string>();
			if (data->name.empty()) {
				throw InvalidInputException("%s: name must not be empty; every model needs one", data->caller);
			}
		} else if (StringUtil::CIEquals(k, "target_column")) {
			data->target_column = v.GetValue<string>();
		} else if (StringUtil::CIEquals(k, "n_threads")) {
			data->n_threads = miint::ParseNThreads(v, data->caller);
		} else if (StringUtil::CIEquals(k, "model")) {
			const auto m = v.GetValue<string>();
			if (!StringUtil::CIEquals(m, "random_forest")) {
				throw InvalidInputException("%s: model must be 'random_forest' (got '%s')", data->caller, m);
			}
			data->model = "random_forest"; // normalised, so the column is stable
		}
	}
	{
		auto conn = MakeReadOnlyHelperConnection(context);
		data->target_column = ResolveTargetColumn(conn, data->metadata_relation, data->target_column, data->caller);
	}

	// Required rather than optional. A name is not an input to the fit -- the
	// same seed with and without one produces a byte-identical model -- but
	// making it mandatory removes a state from the system: `name` is never NULL,
	// so `predict(..., name := ...)` always works and nobody has to reason
	// about the unnamed case. Optional names get skipped, and then a registry
	// needs ALTER TABLE and rowid archaeology to become selectable.
	if (data->name.empty()) {
		throw InvalidInputException(
		    "%s: name is required -- `name := 'my_model'`. It is how a model is selected later, e.g. "
		    "predict(data, models, name := 'my_model'). Any label will do if you are just exploring.",
		    data->caller);
	}

	ValidateForestParameters(params, data->caller);

	// `name` is required, so this column is never NULL and a registry is
	// selectable from the moment it is created. Nothing is ever invented to fill
	// it -- a generated 'm0' would collide on the very next call, since a table
	// function cannot see what already exists in the table its row is headed for.
	//
	// `random_state` is echoed because it is the one thing needed to reproduce a
	// fit that the caller would otherwise have to remember and re-type. Richer
	// provenance stays the caller's: `SELECT 'x' AS target, * FROM sc_fit_...`.
	// `model` names the algorithm and mirrors the `model :=` parameter; the
	// serialized forest lives in `model_blob`. They were one column called
	// `model`, which meant `model := 'random_forest'` went in and bytes came
	// out under the same name -- and left a row unable to say which algorithm
	// produced it once there is more than one.
	//
	// `feature_id_type` is the type the data's feature_id had. Applying the model
	// requires exactly that type, and the column says which one to provide.
	{
		auto conn = MakeReadOnlyHelperConnection(context);
		data->id_types = miint::ProbeIdTypes(conn, data->data_relation, data->caller);
	}
	names = {"name",         "model", "model_blob",   "n_samples",      "n_features",
	         "n_estimators", "task",  "random_state", "feature_id_type"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BLOB,
	                LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::BIGINT,
	                LogicalType::VARCHAR, LogicalType::BIGINT,  LogicalType::VARCHAR};
	return std::move(data);
}

struct ScFitGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<GlobalTableFunctionState> ScFitInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ScFitGlobalState>();
}

/*
    Entry point called by DuckDB execution engine to pull output chunks.
    receives query context, function input state wrappers, and destination chunk
    the output we write to. In this case a single row is produced, that is the fitted model.
*/
void ScFitExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	// global state to tell us if we are finished
	auto &gstate = input.global_state->Cast<ScFitGlobalState>();
	if (gstate.done) {
		output.SetCardinality(0); // an empty chunk ends the scan
		return;
	}
	// A fit returns one row, so the scan runs once.
	gstate.done = true;

	const auto &bind = input.bind_data->Cast<ScFitData>();
	auto conn = MakeReadOnlyHelperConnection(context);

	miint::OwnedArrowArray targets;
	std::unique_ptr<miint::ScCooTable> table;
	if (bind.classification) {
		std::vector<std::string> labels;
		table = ScanTrainingInput(context, conn, bind, labels);
		miint::ExportUtf8(*targets.array(), *targets.schema(), labels);
	} else {
		std::vector<double> values;
		table = ScanTrainingInput(context, conn, bind, values);
		miint::ExportFloat64(*targets.array(), *targets.schema(), std::move(values));
	}

	// Fully resolved at bind time, so a bad parameter fails the query before any
	// scanning happens.
	const sc_rf_params_t &params = bind.params;

	miint::ScContext ctx;
	miint::OpenScContext(context, bind.n_threads, bind.caller, ctx);

	// sc borrows the table and the targets for the call and frees neither; both
	// are released here on scope exit. Fit the model and serialize it into a blob.
	miint::ScModel model;
	const auto fit = bind.classification ? sc_fit_classifier : sc_fit_regressor;
	if (auto st = fit(ctx.ptr, table->get(), targets.array(), targets.schema(), &params, &model.ptr); st != SC_OK) {
		miint::ThrowSc(bind.classification ? "sc_fit_classifier" : "sc_fit_regressor", ctx.ptr, st);
	}

	uint8_t *blob = nullptr;
	size_t blob_len = 0;
	if (auto st = sc_model_serialize(model.ptr, &blob, &blob_len); st != SC_OK) {
		miint::ThrowSc("sc_model_serialize", nullptr, st);
	}
	// sc owns the buffer until sc_buffer_free, which the guard calls however this
	// scope exits -- including when the copy into DuckDB's heap throws.
	const auto free_blob = [blob_len](uint8_t *p) {
		sc_buffer_free(p, blob_len);
	};
	std::unique_ptr<uint8_t, decltype(free_blob)> blob_guard(blob, free_blob);
	duckdb::Value blob_value = Value::BLOB(blob, blob_len);

	output.SetCardinality(1);
	output.SetValue(0, 0, Value(bind.name));
	output.SetValue(1, 0, Value(bind.model));
	output.SetValue(2, 0, blob_value);
	output.SetValue(3, 0, Value::BIGINT(table->NumSamples()));
	output.SetValue(4, 0, Value::BIGINT(table->NumFeatures()));
	output.SetValue(5, 0, Value::BIGINT(params.n_estimators));
	output.SetValue(6, 0, Value(bind.classification ? "classification" : "regression"));
	output.SetValue(7, 0, Value::BIGINT(static_cast<int64_t>(params.random_state)));
	output.SetValue(8, 0, Value(bind.id_types.feature_id.ToString()));
}
/**
 * Creates a new table function for fitting a model based on sc rf
 */
TableFunction MakeFitFunction(const char *name, table_function_bind_t bind) {
	// declare a variable named fn and initialize an instance of the TableFunction class with the constructor call. The
	// TableFunction constructor takes the following parameters:
	TableFunction fn(name, {LogicalType::VARCHAR, LogicalType::VARCHAR}, ScFitExecute, bind, ScFitInitGlobal);
	fn.named_parameters["name"] = LogicalType::VARCHAR;
	fn.named_parameters["target_column"] = LogicalType::VARCHAR;
	fn.named_parameters["n_estimators"] = LogicalType::BIGINT;
	fn.named_parameters["random_state"] = LogicalType::BIGINT;
	fn.named_parameters["n_threads"] = LogicalType::INTEGER;
	fn.named_parameters["model"] = LogicalType::VARCHAR;
	fn.named_parameters["max_depth"] = LogicalType::BIGINT;
	fn.named_parameters["criterion"] = LogicalType::VARCHAR;
	fn.named_parameters["min_weight_fraction_leaf"] = LogicalType::DOUBLE;
	fn.named_parameters["min_impurity_decrease"] = LogicalType::DOUBLE;
	fn.named_parameters["bootstrap"] = LogicalType::BOOLEAN;
	// ANY, not a fixed type: these are sklearn's tagged unions, where an integer
	// means a count and a float means a fraction. SQL literal types carry that
	// distinction already, so ParseMaxFeatures / ParseMinSamples / ParseMaxSamples
	// dispatch on what the user wrote.
	fn.named_parameters["max_features"] = LogicalType::ANY;
	fn.named_parameters["min_samples_split"] = LogicalType::ANY;
	fn.named_parameters["min_samples_leaf"] = LogicalType::ANY;
	fn.named_parameters["max_samples"] = LogicalType::ANY;
	return fn;
}

unique_ptr<FunctionData> ScFitClassifierBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	return ScFitBind(context, input, return_types, names, /*classification=*/true);
}
unique_ptr<FunctionData> ScFitRegressorBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	return ScFitBind(context, input, return_types, names, /*classification=*/false);
}

} // namespace

void ScFitFunction::Register(ExtensionLoader &loader) {
	loader.RegisterFunction(MakeFitFunction("fit_classifier", ScFitClassifierBind));
	loader.RegisterFunction(MakeFitFunction("fit_regressor", ScFitRegressorBind));
}

} // namespace duckdb
