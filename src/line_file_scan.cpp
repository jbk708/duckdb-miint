#include "line_file_scan.hpp"

#include "remote_file_helper.hpp"

#include <algorithm>
#include <climits>
#include <thread>

namespace duckdb {

unique_ptr<LineScanData> BindLineScan(ClientContext &context, TableFunctionBindInput &input,
                                      const std::string &function_name) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto data = make_uniq<LineScanData>();
	const auto &arg = input.inputs[0];

	if (arg.IsNull()) {
		throw InvalidInputException(function_name + ": first argument cannot be NULL");
	}
	if (arg.type().id() == LogicalTypeId::VARCHAR) {
		data->file_paths = ExpandGlobPatternWithInfo(fs, context, arg.ToString()).paths;
	} else if (arg.type().id() == LogicalTypeId::LIST) {
		for (const auto &child : ListValue::GetChildren(arg)) {
			if (child.IsNull()) {
				throw InvalidInputException(function_name + ": file path list cannot contain NULL");
			}
			data->file_paths.push_back(child.ToString());
		}
		if (data->file_paths.empty()) {
			throw InvalidInputException(function_name + ": at least one file path must be provided");
		}
	} else {
		throw InvalidInputException(function_name + ": first argument must be VARCHAR or VARCHAR[]");
	}
	for (const auto &path : data->file_paths) {
		if (IsStdinPath(path)) {
			throw InvalidInputException(function_name + ": stdin is not supported");
		}
		if (!miint::RemoteFileHelper::IsRemotePath(path) && !fs.FileExists(path)) {
			throw IOException("File not found: " + path);
		}
	}
	data->include_filepath = ParseIncludeFilepathParameter(input.named_parameters);
	return data;
}

unique_ptr<GlobalTableFunctionState> InitLineScanGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<LineScanData>();
	idx_t threads = std::max<idx_t>(1, std::min<idx_t>(data.file_paths.size(), std::thread::hardware_concurrency()));
	return make_uniq<LineScanGlobalState>(threads);
}

LineFile::LineFile(FileSystem &fs, const std::string &path_p, const std::string &function_name)
    : path(path_p), stream(CreateDuckDBSeqStream(fs, path_p), miint::duckdb_seq_close),
      lines([raw = stream.get(), prefix = function_name + ": " + path_p + ": "](char *dst, size_t len) -> size_t {
	      miint::g_seq_read_error.clear();
	      int n = miint::duckdb_seq_read(raw, dst, static_cast<unsigned int>(std::min<size_t>(len, INT_MAX)));
	      if (n < 0) {
		      throw IOException(prefix + miint::g_seq_read_error);
	      }
	      return static_cast<size_t>(n);
      }) {
}

void SetTagMap(Vector &map_vec, idx_t row, const std::vector<miint::SamStyleTag> &tags) {
	auto offset = ListVector::GetListSize(map_vec);
	ListVector::Reserve(map_vec, offset + tags.size());
	auto &keys = MapVector::GetKeys(map_vec);
	auto &values = MapVector::GetValues(map_vec);
	for (size_t i = 0; i < tags.size(); i++) {
		FlatVector::GetData<string_t>(keys)[offset + i] = StringVector::AddString(keys, tags[i].key);
		FlatVector::GetData<string_t>(values)[offset + i] = StringVector::AddString(values, tags[i].value);
	}
	auto &entry = FlatVector::GetData<list_entry_t>(map_vec)[row];
	entry.offset = offset;
	entry.length = tags.size();
	ListVector::SetListSize(map_vec, offset + tags.size());
}

} // namespace duckdb
