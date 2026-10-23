#include "sc_predict_proba_function.hpp"

#include "catalog_utils.hpp"
#include "miint_log.hpp"
#include "id_column_utils.hpp"
#include "sc_common.hpp"
#include "sc_coo_builder.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/main/connection.hpp"

#include <string>
#include <vector>
#include "miint_named_parameter.hpp"

namespace duckdb {

namespace {

struct ScProbaData : public TableFunctionData {
	string data_relation;
	string model_relation;
	string model_name;
	miint::ScIdTypes id_types;
};

struct ScProbaGlobalState : public GlobalTableFunctionState {
	std::vector<std::string> sample_ids;
	std::vector<std::string> classes;
	std::vector<double> known_fraction;
	//! Row-major, n_samples * n_classes.
	std::vector<double> proba;
	//! One output row per (sample, class) pair.
	idx_t emitted = 0;
	bool loaded = false;
};

unique_ptr<FunctionData> ScProbaBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto data = make_uniq<ScProbaData>();
	data->data_relation = input.inputs[0].GetValue<string>();
	data->model_relation = input.inputs[1].GetValue<string>();
	RejectCTERelationName(input, data->data_relation);
	RejectCTERelationName(input, data->model_relation);
	if (data->data_relation.empty() || data->model_relation.empty()) {
		throw InvalidInputException("predict_proba: data and model relation names must not be empty");
	}
	for (auto &kv : input.named_parameters) {
		miint::RequireNotNull(kv.first.GetIdentifierName(), kv.second, "predict_proba");
		if (kv.first == "name") {
			data->model_name = kv.second.GetValue<string>();
		}
	}

	// Reject a regressor at bind, before any scanning. sc would refuse too, but
	// only after the whole COO had been marshalled.
	{
		auto conn = MakeReadOnlyHelperConnection(context);
		data->id_types = miint::ProbeIdTypes(conn, data->data_relation, "predict_proba");
		const auto header = miint::ReadModelHeader(conn, data->model_relation, data->model_name, "predict_proba", true);
		if (header.task != miint::kClassification) {
			throw InvalidInputException(
			    "predict_proba: The model in '%s' is a regressor, which has no classes to give "
			    "probabilities over.\n\n"
			    "Remedy:\n"
			    "  Use predict for a regressor -- it returns the predicted value directly:\n"
			    "    SELECT * FROM predict(%s, %s%s);",
			    data->model_relation, SQLString::ToString(data->data_relation),
			    SQLString::ToString(data->model_relation),
			    data->model_name.empty() ? "" : (", name := " + SQLString::ToString(data->model_name)).c_str());
		}
		miint::RequireModelFeatureIdType(header.feature_id_type, data->id_types.feature_id, "predict_proba",
		                                 data->model_relation, data->model_name, data->data_relation);
	}

	names = {"sample_id", "class", "probability", "known_feature_fraction"};
	return_types = {data->id_types.sample_id, LogicalType::VARCHAR, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> ScProbaInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ScProbaGlobalState>();
}

void ScProbaExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &gstate = input.global_state->Cast<ScProbaGlobalState>();
	const auto &bind = input.bind_data->Cast<ScProbaData>();

	if (!gstate.loaded) {
		gstate.loaded = true;
		auto conn = MakeReadOnlyHelperConnection(context);

		miint::ScContext ctx;
		// sc predicts serially, so the context needs no pool.
		miint::OpenScContext(0, "predict_proba", ctx);
		miint::ScModel model;
		miint::LoadModelFromRelation(conn, bind.model_relation, bind.model_name, "predict_proba",
		                             miint::kClassification, bind.id_types.feature_id, ctx.ptr, model);

		auto table = miint::ScanForModel(context, conn, model, bind.data_relation, "predict_proba",
		                                 " and are predicted from an all-zero row");
		gstate.sample_ids = table->SampleIds();
		gstate.known_fraction = table->KnownFeatureFraction();

		miint::OwnedArrowArray proba, classes;
		if (auto st = sc_predict_proba(ctx.ptr, model.ptr, table->get(), proba.array(), proba.schema(), classes.array(),
		                               classes.schema());
		    st != SC_OK) {
			miint::ThrowSc("sc_predict_proba", ctx.ptr, st);
		}
		gstate.classes = classes.ReadUtf8("sc_predict_proba classes");

		// One probability per class, in the order the classes array reports.
		gstate.proba =
		    miint::ReadProbaMatrix(proba, gstate.classes.size(), gstate.sample_ids.size(), "sc_predict_proba");
	}

	// One row per (sample, class): index i is sample i / n_classes, class
	// i % n_classes -- the same row-major order sc flattened them in.
	const idx_t n_classes = gstate.classes.size();
	const idx_t total = gstate.sample_ids.size() * n_classes;
	const idx_t remaining = total - gstate.emitted;
	const idx_t n = remaining < STANDARD_VECTOR_SIZE ? remaining : STANDARD_VECTOR_SIZE;
	output.SetChildCardinality(n);
	for (idx_t i = 0; i < n; i++) {
		const auto at = gstate.emitted + i;
		const auto sample = at / n_classes;
		const auto klass = at % n_classes;
		EmitIdCell(output.data[0], i, gstate.sample_ids[sample], bind.id_types.sample_id);
		output.data[1].SetValue(i, Value(gstate.classes[klass]));
		output.data[2].SetValue(i, Value::DOUBLE(gstate.proba[at]));
		output.data[3].SetValue(i, Value::DOUBLE(gstate.known_fraction[sample]));
	}
	gstate.emitted += n;
}

} // namespace

void ScPredictProbaFunction::Register(ExtensionLoader &loader) {
	TableFunction fn("predict_proba", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ScProbaExecute, ScProbaBind,
	                 ScProbaInitGlobal);
	AddNamedParameter(fn, "name", LogicalType::VARCHAR);
	loader.RegisterFunction(fn);
}

} // namespace duckdb
