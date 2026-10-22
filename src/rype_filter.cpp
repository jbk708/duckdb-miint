#include "rype_filter.hpp"
#include "rype_filter_schedule.hpp"
#include "catalog_utils.hpp"
#include "id_column_utils.hpp"
#include "miint_log.hpp"
#include "miint_named_parameter.hpp"
#include "rype_common.hpp"

#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace duckdb {

namespace {

//! A mean sequence1 length at or above this, sampled from the head of the scan,
//! is long reads, which minimap2's short-read preset is not for: rype_filter then
//! runs RYpe alone and never opens the minimap2 index.
constexpr size_t LONG_READ_MEAN_LENGTH = 300;

//! minimap2 as align_minimap2 runs it by default (the sr preset, five secondary
//! alignments), less what only shapes SAM rows. =/X CIGAR operations are written
//! after an alignment is scored, so they cannot change whether a read maps.
miint::Minimap2Config FilterMinimap2Config() {
	miint::Minimap2Config config;
	config.eqx = false;
	return config;
}

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

//! A thread that cannot park waits this long at most before looking again, so
//! that it notices an interrupted query.
constexpr auto WAIT_FOR_PROGRESS = std::chrono::milliseconds(10);

} // namespace

// ============================================================================
// Schedule
// ============================================================================

RypeFilterTableFunction::Schedule::~Schedule() {
	// Every RYpe output stream was released inside the call that made it, so
	// nothing of RYpe's is left reading the input stream or the index.
	units.clear();
	if (index) {
		rype_index_free(index);
	}
	// The stream holds the streaming result, which points into the connection's
	// context: stream first, connection last.
	input_stream.reset();
	input_connection.reset();
}

bool RypeFilterTableFunction::Schedule::TakeClassifyLocked() {
	if (error.HasError() || classifying || input_done || held_groups >= MAX_HELD_GROUPS) {
		return false;
	}
	classifying = true;
	held_groups++;
	return true;
}

bool RypeFilterTableFunction::Schedule::TakeIndexLoadLocked() {
	if (error.HasError() || !use_minimap2 || minimap2_index || index_loading) {
		return false;
	}
	index_loading = true;
	return true;
}

RypeFilterJobs RypeFilterTableFunction::Schedule::TakeJobsLocked(bool inline_jobs) {
	RypeFilterJobs jobs;
	jobs.load_index = TakeIndexLoadLocked();
	if (!inline_jobs || !jobs.load_index) {
		jobs.classify = TakeClassifyLocked();
	}
	return jobs;
}

vector<InterruptState> RypeFilterTableFunction::Schedule::TakeWaitersLocked() {
	wakes++;
	vector<InterruptState> woken;
	woken.swap(parked);
	return woken;
}

RypeFilterFollowUp RypeFilterTableFunction::Schedule::PublishLocked(shared_ptr<RypeFilterGroup> group) {
	RypeFilterFollowUp follow_up;
	classifying = false;
	if (group) {
		group->runs_left = group->runs.size();
		for (idx_t run = 0; run < group->runs.size(); run++) {
			units.push_back(RypeFilterUnit {group, run});
		}
	} else {
		// Nothing more will be published: every parked thread can finish.
		input_done = true;
		held_groups--;
	}
	follow_up.wake = TakeWaitersLocked();
	// Carry on with the next group while there is room for it.
	follow_up.jobs.classify = TakeClassifyLocked();
	return follow_up;
}

RypeFilterFollowUp
RypeFilterTableFunction::Schedule::IndexLoadedLocked(std::shared_ptr<miint::SharedMinimap2Index> loaded) {
	RypeFilterFollowUp follow_up;
	minimap2_index = std::move(loaded);
	index_loading = false;
	follow_up.wake = TakeWaitersLocked();
	return follow_up;
}

RypeFilterFollowUp RypeFilterTableFunction::Schedule::FailLocked(ErrorData failure) {
	RypeFilterFollowUp follow_up;
	if (!error.HasError()) {
		error = std::move(failure);
	}
	follow_up.wake = TakeWaitersLocked();
	return follow_up;
}

bool RypeFilterTableFunction::Schedule::ClaimLocked(RypeFilterUnit &unit) {
	if (units.empty() || (use_minimap2 && !minimap2_index)) {
		return false;
	}
	unit = std::move(units.front());
	units.pop_front();
	return true;
}

RypeFilterFollowUp RypeFilterTableFunction::Schedule::FinishUnitLocked(const RypeFilterUnit &unit, bool inline_jobs) {
	RypeFilterFollowUp follow_up;
	if (--unit.group->runs_left > 0) {
		return follow_up;
	}
	held_groups--;
	if (inline_jobs) {
		// Room for the next group, which a waiting thread can now classify.
		follow_up.wake = TakeWaitersLocked();
	} else {
		// Room for the next group: classify it on the async pool.
		follow_up.jobs.classify = TakeClassifyLocked();
	}
	return follow_up;
}

namespace {

using Schedule = RypeFilterTableFunction::Schedule;

void Wake(Schedule &s, const vector<InterruptState> &parked) {
	s.progress.notify_all();
	for (auto &task : parked) {
		task.Callback();
	}
}

//! Record the first error and wake everyone, who will throw it.
void Fail(Schedule &s, ErrorData error) {
	RypeFilterFollowUp follow_up;
	{
		std::lock_guard<std::mutex> guard(s.lock);
		follow_up = s.FailLocked(std::move(error));
	}
	Wake(s, follow_up.wake);
}

//! Classify the next group with one RYpe call and mark the rows it matched.
//! Null when the scan has no rows left.
shared_ptr<RypeFilterGroup> ClassifyGroup(Schedule &s) {
	auto &input = *s.input_stream;
	if (!input.BeginGroup()) {
		return nullptr;
	}
	auto group = make_shared_ptr<RypeFilterGroup>();
	group->first_id = s.next_first_id;

	// The input stream stays owned by the schedule whatever RYpe does with it;
	// see the ownership note in rype_input_stream.hpp. The stream ends after one
	// group, so RYpe sees exactly the rows that group holds.
	ArrowArrayStreamWrapper output;
	if (rype_classify_arrow_ex(s.index, nullptr, &input.stream, s.threshold, s.group_rows,
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
	group->matched.assign(s.group_rows, false);
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
			if (id < 0 || static_cast<idx_t>(id) < group->first_id ||
			    static_cast<idx_t>(id) - group->first_id >= s.group_rows) {
				throw IOException("RYpe returned query_id %lld, outside the group it was sent (ids %llu to %llu)",
				                  static_cast<long long>(id), static_cast<unsigned long long>(group->first_id),
				                  static_cast<unsigned long long>(group->first_id + s.group_rows - 1));
			}
			const idx_t row = static_cast<idx_t>(id) - group->first_id;
			group->matched[row] = true;
			matched_end = MaxValue<idx_t>(matched_end, row + 1);
		}
	}

	// RYpe has pulled the group to its end-of-stream, so every row it was sent is
	// held now.
	group->runs = input.TakeGroup();
	idx_t group_size = 0;
	for (const auto &run : group->runs) {
		// A unit is one run and becomes at most one output chunk.
		D_ASSERT(run.to - run.from <= STANDARD_VECTOR_SIZE);
		group_size += run.to - run.from;
	}
	if (group_size == 0) {
		// The same rows would begin the next group, and this would never end.
		throw InternalException("rype_filter: RYpe finished a group without reading any of it");
	}
	D_ASSERT(group->runs.front().first_id == group->first_id);
	if (matched_end > group_size) {
		throw IOException("RYpe returned a query_id past the %llu rows of its group",
		                  static_cast<unsigned long long>(group_size));
	}
	s.next_first_id += group_size;
	return group;
}

//! The classify job: classify groups and publish their runs until MAX_HELD_GROUPS
//! are held or the scan ends. Whoever took the job runs it, so it runs alone.
void RunClassify(Schedule &s) {
	for (;;) {
		shared_ptr<RypeFilterGroup> group;
		try {
			group = ClassifyGroup(s);
		} catch (std::exception &e) {
			Fail(s, ErrorData(e));
			return;
		}
		RypeFilterFollowUp follow_up;
		{
			std::lock_guard<std::mutex> guard(s.lock);
			follow_up = s.PublishLocked(std::move(group));
		}
		Wake(s, follow_up.wake);
		if (!follow_up.jobs.classify) {
			return;
		}
	}
}

//! The index-load job. minimap2's loader cannot be interrupted, so a query that
//! ends meanwhile waits for it in teardown.
void RunIndexLoad(Schedule &s) {
	std::shared_ptr<miint::SharedMinimap2Index> loaded;
	try {
		loaded = std::make_shared<miint::SharedMinimap2Index>(s.minimap2_path, FilterMinimap2Config());
	} catch (std::exception &e) {
		Fail(s, ErrorData(IOException("Failed to load minimap2 index '%s': %s", s.minimap2_path, e.what())));
		return;
	}
	RypeFilterFollowUp follow_up;
	{
		std::lock_guard<std::mutex> guard(s.lock);
		follow_up = s.IndexLoadedLocked(std::move(loaded));
	}
	Wake(s, follow_up.wake);
}

//! A job scheduled on the async pool. The schedule it runs against is its own
//! reference, so the job never depends on the GlobalState, which drains the
//! executor before it goes anyway.
class FilterJobTask : public BaseExecutorTask {
public:
	FilterJobTask(TaskExecutor &executor, shared_ptr<Schedule> schedule_p, bool classify_p)
	    : BaseExecutorTask(executor), schedule(std::move(schedule_p)), classify(classify_p) {
	}

	void ExecuteTask() override {
		if (classify) {
			RunClassify(*schedule);
		} else {
			RunIndexLoad(*schedule);
		}
	}

	//! Retired unrun, which happens only as the query is torn down: settle it as a
	//! failure, so nothing waits on a job that will never finish.
	void Cancel() override {
		Fail(*schedule, ErrorData(InterruptException()));
	}

	string TaskType() const override {
		return classify ? "RypeFilterClassify" : "RypeFilterIndexLoad";
	}

private:
	shared_ptr<Schedule> schedule;
	bool classify;
};

//! Run the jobs a thread took: on the async pool, or here when it has no threads.
void StartJobs(RypeFilterTableFunction::GlobalState &gstate, const RypeFilterJobs &jobs) {
	auto &s = *gstate.schedule;
	if (!gstate.executor) {
		if (jobs.load_index) {
			RunIndexLoad(s);
		}
		if (jobs.classify) {
			RunClassify(s);
		}
		return;
	}
	vector<unique_ptr<BaseExecutorTask>> tasks;
	if (jobs.load_index) {
		tasks.push_back(make_uniq<FilterJobTask>(*gstate.executor, gstate.schedule, false));
	}
	if (jobs.classify) {
		tasks.push_back(make_uniq<FilterJobTask>(*gstate.executor, gstate.schedule, true));
	}
	gstate.executor->ScheduleTasks(std::move(tasks));
}

//! Slice the rows of `unit` that neither stage removes into `output`: RYpe did
//! not match them and, when minimap2 runs, it maps neither mate. False when it
//! has none, leaving `output` untouched.
bool EmitUnit(const RypeFilterTableFunction::Data &bind_data, RypeFilterTableFunction::LocalState &lstate,
              const RypeFilterUnit &unit, DataChunk &output) {
	const auto &group = *unit.group;
	const auto &run = group.runs[unit.run];
	auto &chunk = *run.chunk;
	auto *aligner = lstate.aligner.get();
	// minimap2 reads the sequences where the chunk holds them, and names each read
	// by its read_id as align_minimap2 does; no name when there is none.
	UnifiedVectorFormat sequence1;
	UnifiedVectorFormat sequence2;
	std::vector<std::string> names;
	std::vector<bool> name_is_null;
	if (aligner) {
		chunk.data[bind_data.sequence1_column].ToUnifiedFormat(sequence1);
		if (bind_data.sequence2_column != DConstants::INVALID_INDEX) {
			chunk.data[bind_data.sequence2_column].ToUnifiedFormat(sequence2);
		}
		if (bind_data.read_id_column != DConstants::INVALID_INDEX) {
			ExtractIdColumnAsStrings(chunk, bind_data.read_id_column, bind_data.column_types[bind_data.read_id_column],
			                         names, name_is_null);
		}
	}
	const auto maps = [&](idx_t i) {
		const auto s1_idx = sequence1.sel->get_index(i);
		// The stream refused a NULL sequence1 before RYpe saw the row.
		D_ASSERT(sequence1.validity.RowIsValid(s1_idx));
		const auto s1 = UnifiedVectorFormat::GetData<string_t>(sequence1)[s1_idx];
		std::string_view mate;
		if (bind_data.sequence2_column != DConstants::INVALID_INDEX) {
			const auto s2_idx = sequence2.sel->get_index(i);
			if (sequence2.validity.RowIsValid(s2_idx)) {
				const auto s2 = UnifiedVectorFormat::GetData<string_t>(sequence2)[s2_idx];
				mate = std::string_view(s2.GetData(), s2.GetSize());
			}
		}
		const char *name = names.empty() || name_is_null[i] ? nullptr : names[i].c_str();
		return aligner->any_mapped(std::string_view(s1.GetData(), s1.GetSize()), mate, name);
	};

	SelectionVector sel(run.to - run.from);
	idx_t count = 0;
	for (idx_t i = run.from; i < run.to; i++) {
		if (group.matched[run.first_id + (i - run.from) - group.first_id]) {
			continue;
		}
		if (aligner && maps(i)) {
			continue;
		}
		sel.set_index(count++, i);
	}
	if (count == 0) {
		return false;
	}
	// The output references the held chunk's buffers, so the unit can be dropped
	// once it is emitted.
	output.Slice(chunk, sel, count);
	return true;
}

//! Count `unit` emitted, and release its group with its last run.
void FinishUnit(RypeFilterTableFunction::GlobalState &gstate, const RypeFilterUnit &unit) {
	auto &s = *gstate.schedule;
	RypeFilterFollowUp follow_up;
	{
		std::lock_guard<std::mutex> guard(s.lock);
		follow_up = s.FinishUnitLocked(unit, !gstate.executor);
	}
	Wake(s, follow_up.wake);
	if (follow_up.jobs.Any()) {
		StartJobs(gstate, follow_up.jobs);
	}
}

} // namespace

// ============================================================================
// GlobalState destructor
// ============================================================================
RypeFilterTableFunction::GlobalState::~GlobalState() {
	// A query can end with a group still being classified, after a LIMIT or an
	// error. Stop that RYpe call at its next batch, and the scan behind it at its
	// next fetch; the jobs are waited for below.
	if (schedule && schedule->input_stream) {
		schedule->input_stream->Abort();
	}
	if (schedule && schedule->input_connection) {
		schedule->input_connection->Interrupt();
	}
	if (executor) {
		executor->CancelAndDrain();
	}
	// The schedule, and with it the index, the scan and its connection, goes with
	// its last reference.
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
	if (input.inputs.size() > 2) {
		data->minimap2_index_path = input.inputs[2].ToString();
	}

	auto threshold_param = input.named_parameters.find("threshold");
	if (threshold_param != input.named_parameters.end() && !threshold_param->second.IsNull()) {
		data->threshold = threshold_param->second.GetValue<double>();
		if (data->threshold < 0.0 || data->threshold > 1.0) {
			throw BinderException("threshold must be between 0.0 and 1.0");
		}
	}

	// The index paths are left to the loaders in InitGlobal, as rype_classify
	// leaves its own: they report a missing or damaged index better than a check
	// here, and a minimap2 index is not opened at all for long reads.
	auto columns = GetTableColumns(context, data->relation);
	for (idx_t i = 0; i < columns.names.size(); i++) {
		const auto &name = columns.names[i];
		if (StringUtil::CIEquals(name, "read_id")) {
			data->read_id_column = i;
			continue;
		}
		const bool is_sequence1 = StringUtil::CIEquals(name, "sequence1");
		if (!is_sequence1 && !StringUtil::CIEquals(name, "sequence2")) {
			continue;
		}
		(is_sequence1 ? data->sequence1_column : data->sequence2_column) = i;
		const auto type = columns.types[i].id();
		if (type != LogicalTypeId::VARCHAR && type != LogicalTypeId::BLOB) {
			throw BinderException("Column '%s' in '%s' must be VARCHAR or BLOB, not %s", name, data->relation,
			                      columns.types[i].ToString());
		}
	}
	if (data->sequence1_column == DConstants::INVALID_INDEX) {
		throw BinderException("Table '%s' missing required column 'sequence1'", data->relation);
	}
	// minimap2 is given the read_id as its name; RYpe alone never reads it.
	if (!data->minimap2_index_path.empty() && data->read_id_column != DConstants::INVALID_INDEX &&
	    !IsAllowedIdType(columns.types[data->read_id_column])) {
		throw BinderException("Column '%s' in '%s' must be %s to name its reads to minimap2, not %s",
		                      columns.names[data->read_id_column], data->relation, AllowedIdTypeList(),
		                      columns.types[data->read_id_column].ToString());
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
	gstate->schedule = make_shared_ptr<Schedule>();
	auto &s = *gstate->schedule;
	s.threshold = bind_data.threshold;
	const idx_t group_rows_setting = GetRypeFilterGroupRows(context);

	// The scan opens first: whether minimap2 runs is read from its head, and a
	// minimap2 index rype_filter cannot use is refused before the RYpe index loads.
	auto &db = DatabaseInstance::GetDatabase(context);
	s.input_connection = make_uniq<Connection>(db);
	InheritTempObjects(context, *s.input_connection);
	auto &conn = *s.input_connection;
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
	s.input_stream = BuildRypeFilterStream(conn, bind_data.column_names, std::move(stream_options));
	// The output schema was fixed at bind; a relation redefined since then would
	// hand rows of other types to a plan that expects these.
	if (s.input_stream->ScanTypes() != bind_data.column_types) {
		throw InvalidInputException("'%s' changed after the query was bound: its column types differ",
		                            bind_data.relation);
	}

	// From the head of the stream's own scan, which SampleSizing does not consume:
	// the relation is read once (#229).
	const auto &minimap2_path = bind_data.minimap2_index_path;
	RypeInputStream::Sizing sizing {};
	if (group_rows_setting == 0 || !minimap2_path.empty()) {
		sizing = s.input_stream->SampleSizing();
	}
	// An empty relation has nothing to map, and no length to tell short reads by.
	const bool use_minimap2 =
	    !minimap2_path.empty() && sizing.sampled && sizing.avg_read_length < LONG_READ_MEAN_LENGTH;
	idx_t minimap2_index_bytes = 0;
	if (use_minimap2) {
		bool multi_part = false;
		try {
			multi_part = miint::Minimap2Aligner::IsMultiPartIndex(minimap2_path);
			minimap2_index_bytes =
			    FileSystem::GetFileSystem(context).OpenFile(minimap2_path, FileFlags::FILE_FLAGS_READ)->GetFileSize();
		} catch (const std::exception &e) {
			throw IOException("Failed to load minimap2 index '%s': %s", minimap2_path, e.what());
		}
		// Each part would be loaded and mapped against in turn, and the reads held
		// meanwhile: the whole relation, or a second read of it.
		if (multi_part) {
			throw InvalidInputException(
			    "rype_filter: '%s' is a multi-part minimap2 index, and rype_filter maps against a whole index at "
			    "once. Rebuild it as one part: save_minimap2_index always builds one, and minimap2 -d does when -I "
			    "is larger than the reference.",
			    minimap2_path);
		}
	}

	s.index = rype_index_load(bind_data.index_path.c_str());
	if (!s.index) {
		const char *err = rype_get_last_error();
		throw IOException("Failed to load RYpe index '%s': %s", bind_data.index_path, err ? err : "unknown error");
	}

	if (group_rows_setting > 0) {
		s.group_rows = group_rows_setting;
	} else {
		// The read-ahead buffer is drawn from the same memory, and so is the
		// minimap2 index, which loads after this reading is taken. 0 means
		// detection failed, which leaves RYpe to size against its own estimate.
		size_t budget = ResolveRypeMemoryBudget(context, 0);
		if (budget > 0) {
			const size_t floor = MinValue<size_t>(budget, RYPE_MIN_MEMORY_BUDGET);
			const size_t reserved = read_ahead_bytes + minimap2_index_bytes;
			budget = MaxValue<size_t>(budget > reserved ? budget - reserved : 0, floor);
			if (budget <= RYPE_MIN_MEMORY_BUDGET) {
				miint::EmitWarning(context,
				                   "rype_filter: only %s of memory is free for classification, so groups are small and "
				                   "each costs a pass over the index; this can be many times slower than usual. Free "
				                   "memory, or run when the host is quieter.",
				                   StringUtil::BytesToHumanReadableString(budget));
			}
		}
		s.group_rows = AutoGroupRows(context, s.index, sizing, budget);
	}
	s.input_stream->SetGroupRows(s.group_rows);
	s.input_stream->SetBatchRows(s.group_rows);

	// The index loads on a job of its own, alongside the first groups' classifying.
	s.use_minimap2 = use_minimap2;
	s.minimap2_path = minimap2_path;

	auto &scheduler = TaskScheduler::GetScheduler(context);
	gstate->max_threads = NumericCast<idx_t>(scheduler.NumberOfThreads());
	if (scheduler.NumberOfAsyncThreads() > 0) {
		gstate->executor = make_uniq<TaskExecutor>(context, TaskSchedulerType::ASYNC);
	}
	return gstate;
}

// ============================================================================
// InitLocal
// ============================================================================
unique_ptr<LocalTableFunctionState> RypeFilterTableFunction::InitLocal(ExecutionContext &context,
                                                                       TableFunctionInitInput &input,
                                                                       GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<GlobalState>();
	auto lstate = make_uniq<LocalState>();
	if (gstate.schedule->use_minimap2) {
		// Attached to the index once it has loaded, when the thread claims its first unit.
		lstate->aligner = make_uniq<miint::Minimap2Aligner>(FilterMinimap2Config());
	}
	return std::move(lstate);
}

// ============================================================================
// Execute
// ============================================================================
void RypeFilterTableFunction::Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<GlobalState>();
	auto &lstate = data_p.local_state->Cast<LocalState>();
	auto &bind_data = data_p.bind_data->Cast<Data>();
	auto &s = *gstate.schedule;
	// Parking needs a task to wake later: there is none under a synchronous scan,
	// or where a positional join drives this one.
	const bool can_park =
	    data_p.results_execution_mode == AsyncResultsExecutionMode::TASK_EXECUTOR && data_p.interrupt_state;
	const bool inline_jobs = !gstate.executor;
	for (;;) {
		context.InterruptCheck();
		RypeFilterUnit unit;
		bool claimed = false;
		RypeFilterJobs jobs;
		bool park = false;
		idx_t wakes_seen = 0;
		{
			std::unique_lock<std::mutex> guard(s.lock);
			if (s.error.HasError()) {
				auto error = s.error;
				guard.unlock();
				error.Throw();
			}
			if (s.ClaimLocked(unit)) {
				claimed = true;
				if (lstate.aligner && !lstate.attached) {
					lstate.aligner->attach_shared_index(s.minimap2_index);
					lstate.attached = true;
				}
			} else if (s.input_done && s.units.empty()) {
				// No unit is left, and no group will follow.
				data_p.async_result = AsyncResultType::FINISHED;
				return;
			} else {
				jobs = s.TakeJobsLocked(inline_jobs);
				// A thread with a job to run inline runs it; any other waits.
				if (can_park && !(inline_jobs && jobs.Any())) {
					// Registered last, under the lock every wake-up takes, so none is
					// missed; and nothing this call does after it wakes anyone.
					s.parked.push_back(*data_p.interrupt_state);
					park = true;
				}
				wakes_seen = s.wakes;
			}
		}

		if (claimed) {
			const bool emitted = EmitUnit(bind_data, lstate, unit, output);
			FinishUnit(gstate, unit);
			if (emitted) {
				data_p.async_result = AsyncResultType::HAVE_MORE_OUTPUT;
				return;
			}
			// A unit with no survivors: claim the next one in this same call.
			continue;
		}
		if (jobs.Any()) {
			StartJobs(gstate, jobs);
		}
		if (park) {
			data_p.async_result = AsyncResultType::BLOCKED;
			return;
		}
		if (inline_jobs && jobs.Any()) {
			continue;
		}
		// Cannot park: wait for the background work, looking again now and then so
		// an interrupted query stops.
		std::unique_lock<std::mutex> guard(s.lock);
		s.progress.wait_for(guard, WAIT_FOR_PROGRESS, [&]() { return s.wakes != wakes_seen; });
	}
}

// ============================================================================
// Register
// ============================================================================
void RypeFilterTableFunction::Register(ExtensionLoader &loader) {
	TableFunctionSet set("rype_filter");
	// rype_filter(relation, rype_index) -> RYpe alone;
	// rype_filter(relation, rype_index, minimap2_index) -> RYpe, then minimap2 for short reads.
	for (const auto &args : {vector<LogicalType> {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                         vector<LogicalType> {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}}) {
		TableFunction tf("rype_filter", args, Execute, Bind, InitGlobal, InitLocal);
		AddNamedParameter(tf, "threshold", LogicalType::DOUBLE);
		// Each thread emits the units it claims, so rows come out in no fixed order,
		// as rype_classify's and align_minimap2's do. Kept in order, every unit would
		// be its own batch, which DuckDB's order-keeping sinks hold until enough rows
		// follow to merge them: a run with few survivors per unit would pay for that.
		tf.order_preservation_type = OrderPreservationType::NO_ORDER;
		set.AddFunction(tf);
	}
	loader.RegisterFunction(set);
}

} // namespace duckdb
