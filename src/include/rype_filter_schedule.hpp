#pragma once
//
// What rype_filter's threads and background jobs share (rype_filter.cpp), and the
// state changes they make to it under its lock. The state changes do no work and
// wake no one themselves: each returns what its caller must do once the lock is
// released, which keeps them testable alone (test/cpp/test_RypeFilterSchedule.cpp).

#include "rype_filter.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/parallel/interrupt.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace duckdb {

//! One group RYpe classified: the runs the stream held for its rows, and which
//! of them RYpe matched. Read-only once published, except runs_left.
struct RypeFilterGroup {
	vector<RypeHeldRows> runs;
	//! The Arrow id of the group's first row; matched is indexed by id - first_id.
	idx_t first_id = 0;
	vector<bool> matched;
	//! Runs not yet emitted. Guarded by the schedule's lock.
	idx_t runs_left = 0;
};

//! One held run of a group, emitted by whichever thread claims it.
struct RypeFilterUnit {
	shared_ptr<RypeFilterGroup> group;
	idx_t run = 0;
};

//! Background work a thread has taken on under the lock, and must now run or schedule.
struct RypeFilterJobs {
	bool classify = false;
	bool load_index = false;

	bool Any() const {
		return classify || load_index;
	}
};

//! What a state change leaves its caller to do once the lock is released: the
//! jobs it took on, and the parked threads to wake. A parked task's callback waits
//! for that task to return, and it may be about to take the lock.
struct RypeFilterFollowUp {
	RypeFilterJobs jobs;
	vector<InterruptState> wake;
};

//! Two jobs run off DuckDB's threads, on its async pool: classifying, one RYpe
//! call per group, one group at a time and in scan order; and, when minimap2
//! runs, loading its index. Each group's runs are published as units, which
//! DuckDB's threads claim, map, and emit. The output is declared unordered, as
//! rype_classify's and align_minimap2's are, so the threads' rows interleave
//! freely. A thread with nothing to claim parks where it can, and parked threads
//! are woken when units become ready, at the end of the input, and for an error.
//! Where a thread cannot park (a synchronous scan, or a positional join driving
//! this one) it waits on `progress` instead.
//! With no async threads the jobs run on the DuckDB thread that finds them
//! needed, and threads are also woken when a group is released, since one of
//! them must then classify the next.
struct RypeFilterTableFunction::Schedule {
	//! At most this many groups are held at once: one whose runs are being mapped
	//! and emitted while the next is classified. AutoGroupRows sizes groups for it.
	static constexpr idx_t MAX_HELD_GROUPS = 2;

	// Set at InitGlobal, then read-only.
	RypeIndex *index = nullptr;
	//! The connection the scan runs on. The stream's streaming result points into
	//! its context, so it outlives input_stream.
	unique_ptr<Connection> input_connection;
	unique_ptr<RypeInputStream> input_stream;
	double threshold = 0.1;
	idx_t group_rows = 0;
	bool use_minimap2 = false;
	std::string minimap2_path;

	//! The Arrow id the next group starts at. Only the one classify running reads
	//! or writes it.
	idx_t next_first_id = 0;

	std::mutex lock;
	std::condition_variable progress;
	// Guarded by lock.
	vector<InterruptState> parked;
	//! Counts wake-ups, so a thread waiting on `progress` sees one it slept through.
	idx_t wakes = 0;
	//! Published runs not yet claimed, in scan order.
	std::deque<RypeFilterUnit> units;
	//! Groups being classified, or with runs not yet emitted.
	idx_t held_groups = 0;
	bool classifying = false;
	//! The scan has no rows left: no group will follow those published.
	bool input_done = false;
	bool index_loading = false;
	std::shared_ptr<miint::SharedMinimap2Index> minimap2_index;
	ErrorData error;

	// The state changes. Each is called with `lock` held.

	//! The jobs that are due, marked as running. Run inline, a thread takes one,
	//! so that another thread can take the other.
	RypeFilterJobs TakeJobsLocked(bool inline_jobs);
	//! A classify job's group, or null for the end of the input. The job goes on to
	//! the next group when the follow-up's classify is set.
	RypeFilterFollowUp PublishLocked(shared_ptr<RypeFilterGroup> group);
	RypeFilterFollowUp IndexLoadedLocked(std::shared_ptr<miint::SharedMinimap2Index> loaded);
	//! Record the first error; everyone woken throws it.
	RypeFilterFollowUp FailLocked(ErrorData failure);
	//! Take the next unit, when one is published and minimap2, if it runs, has its index.
	bool ClaimLocked(RypeFilterUnit &unit);
	//! `unit` is emitted. With its group's last unit the group is released, and the
	//! next group's classify is due: taken here when async threads will run it, and
	//! otherwise left to a woken thread.
	RypeFilterFollowUp FinishUnitLocked(const RypeFilterUnit &unit, bool inline_jobs);

	~Schedule();

private:
	bool TakeClassifyLocked();
	bool TakeIndexLoadLocked();
	//! Every parked thread, and a wake-up for those waiting on `progress`.
	vector<InterruptState> TakeWaitersLocked();
};

} // namespace duckdb
