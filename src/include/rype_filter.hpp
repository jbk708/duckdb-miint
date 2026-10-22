#pragma once
//
// rype_filter(relation, index_path, threshold := 0.1): the rows of `relation`
// that RYpe does not match, every column as the relation has it.
//
// It is the host-depletion step
//
//   SELECT * FROM reads ANTI JOIN rype_classify(index, 'reads', threshold := t) USING (read_id)
//
// in one pass: the relation is read once, by a filter stream
// (rype_input_stream.hpp), in groups of rows. Each group is one RYpe call; the
// rows RYpe returns no hit for are emitted from the chunks the stream held for
// them. No read_id is needed, and nothing is read a second time to join on it.

#include "rype.h"
#include "rype_input_stream.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <string>

namespace duckdb {

class RypeFilterTableFunction {
public:
	struct Data : public TableFunctionData {
		std::string relation;
		std::string index_path;
		double threshold = 0.1;
		//! The relation's columns at bind: the output schema, and what the scan projects.
		vector<string> column_names;
		vector<LogicalType> column_types;
	};

	struct GlobalState : public GlobalTableFunctionState {
		RypeIndex *index = nullptr;
		//! The connection the scan runs on. The stream's streaming result points
		//! into its context, so it outlives input_stream.
		unique_ptr<Connection> input_connection;
		unique_ptr<RypeInputStream> input_stream;
		double threshold = 0.1;
		idx_t group_rows = 0;

		//! The group being emitted: its runs, the next run to emit, the Arrow id of
		//! its first row, and which of its rows RYpe matched (by id - group_first_id).
		vector<RypeHeldRows> runs;
		idx_t next_run = 0;
		idx_t group_first_id = 0;
		vector<bool> matched;
		//! The Arrow id the next group starts at: the rows sent so far.
		idx_t next_first_id = 0;
		bool done = false;

		idx_t MaxThreads() const override {
			return 1;
		}

		~GlobalState();
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<Identifier> &names);

	static unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input);

	static void Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);

	static TableFunction GetFunction();
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
