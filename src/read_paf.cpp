#include "read_paf.hpp"

#include "remote_file_helper.hpp"
#include "table_function_common.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <climits>
#include <thread>

namespace duckdb {

namespace {

enum Column : idx_t {
	READ_ID,
	QUERY_LENGTH,
	QUERY_START,
	QUERY_STOP,
	STRAND,
	REFERENCE,
	REFERENCE_LENGTH,
	POSITION,
	STOP_POSITION,
	RESIDUE_MATCHES,
	BLOCK_LENGTH,
	MAPQ,
	TAGS,
	FILEPATH
};

void SetString(Vector &vec, idx_t row, const std::string &value) {
	FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, value);
}

void SetTags(Vector &map_vec, idx_t row, const std::vector<miint::SamStyleTag> &tags) {
	auto offset = ListVector::GetListSize(map_vec);
	ListVector::Reserve(map_vec, offset + tags.size());
	auto &keys = MapVector::GetKeys(map_vec);
	auto &values = MapVector::GetValues(map_vec);
	for (size_t i = 0; i < tags.size(); i++) {
		SetString(keys, offset + i, tags[i].key);
		SetString(values, offset + i, tags[i].value);
	}
	auto &entry = FlatVector::GetData<list_entry_t>(map_vec)[row];
	entry.offset = offset;
	entry.length = tags.size();
	ListVector::SetListSize(map_vec, offset + tags.size());
}

void WriteRecord(const miint::PafRecord &r, DataChunk &output, idx_t row) {
	SetString(output.data[READ_ID], row, r.read_id);
	FlatVector::GetData<int64_t>(output.data[QUERY_LENGTH])[row] = r.query_length;
	FlatVector::GetData<int64_t>(output.data[QUERY_START])[row] = r.query_start;
	FlatVector::GetData<int64_t>(output.data[QUERY_STOP])[row] = r.query_stop;
	SetString(output.data[STRAND], row, r.strand);
	SetString(output.data[REFERENCE], row, r.reference);
	FlatVector::GetData<int64_t>(output.data[REFERENCE_LENGTH])[row] = r.reference_length;
	FlatVector::GetData<int64_t>(output.data[POSITION])[row] = r.position;
	FlatVector::GetData<int64_t>(output.data[STOP_POSITION])[row] = r.stop_position;
	FlatVector::GetData<int64_t>(output.data[RESIDUE_MATCHES])[row] = r.residue_matches;
	FlatVector::GetData<int64_t>(output.data[BLOCK_LENGTH])[row] = r.block_length;
	FlatVector::GetData<uint8_t>(output.data[MAPQ])[row] = r.mapq;
	SetTags(output.data[TAGS], row, r.tags);
}

} // namespace

ReadPafTableFunction::OpenFile::OpenFile(FileSystem &fs, const std::string &path_p)
    : path(path_p), stream(CreateDuckDBSeqStream(fs, path_p), miint::duckdb_seq_close),
      lines([raw = stream.get(), path_p](char *dst, size_t len) -> size_t {
	      miint::g_seq_read_error.clear();
	      int n = miint::duckdb_seq_read(raw, dst, static_cast<unsigned int>(std::min<size_t>(len, INT_MAX)));
	      if (n < 0) {
		      throw IOException("read_paf: " + path_p + ": " + miint::g_seq_read_error);
	      }
	      return static_cast<size_t>(n);
      }) {
}

unique_ptr<FunctionData> ReadPafTableFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<std::string> &names) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto data = make_uniq<Data>();

	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("read_paf: first argument cannot be NULL");
	}
	if (input.inputs[0].type().id() == LogicalTypeId::VARCHAR) {
		data->file_paths = ExpandGlobPatternWithInfo(fs, context, input.inputs[0].ToString()).paths;
	} else if (input.inputs[0].type().id() == LogicalTypeId::LIST) {
		for (const auto &child : ListValue::GetChildren(input.inputs[0])) {
			if (child.IsNull()) {
				throw InvalidInputException("read_paf: file path list cannot contain NULL");
			}
			data->file_paths.push_back(child.ToString());
		}
		if (data->file_paths.empty()) {
			throw InvalidInputException("read_paf: at least one file path must be provided");
		}
	} else {
		throw InvalidInputException("read_paf: first argument must be VARCHAR or VARCHAR[]");
	}
	for (const auto &path : data->file_paths) {
		if (IsStdinPath(path)) {
			throw InvalidInputException("read_paf: stdin is not supported");
		}
		if (!miint::RemoteFileHelper::IsRemotePath(path) && !fs.FileExists(path)) {
			throw IOException("File not found: " + path);
		}
	}
	data->include_filepath = ParseIncludeFilepathParameter(input.named_parameters);

	names = {"read_id",  "query_length",  "query_start",     "query_stop",   "strand", "reference", "reference_length",
	         "position", "stop_position", "residue_matches", "block_length", "mapq",   "tags"};
	return_types = {LogicalType::VARCHAR,
	                LogicalType::BIGINT,
	                LogicalType::BIGINT,
	                LogicalType::BIGINT,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::BIGINT,
	                LogicalType::BIGINT,
	                LogicalType::BIGINT,
	                LogicalType::BIGINT,
	                LogicalType::BIGINT,
	                LogicalType::UTINYINT,
	                LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)};
	if (data->include_filepath) {
		names.emplace_back("filepath");
		return_types.emplace_back(LogicalType::VARCHAR);
	}
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> ReadPafTableFunction::InitGlobal(ClientContext &context,
                                                                      TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<Data>();
	idx_t threads = std::max<idx_t>(1, std::min<idx_t>(data.file_paths.size(), std::thread::hardware_concurrency()));
	return make_uniq<GlobalState>(threads);
}

unique_ptr<LocalTableFunctionState> ReadPafTableFunction::InitLocal(ExecutionContext &context,
                                                                    TableFunctionInitInput &input,
                                                                    GlobalTableFunctionState *global_state) {
	return make_uniq<LocalState>();
}

void ReadPafTableFunction::Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<Data>();
	auto &global = data_p.global_state->Cast<GlobalState>();
	auto &local = data_p.local_state->Cast<LocalState>();

	idx_t row = 0;
	while (row < STANDARD_VECTOR_SIZE) {
		if (!local.file) {
			// A chunk holds rows from one file only, so its filepath column is constant.
			if (row > 0) {
				break;
			}
			size_t idx;
			{
				std::lock_guard<std::mutex> guard(global.lock);
				if (global.next_file_idx >= data.file_paths.size()) {
					break;
				}
				idx = global.next_file_idx++;
			}
			local.file = make_uniq<OpenFile>(FileSystem::GetFileSystem(context), data.file_paths[idx]);
		}

		std::string_view line;
		if (!local.file->lines.Next(line)) {
			if (data.include_filepath && row > 0) {
				SetResultVectorFilepath(output.data[FILEPATH], local.file->path);
			}
			local.file.reset();
			continue;
		}
		try {
			local.parser.Parse(line, local.record);
		} catch (const std::runtime_error &e) {
			throw InvalidInputException("read_paf: " + local.file->path + ": line " +
			                            std::to_string(local.file->lines.LineNumber()) + ": " + e.what());
		}
		WriteRecord(local.record, output, row);
		row++;
	}
	if (data.include_filepath && row > 0 && local.file) {
		SetResultVectorFilepath(output.data[FILEPATH], local.file->path);
	}
	output.SetCardinality(row);
}

void ReadPafTableFunction::Register(ExtensionLoader &loader) {
	TableFunction tf("read_paf", {LogicalType::ANY}, Execute, Bind, InitGlobal, InitLocal);
	tf.named_parameters["include_filepath"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(tf);
}

} // namespace duckdb
