#include "rype_filter.hpp"
#include "catalog_utils.hpp"
#include "miint_log.hpp"
#include "miint_named_parameter.hpp"
#include "rype_common.hpp"

#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

namespace {

//! Read the session-scoped `miint_rype_filter_group_rows` setting: rows per
//! group, or 0 to size groups from the memory budget.
idx_t GetRypeFilterGroupRows(ClientContext &context) {
	Value v;
	// Registered as a BIGINT extension option in LoadInternal; missing means it
	// never was, which is a load-order programmer error.
	if (!context.TryGetCurrentSetting("miint_rype_filter_group_rows", v)) {
		throw InternalException(
		    "miint extension option 'miint_rype_filter_group_rows' is not registered — was LoadInternal completed?");
	}
	if (v.IsNull()) {
		return 0;
	}
	const auto rows = v.GetValue<int64_t>();
	if (rows < 0) {
		throw InvalidInputException(
		    "miint_rype_filter_group_rows must be >= 0 (0 sizes groups from the memory budget)");
	}
	return NumericCast<idx_t>(rows);
}

//! The most rows one group should carry within `budget` bytes.
//!
//! RYpe sizes its own batch B to the budget, at a cost of c = per_batch_memory / B
//! per read (the estimate is linear in B). A group costs that while RYpe
//! classifies it, and its rows are held meanwhile at h bytes each: every column,
//! not just the sequences. Up to two groups are held at once, one being
//! classified while the one before it is emitted, so a group of G rows needs
//! G * (c + 2h) of the B * c RYpe would have spent alone.
idx_t AutoGroupRows(ClientContext &context, const RypeIndex *index, const RypeInputStream::Sizing &sizing,
                    size_t budget) {
	const RypeBatchConfig config =
	    rype_calculate_batch_config(index, sizing.avg_read_length, sizing.is_paired ? 1 : 0, budget, 1);
	if (config.batch_size == 0) {
		const char *err = rype_get_last_error();
		miint::EmitWarning(context,
		                   "rype_filter: could not size a classification batch (%s); falling back to %llu reads",
		                   err ? err : "unknown error", static_cast<unsigned long long>(STANDARD_VECTOR_SIZE));
		return STANDARD_VECTOR_SIZE;
	}
	const double per_read = static_cast<double>(config.per_batch_memory) / static_cast<double>(config.batch_size);
	const double rows = static_cast<double>(config.per_batch_memory) /
	                    (per_read + 2.0 * static_cast<double>(sizing.held_bytes_per_row));
	return MaxValue<idx_t>(1, MinValue<idx_t>(static_cast<idx_t>(rows), config.batch_size));
}

//! Slice the rows of `run` that RYpe did not match into `output`. False when it
//! has none, leaving `output` untouched.
bool EmitRun(const RypeFilterTableFunction::GlobalState &gstate, const RypeHeldRows &run, DataChunk &output) {
	D_ASSERT(run.to - run.from <= STANDARD_VECTOR_SIZE);
	SelectionVector sel(run.to - run.from);
	idx_t count = 0;
	for (idx_t i = run.from; i < run.to; i++) {
		if (!gstate.matched[run.first_id + (i - run.from) - gstate.group_first_id]) {
			sel.set_index(count++, i);
		}
	}
	if (count == 0) {
		return false;
	}
	// The output references the held chunk's buffers, so the run can be dropped
	// once the next group begins.
	output.Slice(*run.chunk, sel, count);
	return true;
}

//! Classify the next group with one RYpe call and mark the rows it matched.
//! False when the scan has no rows left.
bool ClassifyNextGroup(RypeFilterTableFunction::GlobalState &gstate) {
	gstate.runs.clear();
	gstate.next_run = 0;
	auto &input = *gstate.input_stream;
	if (!input.BeginGroup()) {
		return false;
	}
	gstate.group_first_id = gstate.next_first_id;

	// The input stream stays owned by the GlobalState whatever RYpe does with it;
	// see the ownership note in rype_input_stream.hpp. The stream ends after one
	// group, so RYpe sees exactly the rows that group holds.
	ArrowArrayStreamWrapper output;
	if (rype_classify_arrow_ex(gstate.index, nullptr, &input.stream, gstate.threshold, gstate.group_rows,
	                           &output.arrow_array_stream) != 0) {
		const char *err = rype_get_last_error();
		throw IOException("RYpe classification failed: %s", err ? err : "unknown error");
	}
	ArrowSchemaWrapper schema;
	if (output.arrow_array_stream.get_schema(&output.arrow_array_stream, &schema.arrow_schema) != 0) {
		const char *err = output.arrow_array_stream.get_last_error(&output.arrow_array_stream);
		throw IOException("Failed to get RYpe output schema: %s", err ? err : "unknown error");
	}
	if (schema.arrow_schema.n_children < 1 || std::string(schema.arrow_schema.children[0]->format) != "l") {
		throw IOException("RYpe classify returned an unexpected schema: its first column is not an Int64 query_id");
	}

	// Only hits come back, a read once per bucket it reaches the threshold for.
	gstate.matched.assign(gstate.group_rows, false);
	idx_t matched_end = 0;
	for (;;) {
		ArrowArrayWrapper batch;
		if (output.arrow_array_stream.get_next(&output.arrow_array_stream, &batch.arrow_array) != 0) {
			const char *err = output.arrow_array_stream.get_last_error(&output.arrow_array_stream);
			throw IOException("Error getting next batch from RYpe: %s", err ? err : "unknown error");
		}
		if (!batch.arrow_array.release) {
			break;
		}
		const auto &query_ids = *batch.arrow_array.children[0];
		if (batch.arrow_array.length > 0 && !query_ids.buffers[1]) {
			throw IOException("Arrow array has null data buffer in RYpe classify output");
		}
		const auto ids = static_cast<const int64_t *>(query_ids.buffers[1]);
		for (int64_t r = 0; r < batch.arrow_array.length; r++) {
			const int64_t id = ids[r + batch.arrow_array.offset + query_ids.offset];
			// The ids are the surrogates this group was sent, so anything else means
			// RYpe echoed something it was not given.
			if (id < 0 || static_cast<idx_t>(id) < gstate.group_first_id ||
			    static_cast<idx_t>(id) - gstate.group_first_id >= gstate.group_rows) {
				throw IOException("RYpe returned query_id %lld, outside the group it was sent (ids %llu to %llu)",
				                  static_cast<long long>(id), static_cast<unsigned long long>(gstate.group_first_id),
				                  static_cast<unsigned long long>(gstate.group_first_id + gstate.group_rows - 1));
			}
			const idx_t row = static_cast<idx_t>(id) - gstate.group_first_id;
			gstate.matched[row] = true;
			matched_end = MaxValue<idx_t>(matched_end, row + 1);
		}
	}

	// RYpe has pulled the group to its end-of-stream, so every row it was sent is
	// held now.
	gstate.runs = input.TakeGroup();
	idx_t group_size = 0;
	for (const auto &run : gstate.runs) {
		group_size += run.to - run.from;
	}
	if (group_size == 0) {
		// The same rows would begin the next group, and this would never end.
		throw InternalException("rype_filter: RYpe finished a group without reading any of it");
	}
	D_ASSERT(gstate.runs.front().first_id == gstate.group_first_id);
	if (matched_end > group_size) {
		throw IOException("RYpe returned a query_id past the %llu rows of its group",
		                  static_cast<unsigned long long>(group_size));
	}
	gstate.next_first_id += group_size;
	return true;
}

} // namespace

// ============================================================================
// GlobalState destructor
// ============================================================================
RypeFilterTableFunction::GlobalState::~GlobalState() {
	// Every RYpe output stream was released inside the call that made it, so
	// nothing of RYpe's is left reading the input stream or the index.
	runs.clear();
	if (index) {
		rype_index_free(index);
	}
	// The stream holds the streaming result, which points into the connection's
	// context: stream first, connection last.
	input_stream.reset();
	input_connection.reset();
}

// ============================================================================
// Bind
// ============================================================================
unique_ptr<FunctionData> RypeFilterTableFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto data = make_uniq<Data>();
	data->relation = input.inputs[0].ToString();
	RejectCTERelationName(input, data->relation);
	data->index_path = input.inputs[1].ToString();

	auto threshold_param = input.named_parameters.find("threshold");
	if (threshold_param != input.named_parameters.end() && !threshold_param->second.IsNull()) {
		data->threshold = threshold_param->second.GetValue<double>();
		if (data->threshold < 0.0 || data->threshold > 1.0) {
			throw BinderException("threshold must be between 0.0 and 1.0");
		}
	}

	// The index path is left to rype_index_load in InitGlobal, as rype_classify
	// leaves it: it reports a missing or damaged index better than a check here.
	auto columns = GetTableColumns(context, data->relation);
	bool has_sequence1 = false;
	for (idx_t i = 0; i < columns.names.size(); i++) {
		const auto &name = columns.names[i];
		const bool is_sequence1 = StringUtil::CIEquals(name, "sequence1");
		if (!is_sequence1 && !StringUtil::CIEquals(name, "sequence2")) {
			continue;
		}
		has_sequence1 |= is_sequence1;
		const auto type = columns.types[i].id();
		if (type != LogicalTypeId::VARCHAR && type != LogicalTypeId::BLOB) {
			throw BinderException("Column '%s' in '%s' must be VARCHAR or BLOB, not %s", name, data->relation,
			                      columns.types[i].ToString());
		}
	}
	if (!has_sequence1) {
		throw BinderException("Table '%s' missing required column 'sequence1'", data->relation);
	}

	for (idx_t i = 0; i < columns.names.size(); i++) {
		names.emplace_back(columns.names[i]);
		return_types.push_back(columns.types[i]);
	}
	data->column_names = std::move(columns.names);
	data->column_types = std::move(columns.types);
	return data;
}

// ============================================================================
// InitGlobal
// ============================================================================
unique_ptr<GlobalTableFunctionState> RypeFilterTableFunction::InitGlobal(ClientContext &context,
                                                                         TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<Data>();
	auto gstate = make_uniq<GlobalState>();
	gstate->threshold = bind_data.threshold;
	const idx_t group_rows_setting = GetRypeFilterGroupRows(context);

	gstate->index = rype_index_load(bind_data.index_path.c_str());
	if (!gstate->index) {
		const char *err = rype_get_last_error();
		throw IOException("Failed to load RYpe index '%s': %s", bind_data.index_path, err ? err : "unknown error");
	}

	auto &db = DatabaseInstance::GetDatabase(context);
	gstate->input_connection = make_uniq<Connection>(db);
	InheritTempObjects(context, *gstate->input_connection);
	auto &conn = *gstate->input_connection;
	ConfigureRypeArrowExport(conn);

	// Let the scan run up to two Arrow batches ahead of RYpe, instead of DuckDB's
	// 10 MiB, so decoding the relation overlaps classifying it. The buffer is sized
	// when the stream opens, so this goes first.
	const idx_t batch_bytes = GetRypeArrowBatchBytes(context);
	const idx_t read_ahead_bytes = 2 * batch_bytes;
	if (read_ahead_bytes > 0) {
		auto result = conn.Query(StringUtil::Format("SET max_streaming_buffer_size = '%llu bytes'",
		                                            static_cast<unsigned long long>(read_ahead_bytes)));
		if (result->HasError()) {
			throw InvalidInputException("rype_filter could not set the scan's read-ahead: %s", result->GetError());
		}
	}

	RypeInputStreamOptions stream_options;
	stream_options.relation_quoted = SQLIdentifier::ToString(bind_data.relation);
	stream_options.source_name = bind_data.relation;
	stream_options.batch_bytes = batch_bytes;
	gstate->input_stream = BuildRypeFilterStream(conn, bind_data.column_names, std::move(stream_options));
	// The output schema was fixed at bind; a relation redefined since then would
	// hand rows of other types to a plan that expects these.
	if (gstate->input_stream->ScanTypes() != bind_data.column_types) {
		throw InvalidInputException("'%s' changed after the query was bound: its column types differ",
		                            bind_data.relation);
	}

	if (group_rows_setting > 0) {
		gstate->group_rows = group_rows_setting;
	} else {
		// From the head of the stream's own scan, which SampleSizing does not
		// consume: the relation is read once (#229).
		const auto sizing = gstate->input_stream->SampleSizing();
		// The read-ahead buffer is drawn from the same memory. 0 means detection
		// failed, which leaves RYpe to size against its own estimate.
		size_t budget = ResolveRypeMemoryBudget(context, 0);
		if (budget > 0) {
			const size_t floor = MinValue<size_t>(budget, RYPE_MIN_MEMORY_BUDGET);
			budget = MaxValue<size_t>(budget > read_ahead_bytes ? budget - read_ahead_bytes : 0, floor);
			if (budget <= RYPE_MIN_MEMORY_BUDGET) {
				miint::EmitWarning(context,
				                   "rype_filter: only %s of memory is free for classification, so groups are small and "
				                   "each costs a pass over the index; this can be many times slower than usual. Free "
				                   "memory, or run when the host is quieter.",
				                   StringUtil::BytesToHumanReadableString(budget));
			}
		}
		gstate->group_rows = AutoGroupRows(context, gstate->index, sizing, budget);
	}
	gstate->input_stream->SetGroupRows(gstate->group_rows);
	gstate->input_stream->SetBatchRows(gstate->group_rows);
	return gstate;
}

// ============================================================================
// Execute
// ============================================================================
void RypeFilterTableFunction::Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<GlobalState>();
	// No mutex: MaxThreads() is 1.
	while (!gstate.done) {
		if (gstate.next_run < gstate.runs.size()) {
			if (EmitRun(gstate, gstate.runs[gstate.next_run++], output)) {
				return;
			}
			continue;
		}
		// A RYpe call is the long step; let a cancelled query stop between them.
		context.InterruptCheck();
		if (!ClassifyNextGroup(gstate)) {
			gstate.runs.clear();
			gstate.done = true;
		}
	}
	output.SetChildCardinality(0);
}

// ============================================================================
// GetFunction / Register
// ============================================================================
TableFunction RypeFilterTableFunction::GetFunction() {
	TableFunction tf("rype_filter", {LogicalType::VARCHAR, LogicalType::VARCHAR}, Execute, Bind, InitGlobal);
	AddNamedParameter(tf, "threshold", LogicalType::DOUBLE);
	return tf;
}

void RypeFilterTableFunction::Register(ExtensionLoader &loader) {
	loader.RegisterFunction(GetFunction());
}

} // namespace duckdb
