#pragma once

#include "duckdb_seq_stream.hpp"
#include "line_splitter.hpp"
#include "paf_parser.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {

// read_paf(path): one row per PAF record, coordinates in miint's convention
// (docs/reading.md, "Coordinate conventions"). Files are read through DuckDB's file
// system, so gzip and remote paths share one code path; each file is read by one thread.
class ReadPafTableFunction {
public:
	struct Data : public TableFunctionData {
		std::vector<std::string> file_paths;
		bool include_filepath;
	};

	struct GlobalState : public GlobalTableFunctionState {
		std::mutex lock;
		size_t next_file_idx = 0;
		idx_t max_threads;

		explicit GlobalState(idx_t max_threads) : max_threads(max_threads) {
		}
		idx_t MaxThreads() const override {
			return max_threads;
		}
	};

	// The file a thread is reading. Destroyed at end of file, so open descriptors scale
	// with threads rather than with files.
	struct OpenFile {
		std::string path;
		miint::DuckDBSeqStreamHandle stream;
		miint::LineSplitter lines;

		OpenFile(FileSystem &fs, const std::string &path);
	};

	struct LocalState : public LocalTableFunctionState {
		std::unique_ptr<OpenFile> file;
		miint::PafParser parser;
		miint::PafRecord record;
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<std::string> &names);
	static unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input);
	static unique_ptr<LocalTableFunctionState> InitLocal(ExecutionContext &context, TableFunctionInitInput &input,
	                                                     GlobalTableFunctionState *global_state);
	static void Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);

	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
