// rype_filter's schedule bookkeeping, driven without RYpe, minimap2 or a query.
//
// Which background job is due, and which parked threads to wake, are decided under the schedule's
// lock by a few state changes (rype_filter_schedule.hpp). Most of them show in a query's result
// when wrong: a lost wake-up hangs the query, and a unit dropped or emitted twice changes its rows
// (test/sql/rype_filter_parallel.test). These do not, or only now and then, and are pinned here:
//  - At most two groups are held: one being mapped while the next is classified. Classifying is
//    faster than mapping, so without the cap the classify job would run ahead through the whole
//    input, holding every read it passed in memory, and the rows would still come out right.
//  - The classify job goes straight on to the second group. Otherwise the next classify would
//    start only once a thread ran out of units, and every thread would then wait on it: mapping
//    and classifying would take turns instead of overlapping, again with the right rows.
//  - Releasing a group starts the next one's classify. Besides keeping the two overlapped, this is
//    what wakes the parked threads when the thread that released the group stops pulling rows, as
//    it does once a LIMIT is met: with no classify started they wait for one forever. Whether a
//    query meets that depends on timing, so its hang shows only in some runs.
//  - With no async threads, releasing a group wakes the parked threads instead, since one of them
//    must classify the next group itself. Missing that hangs a query the same way, as rarely.

#include <catch2/catch_test_macros.hpp>

#include "rype_filter_schedule.hpp"

namespace {

using namespace duckdb;
using Schedule = RypeFilterTableFunction::Schedule;

//! A classified group of `runs` runs, as the classify job publishes it. The runs hold no rows:
//! the bookkeeping only counts them.
shared_ptr<RypeFilterGroup> Group(idx_t runs) {
	auto group = make_shared_ptr<RypeFilterGroup>();
	group->runs.resize(runs);
	return group;
}

vector<RypeFilterUnit> ClaimAll(Schedule &s) {
	vector<RypeFilterUnit> claimed;
	RypeFilterUnit unit;
	while (s.ClaimLocked(unit)) {
		claimed.push_back(std::move(unit));
	}
	return claimed;
}

} // namespace

TEST_CASE("rype_filter classifies one group ahead of the one being mapped, and no further", "[rype_filter]") {
	Schedule s;
	REQUIRE(s.TakeJobsLocked(false).classify);
	// The job publishes the first group and goes straight on to the second.
	REQUIRE(s.PublishLocked(Group(2)).jobs.classify);
	// With two groups held it stops, and no thread can start a third.
	REQUIRE_FALSE(s.PublishLocked(Group(1)).jobs.classify);
	REQUIRE_FALSE(s.TakeJobsLocked(false).classify);
	// Claiming units releases nothing: they are held until emitted.
	auto units = ClaimAll(s);
	REQUIRE(units.size() == 3);
	REQUIRE_FALSE(s.TakeJobsLocked(false).classify);
}

TEST_CASE("rype_filter starts the next group's classify as a group is released", "[rype_filter]") {
	Schedule s;
	REQUIRE(s.TakeJobsLocked(false).classify);
	REQUIRE(s.PublishLocked(Group(2)).jobs.classify);
	REQUIRE_FALSE(s.PublishLocked(Group(1)).jobs.classify);
	auto units = ClaimAll(s);
	REQUIRE(units.size() == 3);
	// The first group's first unit leaves it held.
	REQUIRE_FALSE(s.FinishUnitLocked(units[0], false).jobs.Any());
	// Its last releases it: the third group's classify is due at once, while the second group's unit
	// is still being mapped, and it is handed to the thread that released the group.
	REQUIRE(s.FinishUnitLocked(units[1], false).jobs.classify);
	REQUIRE_FALSE(s.TakeJobsLocked(false).classify);
}

TEST_CASE("rype_filter without async threads wakes the parked threads as a group is released", "[rype_filter]") {
	Schedule s;
	REQUIRE(s.TakeJobsLocked(true).classify);
	REQUIRE(s.PublishLocked(Group(1)).jobs.classify);
	REQUIRE_FALSE(s.PublishLocked(Group(1)).jobs.classify);
	auto units = ClaimAll(s);
	REQUIRE(units.size() == 2);
	s.parked.push_back(InterruptState());
	auto follow_up = s.FinishUnitLocked(units[0], true);
	// The job is not the releasing thread's: a woken thread takes it, and runs it itself.
	REQUIRE_FALSE(follow_up.jobs.Any());
	REQUIRE(follow_up.wake.size() == 1);
	REQUIRE(s.parked.empty());
	REQUIRE(s.TakeJobsLocked(true).classify);
}
