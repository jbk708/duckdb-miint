#pragma once

#include "line_file_scan.hpp"
#include "paf_parser.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// read_paf(path): one row per PAF record, coordinates in miint's convention
// (docs/reading.md, "Coordinate conventions"), minimap2's tags as typed columns.
class ReadPafTableFunction {
public:
	// The current record's value for one typed tag column.
	struct TypedTagValue {
		bool present = false;
		int64_t integer = 0;
		double real = 0;
		std::string text;
	};

	struct LocalState : public LineScanLocalState {
		miint::PafParser parser;
		miint::PafRecord record;
		// One per typed tag column, in column order; the record's other tags stay in record.tags.
		std::vector<TypedTagValue> typed_tags;
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<std::string> &names);
	static unique_ptr<LocalTableFunctionState> InitLocal(ExecutionContext &context, TableFunctionInitInput &input,
	                                                     GlobalTableFunctionState *global_state);
	static void Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);

	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
