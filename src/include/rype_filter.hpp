#pragma once
//
// rype_filter(relation, index_path[, minimap2_index_path], threshold := 0.1):
// the rows of `relation` that RYpe does not match and, given a minimap2 index,
// that minimap2 maps neither mate of, every column as the relation has it.
//
// It is the host-depletion steps
//
//   SELECT * FROM reads ANTI JOIN rype_classify(index, 'reads', threshold := t) USING (read_id)
//   ... ANTI JOIN align_minimap2(<what RYpe kept>, index_path := minimap2_index_path) USING (read_id)
//
// in one pass: the relation is read once, by a filter stream
// (rype_input_stream.hpp), in groups of rows. Each group is one RYpe call. The
// rows RYpe returns no hit for are read from the chunks the stream held for
// them, mapped by minimap2 there, and the rows it does not map are emitted from
// those chunks. RYpe needs no read_id, and nothing is read a second time to join
// on it. minimap2 runs only on short reads (see LONG_READ_MEAN_LENGTH in
// rype_filter.cpp), and is given each read's read_id as its name, as
// align_minimap2 gives it, so that it breaks ties between equal hits alike.

#include "Minimap2Aligner.hpp"
#include "rype.h"
#include "rype_input_stream.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <memory>
#include <string>

namespace duckdb {

class RypeFilterTableFunction {
public:
	struct Data : public TableFunctionData {
		std::string relation;
		std::string index_path;
		//! Empty when no minimap2 index was given.
		std::string minimap2_index_path;
		double threshold = 0.1;
		//! The relation's columns at bind: the output schema, and what the scan projects.
		vector<string> column_names;
		vector<LogicalType> column_types;
		//! Where minimap2's inputs are among them. sequence2 and read_id are optional.
		idx_t sequence1_column = DConstants::INVALID_INDEX;
		idx_t sequence2_column = DConstants::INVALID_INDEX;
		idx_t read_id_column = DConstants::INVALID_INDEX;
	};

	struct GlobalState : public GlobalTableFunctionState {
		RypeIndex *index = nullptr;
		//! The connection the scan runs on. The stream's streaming result points
		//! into its context, so it outlives input_stream.
		unique_ptr<Connection> input_connection;
		unique_ptr<RypeInputStream> input_stream;
		double threshold = 0.1;
		idx_t group_rows = 0;
		//! Set when minimap2 runs, which is when the reads are short. One aligner, as
		//! there is one thread.
		std::shared_ptr<miint::SharedMinimap2Index> minimap2_index;
		unique_ptr<miint::Minimap2Aligner> aligner;

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

	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
