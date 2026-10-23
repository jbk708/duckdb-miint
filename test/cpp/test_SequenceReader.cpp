#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <string>
#include <fstream>
#include <filesystem>
#include <future>
#include <system_error>
#include <thread>
#include "QualScore.hpp"
#include "SequenceRecord.hpp"
#include "SequenceReader.hpp"
#include <vector>

// POSIX-only calls used by tests below (resource limits, geteuid and permission bits that deny
// the owner, FIFOs, fork); those tests are compiled out elsewhere. Same guard as
// test_NewickParser.cpp.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Test fixture for RAII-based temp file management
class TempFileFixture {
public:
	~TempFileFixture() {
		for (const auto &path : temp_files_) {
			std::filesystem::remove(path);
		}
	}

	void write_temp_fastq(const std::string &path, const std::vector<std::string> &records) {
		std::ofstream out(path);
		if (!out) {
			throw std::runtime_error("Failed to create temp file: " + path);
		}
		for (const auto &r : records) {
			out << r;
		}
		out.close();
		temp_files_.push_back(path);
	}

	std::string simple_read(const std::string &id, const std::string &seq, const std::string &qual,
	                        const std::string &comment = "") {
		return "@" + id + (comment.empty() ? "" : " " + comment) + "\n" + seq + "\n+\n" + qual + "\n";
	}

	std::string simple_fasta(const std::string &id, const std::string &seq, const std::string &comment = "") {
		return ">" + id + (comment.empty() ? "" : " " + comment) + "\n" + seq + "\n";
	}

private:
	std::vector<std::string> temp_files_;
};

TEST_CASE("SequenceReader single-end", "[SequenceReader]") {
	TempFileFixture fixture;
	auto path = "test_R1.fastq";
	fixture.write_temp_fastq(path,
	                         {fixture.simple_read("r1", "ACGT", "IIII"), fixture.simple_read("r2", "TGCA", "HHHH")});

	miint::SequenceReader reader(path);
	auto batch = reader.read(5);

	REQUIRE((batch.size() == 2));
	REQUIRE((batch.read_ids[0] == "r1"));
	REQUIRE((batch.read_ids[1] == "r2"));
}

TEST_CASE("SequenceReader single-end / bug", "[SequenceReader]") {
	TempFileFixture fixture;
	auto path = "test_R1.fastq";
	fixture.write_temp_fastq(
	    path, {fixture.simple_read("r1/foo/bar", "ACGT", "IIII"), fixture.simple_read("r2/123/456", "TGCA", "HHHH")});

	miint::SequenceReader reader(path);
	auto batch = reader.read(5);

	REQUIRE((batch.size() == 2));
	REQUIRE((batch.read_ids[0] == "r1/foo/bar"));
	REQUIRE((batch.read_ids[1] == "r2/123/456"));
}

TEST_CASE("SequenceReader paired-end valid", "[SequenceReader]") {
	TempFileFixture fixture;
	auto r1 = "test_p1.fastq";
	auto r2 = "test_p2.fastq";

	fixture.write_temp_fastq(r1,
	                         {fixture.simple_read("x/1", "ACGT", "IIII"), fixture.simple_read("y", "TGCA", "HHHH")});
	fixture.write_temp_fastq(r2,
	                         {fixture.simple_read("x/2", "AAAA", "DDDD"), fixture.simple_read("y", "CCCC", "EEEE")});

	miint::SequenceReader reader(r1, r2);
	auto batch = reader.read(2);
	REQUIRE((batch.size() == 2));
	REQUIRE((batch.read_ids[0] == "x"));
	REQUIRE((batch.sequences2[0] == "AAAA"));
	REQUIRE((batch.quals2[1].as_string() == "EEEE"));
}

TEST_CASE("SequenceReader mismatched IDs", "[SequenceReader]") {
	TempFileFixture fixture;
	auto r1 = "bad_p1.fastq";
	auto r2 = "bad_p2.fastq";

	fixture.write_temp_fastq(r1, {fixture.simple_read("r1/1", "ACGT", "IIII")});
	fixture.write_temp_fastq(r2, {fixture.simple_read("r2/2", "ACGT", "IIII")});

	REQUIRE_THROWS_WITH(miint::SequenceReader(r1, r2).read(1), "Mismatched read IDs: r1/1 vs r2/2");
}

TEST_CASE("SequenceReader partial batch", "[SequenceReader]") {
	TempFileFixture fixture;
	auto path = "few_reads.fastq";
	fixture.write_temp_fastq(path, {fixture.simple_read("only_one", "ACGT", "IIII")});

	miint::SequenceReader reader(path);
	auto batch = reader.read(5);
	REQUIRE((batch.size() == 1));
}

TEST_CASE("QualScore from string and back", "[QualScore]") {
	std::string qstr = "IJKLMNOP"; // ASCII 73+
	miint::QualScore qs(qstr);
	REQUIRE((qs.as_string() == qstr));

	auto vec = qs.as_vec();
	REQUIRE((vec.size() == qstr.size()));
	for (size_t i = 0; i < vec.size(); ++i) {
		REQUIRE((static_cast<char>(vec[i] + 33) == qstr[i]));
	}

	miint::QualScore from_vec(vec);
	REQUIRE((from_vec.as_string() == qstr));
}

TEST_CASE("SequenceRecord single-end", "[SequenceRecord]") {
	miint::SequenceRecord rec("read1", "abc", "ATGC", "IIII");
	REQUIRE((rec.read_id == "read1"));
	REQUIRE((rec.comment == "abc"));
	REQUIRE((rec.read1 == "ATGC"));
	REQUIRE_FALSE(rec.read2.has_value());
	REQUIRE((rec.qual1.as_string() == "IIII"));
	REQUIRE_FALSE(rec.qual2.has_value());
}

TEST_CASE("SequenceRecord paired-end", "[SequenceRecord]") {
	miint::SequenceRecord rec("read1", "xyz", "ACGT", "HHHH", "TGCA", "BBBB");
	REQUIRE((rec.read2.value() == "TGCA"));
	REQUIRE((rec.qual2->as_string() == "BBBB"));
}

TEST_CASE("SequenceReader FASTA format single-end", "[SequenceReader][FASTA]") {
	TempFileFixture fixture;
	auto path = "test_fasta.fa";
	fixture.write_temp_fastq(path,
	                         {fixture.simple_fasta("seq1", "ATGC", "comment1"), fixture.simple_fasta("seq2", "GGCC")});

	miint::SequenceReader reader(path);
	auto batch = reader.read(5);

	REQUIRE((batch.size() == 2));
	REQUIRE((batch.read_ids[0] == "seq1"));
	REQUIRE((batch.comments[0] == "comment1"));
	REQUIRE((batch.sequences1[0] == "ATGC"));
	REQUIRE((batch.quals1[0].as_string().empty()));
	REQUIRE((batch.read_ids[1] == "seq2"));
	REQUIRE((batch.comments[1] == ""));
	REQUIRE((batch.quals1[1].as_string().empty()));
}

TEST_CASE("SequenceReader FASTA format paired-end", "[SequenceReader][FASTA]") {
	TempFileFixture fixture;
	auto r1 = "test_r1.fa";
	auto r2 = "test_r2.fa";

	fixture.write_temp_fastq(r1, {fixture.simple_fasta("read1/1", "ATGC"), fixture.simple_fasta("read2", "GGCC")});
	fixture.write_temp_fastq(r2, {fixture.simple_fasta("read1/2", "TGCA"), fixture.simple_fasta("read2", "CCGG")});

	miint::SequenceReader reader(r1, r2);
	auto batch = reader.read(5);

	REQUIRE((batch.size() == 2));
	REQUIRE((batch.read_ids[0] == "read1"));
	REQUIRE((batch.sequences1[0] == "ATGC"));
	REQUIRE((batch.sequences2[0] == "TGCA"));
	REQUIRE((batch.quals1[0].as_string().empty()));
	REQUIRE((batch.quals2[0].as_string().empty()));
}

TEST_CASE("SequenceReader mixed FASTA/FASTQ paired-end throws", "[SequenceReader][FASTA][FASTQ][error]") {
	TempFileFixture fixture;
	auto r1 = "test_mixed_r1.fa";
	auto r2 = "test_mixed_r2.fq";

	fixture.write_temp_fastq(r1, {fixture.simple_fasta("read1/1", "ATGC"), fixture.simple_fasta("read2", "GGCC")});
	fixture.write_temp_fastq(
	    r2, {fixture.simple_read("read1/2", "TGCA", "IIII"), fixture.simple_read("read2", "CCGG", "HHHH")});

	REQUIRE_THROWS_WITH(miint::SequenceReader(r1, r2),
	                    Catch::Matchers::ContainsSubstring("Cannot mix FASTA and FASTQ"));
}

TEST_CASE("SequenceReader mixed FASTA/FASTQ paired-end allowed when flag set",
          "[SequenceReader][FASTA][FASTQ][paired-end]") {
	TempFileFixture fixture;
	auto r1 = "test_allowed_r1.fa";
	auto r2 = "test_allowed_r2.fq";

	fixture.write_temp_fastq(r1, {fixture.simple_fasta("read1/1", "ATGC"), fixture.simple_fasta("read2", "GGCC")});
	fixture.write_temp_fastq(
	    r2, {fixture.simple_read("read1/2", "TGCA", "IIII"), fixture.simple_read("read2", "CCGG", "HHHH")});

	miint::SequenceReader reader(r1, r2, true);
	auto batch = reader.read(5);

	REQUIRE((batch.size() == 2));
	REQUIRE((batch.read_ids[0] == "read1"));
	REQUIRE((batch.sequences1[0] == "ATGC"));
	REQUIRE((batch.sequences2[0] == "TGCA"));
	REQUIRE((batch.quals1[0].as_string().empty()));   // FASTA: no quality
	REQUIRE((batch.quals2[0].as_string() == "IIII")); // FASTQ: has quality
}

TEST_CASE("SequenceReader mixed FASTA/FASTQ with longer sequences", "[SequenceReader][FASTA][FASTQ][paired-end]") {
	TempFileFixture fixture;
	auto r1 = "test_long_r1.fa";
	auto r2 = "test_long_r2.fq";

	fixture.write_temp_fastq(
	    r1, {fixture.simple_fasta("read1/1", "ATGCATGC"), fixture.simple_fasta("read2/1", "GGCCGGCC")});
	fixture.write_temp_fastq(r2, {fixture.simple_read("read1/2", "TGCATGCA", "IIIIIIII"),
	                              fixture.simple_read("read2/2", "CCGGCCGG", "HHHHHHHH")});

	miint::SequenceReader reader(r1, r2, true);
	auto batch = reader.read(5);

	REQUIRE((batch.size() == 2));
	REQUIRE((batch.read_ids[0] == "read1"));
	REQUIRE((batch.read_ids[1] == "read2"));
	REQUIRE((batch.sequences1[0] == "ATGCATGC"));
	REQUIRE((batch.sequences2[0] == "TGCATGCA"));
	REQUIRE((batch.quals1[0].as_string().empty()));
	REQUIRE((batch.quals2[0].as_string() == "IIIIIIII"));
}

TEST_CASE("SequenceReader empty file throws", "[SequenceReader][error]") {
	TempFileFixture fixture;
	auto path = "empty.fq";
	fixture.write_temp_fastq(path, {});

	REQUIRE_THROWS_WITH(miint::SequenceReader(path), Catch::Matchers::ContainsSubstring("Empty file"));
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
// kseq++'s SeqStreamIn does not check gzopen()'s return value. A NULL handle reads as a
// stream with zero records, so before CheckedSeqStreamIn every failed open was reported as
// "Empty file: <path>" -- naming a file that is present, non-empty and perfectly valid.
// That is what made descriptor exhaustion over a large VARCHAR[] scan (issue #260)
// undiagnosable: the error blamed the data. These two conditions must stay distinguishable,
// because the message is the only signal a user gets about which one actually happened.
namespace {
// Returns the message SequenceReader fails with, or "" if it does not throw.
std::string open_failure_message(const std::string &path) {
	try {
		miint::SequenceReader reader(path);
	} catch (const std::exception &e) {
		return e.what();
	}
	return "";
}

// Lowers RLIMIT_NOFILE for its lifetime and always puts the soft limit back, including on
// the stack unwind of a failed REQUIRE.
class FdLimit {
public:
	explicit FdLimit(rlim_t soft) {
		if (getrlimit(RLIMIT_NOFILE, &saved_) != 0) {
			throw std::runtime_error("getrlimit failed");
		}
		struct rlimit lowered = saved_;
		lowered.rlim_cur = soft;
		if (setrlimit(RLIMIT_NOFILE, &lowered) != 0) {
			throw std::runtime_error("setrlimit failed");
		}
	}
	~FdLimit() {
		setrlimit(RLIMIT_NOFILE, &saved_);
	}
	FdLimit(const FdLimit &) = delete;
	FdLimit &operator=(const FdLimit &) = delete;

private:
	struct rlimit saved_;
};

// Opens `path` until the process runs out of descriptors, recording the errno that stopped
// it, and closes them all again on destruction.
class HeldFds {
public:
	explicit HeldFds(const char *path) {
		for (int fd = open(path, O_RDONLY); fd >= 0; fd = open(path, O_RDONLY)) {
			fds_.push_back(fd);
		}
		exhausted_errno = errno;
	}
	~HeldFds() {
		for (int fd : fds_) {
			close(fd);
		}
	}
	HeldFds(const HeldFds &) = delete;
	HeldFds &operator=(const HeldFds &) = delete;

	int exhausted_errno = 0;

private:
	std::vector<int> fds_;
};
} // namespace

TEST_CASE("SequenceReader reports a failed open as a failed open, not an empty file", "[SequenceReader][error]") {
	TempFileFixture fixture;

	SECTION("unreadable file") {
		auto path = "unreadable.fq";
		fixture.write_temp_fastq(path, {fixture.simple_read("read1", "ATGC", "IIII")});
		// Root ignores permission bits, so this case cannot be exercised as root.
		if (geteuid() == 0) {
			SUCCEED("skipped: running as root, permission bits are not enforced");
			return;
		}
		std::filesystem::permissions(path, std::filesystem::perms::none);

		const std::string msg = open_failure_message(path);
		std::filesystem::permissions(path, std::filesystem::perms::owner_all); // let the fixture clean up

		INFO("error was: " << msg);
		REQUIRE(msg.find("Failed to open file") != std::string::npos);
		REQUIRE(msg.find(path) != std::string::npos);
		REQUIRE(msg.find("Empty file") == std::string::npos);
	}

	SECTION("descriptor exhaustion") {
		auto path = "fd_exhausted.fq";
		fixture.write_temp_fastq(path, {fixture.simple_read("read1", "ATGC", "IIII")});

		// Hold every remaining descriptor so the next gzopen() has to fail with EMFILE -- the
		// exact condition a large-VARCHAR[] scan hits once readers are not released. Both the
		// lowered limit and the held descriptors unwind via RAII: a REQUIRE that fails (or one
		// a later edit adds) inside this scope would otherwise leak a 96-descriptor limit into
		// every test that runs after it in this binary, turning one failure into a cascade.
		FdLimit limit(96); // low enough to fill quickly, high enough for Catch2 itself
		HeldFds held(path);

		REQUIRE(held.exhausted_errno == EMFILE);
		const std::string msg = open_failure_message(path);
		INFO("error was: " << msg);
		REQUIRE(msg.find("Failed to open file") != std::string::npos);
		REQUIRE(msg.find("Empty file") == std::string::npos);
	}

	SECTION("a genuinely empty file still reports emptiness") {
		auto path = "really_empty.fq";
		fixture.write_temp_fastq(path, {});

		const std::string msg = open_failure_message(path);
		INFO("error was: " << msg);
		REQUIRE(msg.find("Empty file") != std::string::npos);
		REQUIRE(msg.find("Failed to open file") == std::string::npos);
	}
}
#endif // !_WIN32 && !__EMSCRIPTEN__

TEST_CASE("SequenceReader gzipped file", "[SequenceReader][compression]") {
	miint::SequenceReader reader("data/fastq/foo.r1.fastq.gz");
	auto batch = reader.read(5);

	REQUIRE((batch.size() == 2));
	REQUIRE((batch.read_ids[0] == "foo1"));
	REQUIRE((batch.sequences1[0] == "ATGC"));
}

TEST_CASE("SequenceReader multiple sequential exhaustive reads", "[SequenceReader]") {
	TempFileFixture fixture;
	auto path = "multi_batch.fq";
	fixture.write_temp_fastq(path,
	                         {fixture.simple_read("r1", "AAAA", "IIII"), fixture.simple_read("r2", "TTTT", "HHHH"),
	                          fixture.simple_read("r3", "GGGG", "GGGG"), fixture.simple_read("r4", "CCCC", "FFFF")});

	miint::SequenceReader reader(path);

	auto batch1 = reader.read(2);
	REQUIRE((batch1.size() == 2));
	REQUIRE((batch1.read_ids[0] == "r1"));
	REQUIRE((batch1.read_ids[1] == "r2"));

	auto batch2 = reader.read(2);
	REQUIRE((batch2.size() == 2));
	REQUIRE((batch2.read_ids[0] == "r3"));
	REQUIRE((batch2.read_ids[1] == "r4"));

	auto batch3 = reader.read(2);
	REQUIRE((batch3.size() == 0));
}

TEST_CASE("SequenceReader large batch size", "[SequenceReader]") {
	TempFileFixture fixture;
	auto path = "large_batch.fq";
	std::vector<std::string> records;
	records.reserve(10000);
	for (int i = 0; i < 10000; i++) {
		records.push_back(fixture.simple_read("read" + std::to_string(i), "ACGT", "IIII"));
	}
	fixture.write_temp_fastq(path, records);

	miint::SequenceReader reader(path);
	auto batch = reader.read(10000);

	REQUIRE((batch.size() == 10000));
	REQUIRE((batch.read_ids[0] == "read0"));
	REQUIRE((batch.read_ids[9999] == "read9999"));
}

TEST_CASE("SequenceReader partial read at EOF", "[SequenceReader]") {
	TempFileFixture fixture;
	auto path = "partial_eof.fq";
	fixture.write_temp_fastq(path,
	                         {fixture.simple_read("r1", "ACGT", "IIII"), fixture.simple_read("r2", "TGCA", "HHHH"),
	                          fixture.simple_read("r3", "GGCC", "GGGG")});

	miint::SequenceReader reader(path);
	auto batch = reader.read(10);

	REQUIRE((batch.size() == 3));
}

TEST_CASE("QualScore edge cases", "[QualScore]") {
	std::string low_qual(10, '!');
	miint::QualScore qs_low(low_qual);
	auto vec_low = qs_low.as_vec();
	for (auto q : vec_low) {
		REQUIRE((q == 0));
	}

	std::string high_qual(10, '~');
	miint::QualScore qs_high(high_qual);
	auto vec_high = qs_high.as_vec();
	for (auto q : vec_high) {
		REQUIRE((q == 93));
	}
}

TEST_CASE("SequenceReader paired-end mismatched count with context", "[SequenceReader][error]") {
	TempFileFixture fixture;
	auto r1 = "mismatch_r1.fq";
	auto r2 = "mismatch_r2.fq";

	fixture.write_temp_fastq(r1,
	                         {fixture.simple_read("r1", "ACGT", "IIII"), fixture.simple_read("r2", "TGCA", "HHHH")});
	fixture.write_temp_fastq(r2, {fixture.simple_read("r1", "AAAA", "DDDD")});

	miint::SequenceReader reader(r1, r2);
	REQUIRE_THROWS_WITH(reader.read(5), Catch::Matchers::ContainsSubstring("missing mate"));
}

TEST_CASE("SequenceReader byte budget SE: generous budget returns all records", "[SequenceReader][byte_budget]") {
	TempFileFixture fixture;
	auto path = "budget_generous.fq";
	std::vector<std::string> records;
	for (int i = 0; i < 10; i++) {
		records.push_back(fixture.simple_read("r" + std::to_string(i), "ACGTACGT", "IIIIIIII"));
	}
	fixture.write_temp_fastq(path, records);

	miint::SequenceReader reader(path);
	auto batch = reader.read(100, 1024 * 1024); // 1 MiB -- way more than needed

	REQUIRE((batch.size() == 10));
}

TEST_CASE("SequenceReader byte budget SE: tight budget caps records per chunk", "[SequenceReader][byte_budget]") {
	TempFileFixture fixture;
	auto path = "budget_tight.fq";
	std::vector<std::string> records;
	for (int i = 0; i < 10; i++) {
		// Each record: name=~3 chars, comment=0, seq=8, qual=8 -> ~19 bytes/record
		records.push_back(fixture.simple_read("r" + std::to_string(i), "ACGTACGT", "IIIIIIII"));
	}
	fixture.write_temp_fastq(path, records);

	miint::SequenceReader reader(path);

	// Budget ~25 bytes -> first record (~19 B) fits, adding a second (~38 B total) exceeds.
	// Expect 1 record per call.
	size_t total = 0;
	for (int call = 0; call < 20; call++) {
		auto batch = reader.read(100, 25);
		if (batch.empty()) {
			break;
		}
		REQUIRE((batch.size() == 1));
		total += batch.size();
	}
	REQUIRE((total == 10));
}

TEST_CASE("SequenceReader byte budget SE: starvation guard returns at least one record",
          "[SequenceReader][byte_budget]") {
	TempFileFixture fixture;
	auto path = "budget_starve.fq";
	fixture.write_temp_fastq(path, {fixture.simple_read("big", std::string(1000, 'A'), std::string(1000, 'I'))});

	miint::SequenceReader reader(path);
	auto batch = reader.read(100, 1); // budget absurdly small; single record massively over

	// Starvation guard: always return at least one record when stream has data.
	REQUIRE((batch.size() == 1));
	REQUIRE((batch.read_ids[0] == "big"));
}

TEST_CASE("SequenceReader byte budget SE: budget preserves full content across calls",
          "[SequenceReader][byte_budget]") {
	TempFileFixture fixture;
	auto path = "budget_preserve.fq";
	std::vector<std::string> expected_ids;
	std::vector<std::string> records;
	for (int i = 0; i < 7; i++) {
		std::string id = "rec" + std::to_string(i);
		expected_ids.push_back(id);
		records.push_back(fixture.simple_read(id, "ACGT", "IIII"));
	}
	fixture.write_temp_fastq(path, records);

	miint::SequenceReader reader(path);

	std::vector<std::string> seen;
	for (int call = 0; call < 20; call++) {
		auto batch = reader.read(100, 20); // tight budget; ~1 record per call
		if (batch.empty()) {
			break;
		}
		for (auto &id : batch.read_ids) {
			seen.push_back(id);
		}
	}

	REQUIRE((seen == expected_ids));
}

TEST_CASE("SequenceReader byte budget PE: budget counts both mates", "[SequenceReader][byte_budget]") {
	TempFileFixture fixture;
	auto r1 = "pe_budget_r1.fq";
	auto r2 = "pe_budget_r2.fq";

	std::vector<std::string> recs1, recs2;
	for (int i = 0; i < 5; i++) {
		std::string id = "pair" + std::to_string(i);
		recs1.push_back(fixture.simple_read(id, "ACGT", "IIII"));
		recs2.push_back(fixture.simple_read(id, "TGCA", "HHHH"));
	}
	fixture.write_temp_fastq(r1, recs1);
	fixture.write_temp_fastq(r2, recs2);

	miint::SequenceReader reader(r1, r2);

	// Each mate is ~5 (name) + 4 (seq) + 4 (qual) = 13 bytes; pair ~26 bytes.
	// Budget 40 bytes -> pair 1 fits, adding pair 2 (>52 bytes) exceeds -> 1 pair per call.
	size_t total = 0;
	for (int call = 0; call < 20; call++) {
		auto batch = reader.read(100, 40);
		if (batch.empty()) {
			break;
		}
		REQUIRE((batch.size() == 1));
		REQUIRE((batch.is_paired));
		total += batch.size();
	}
	REQUIRE((total == 5));
}

TEST_CASE("SequenceReader byte budget SE: large records do not over-prefetch", "[SequenceReader][byte_budget]") {
	// Each record alone exceeds the budget, so the byte budget caps the batch at one record.
	// The poll that feeds that batch must NOT eagerly materialize a fixed chunk of large
	// records: with a 500 B budget and ~4 KB records, a single poll should pull one record,
	// not POLL_CHUNK_SIZE of them. This is the memory bound the byte budget is meant to give.
	TempFileFixture fixture;
	auto path = "budget_large_prefetch.fq";
	std::vector<std::string> records;
	for (int i = 0; i < 12; i++) {
		records.push_back(fixture.simple_read("r" + std::to_string(i), std::string(2000, 'A'), std::string(2000, 'I')));
	}
	fixture.write_temp_fastq(path, records);

	miint::SequenceReader reader(path);

	size_t total = 0;
	for (int call = 0; call < 50; call++) {
		auto batch = reader.read(100, 500); // budget << one record
		if (batch.empty()) {
			break;
		}
		REQUIRE((batch.size() == 1));
		total += batch.size();
	}
	REQUIRE((total == 12));
	// The dynamic poll must size to the budget: at most one ~4 KB record per poll.
	REQUIRE((reader.MaxPollCount() == 1));
}

TEST_CASE("SequenceReader byte budget PE: large pairs do not over-prefetch", "[SequenceReader][byte_budget]") {
	TempFileFixture fixture;
	auto r1 = "budget_large_pe_r1.fq";
	auto r2 = "budget_large_pe_r2.fq";
	std::vector<std::string> recs1, recs2;
	for (int i = 0; i < 8; i++) {
		std::string id = "pair" + std::to_string(i);
		recs1.push_back(fixture.simple_read(id, std::string(2000, 'A'), std::string(2000, 'I')));
		recs2.push_back(fixture.simple_read(id, std::string(2000, 'C'), std::string(2000, 'H')));
	}
	fixture.write_temp_fastq(r1, recs1);
	fixture.write_temp_fastq(r2, recs2);

	miint::SequenceReader reader(r1, r2);

	size_t total = 0;
	for (int call = 0; call < 50; call++) {
		auto batch = reader.read(100, 500); // budget << one pair (~8 KB)
		if (batch.empty()) {
			break;
		}
		REQUIRE((batch.size() == 1));
		total += batch.size();
	}
	REQUIRE((total == 8));
	REQUIRE((reader.MaxPollCount() == 1));
}

TEST_CASE("SequenceReader byte budget SE: small records still poll in chunks", "[SequenceReader][byte_budget]") {
	// Guard against the dynamic poll collapsing to 1 for the common short-read case: with a
	// generous budget and tiny records, the poll must still amortize (reach POLL_CHUNK_SIZE).
	TempFileFixture fixture;
	auto path = "budget_small_chunked.fq";
	std::vector<std::string> records;
	for (int i = 0; i < 200; i++) {
		records.push_back(fixture.simple_read("r" + std::to_string(i), "ACGT", "IIII"));
	}
	fixture.write_temp_fastq(path, records);

	miint::SequenceReader reader(path);
	auto batch = reader.read(100, 1024 * 1024); // generous budget, fills to n
	REQUIRE((batch.size() == 100));
	// Must still batch the poll, not degrade to one-record-at-a-time.
	REQUIRE((reader.MaxPollCount() > 1));
}

TEST_CASE("SequenceReader byte budget default (SIZE_MAX) unchanged behavior", "[SequenceReader][byte_budget]") {
	// Regression: callers that don't pass a budget see the same rows-only behavior.
	TempFileFixture fixture;
	auto path = "budget_default.fq";
	std::vector<std::string> records;
	for (int i = 0; i < 50; i++) {
		records.push_back(fixture.simple_read("r" + std::to_string(i), "ACGT", "IIII"));
	}
	fixture.write_temp_fastq(path, records);

	miint::SequenceReader reader(path);
	auto batch = reader.read(100); // no budget argument -> default SIZE_MAX
	REQUIRE((batch.size() == 50));
}

// Reading ahead: with `read_ahead`, a regular file is read on a thread of its own, ahead of the
// reads that take its records, so that inflating and parsing it overlap whatever the caller does
// with each batch. What has to hold: the read-ahead runs while nothing is pulled, and again as
// soon as records are taken; it stops at READ_AHEAD_BYTES, or at read()'s byte budget if that is
// smaller, and a poll of long records does not run far past them, since that memory is counted
// against nothing; a read still gets every record it asks for when they take more bytes than
// that; every batch comes out exactly as it would without it; a pipe, whose read can wait on its
// writer forever, is never read ahead; and a reader dropped while reading ahead goes once the
// poll in flight returns. read_fastx reads ahead only while the threads it takes fit in DuckDB's.
// Nothing is read ahead in wasm, and these tests are compiled out there.

TEST_CASE("SequenceReader reads ahead only while its threads fit beside the scan's", "[SequenceReader][read_ahead]") {
	// A file being read takes one thread, and its read-ahead one more per file of the pair.
	REQUIRE(miint::SequenceReader::ReadAheadFits(1, false, 2));
	REQUIRE_FALSE(miint::SequenceReader::ReadAheadFits(1, true, 2));
	REQUIRE(miint::SequenceReader::ReadAheadFits(1, true, 3));
	REQUIRE_FALSE(miint::SequenceReader::ReadAheadFits(1, false, 1));
	// Every file of the scan counts: eight files on eight threads leave none to read ahead with.
	REQUIRE_FALSE(miint::SequenceReader::ReadAheadFits(8, false, 8));
	REQUIRE(miint::SequenceReader::ReadAheadFits(2, true, 8));
	REQUIRE_FALSE(miint::SequenceReader::ReadAheadFits(3, true, 8));
}

#ifndef __EMSCRIPTEN__

namespace {

//! The most bytes ReadAheadRecords' records count for: a name of up to eight bytes
//! ("r39999/1"), then 150 bases and 150 qualities.
constexpr size_t READ_AHEAD_RECORD_BYTES = 8 + 150 + 150;
//! Records of that size are polled sixteen at a time (POLL_CHUNK_SIZE in SequenceReader.cpp).
constexpr size_t SHORT_RECORDS_PER_POLL = 16;

std::vector<std::string> ReadAheadRecords(TempFileFixture &fixture, const std::string &mate, int count) {
	std::vector<std::string> records;
	for (int i = 0; i < count; i++) {
		records.push_back(fixture.simple_read("r" + std::to_string(i) + "/" + mate, std::string(150, "ACGT"[i % 4]),
		                                      std::string(150, 'I')));
	}
	return records;
}

//! Waits up to thirty seconds for the read-ahead of `segment` to stop (ReadAheadIdle).
bool WaitIdle(const miint::SequenceReader &reader, int segment) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!reader.ReadAheadIdle(segment)) {
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

//! Runs `body` on a thread of its own, failing the run rather than hanging it if it has not
//! returned within thirty seconds. Whatever it waits on still holds its objects then, so nothing
//! can be torn down: the run aborts. `body` must not assert: Catch2 asserts on one thread only.
template <class Body>
auto WithinDeadline(const char *what, Body body) -> decltype(body()) {
	auto done = std::async(std::launch::async, std::move(body));
	if (done.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
		FAIL_CHECK(what << " has not returned after thirty seconds");
		std::abort();
	}
	return done.get();
}

miint::SequenceRecordBatch ReadWithin(miint::SequenceReader &reader, int n) {
	return WithinDeadline("read()", [&reader, n]() { return reader.read(n); });
}

std::vector<std::string> QualStrings(const std::vector<miint::QualScore> &quals) {
	std::vector<std::string> strings;
	for (const auto &qual : quals) {
		strings.push_back(qual.as_string());
	}
	return strings;
}

} // namespace

TEST_CASE("SequenceReader reads nothing ahead unless asked to", "[SequenceReader][read_ahead]") {
	TempFileFixture fixture;
	auto path = "read_ahead_off.fq";
	fixture.write_temp_fastq(path, ReadAheadRecords(fixture, "1", 1000));
	miint::SequenceReader reader(path);
	REQUIRE((reader.read(10).size() == 10));
	REQUIRE((reader.ReadAheadBytes(0) == 0));
}

TEST_CASE("SequenceReader returns the same batches with read-ahead as without", "[SequenceReader][read_ahead]") {
	// Records of many lengths, so that byte budgets end batches part-way through a poll and the
	// rest of that poll is held back for the next read, with both files of a pair.
	TempFileFixture fixture;
	auto path1 = "read_ahead_same_r1.fq";
	auto path2 = "read_ahead_same_r2.fq";
	const int count = 3000;
	std::vector<std::string> mates1;
	std::vector<std::string> mates2;
	for (int i = 0; i < count; i++) {
		const std::string id = "r" + std::to_string(i);
		const size_t length1 = 20 + (i * 37) % 400;
		const size_t length2 = 20 + (i * 53) % 400;
		mates1.push_back(fixture.simple_read(id + "/1", std::string(length1, "ACGT"[i % 4]),
		                                     std::string(length1, "IJK"[i % 3]), i % 5 == 0 ? "c" + id : ""));
		mates2.push_back(
		    fixture.simple_read(id + "/2", std::string(length2, "TGCA"[i % 4]), std::string(length2, 'H')));
	}
	fixture.write_temp_fastq(path1, mates1);
	fixture.write_temp_fastq(path2, mates2);
	// What a scan might ask for, in turn: counts and byte budgets, some below a single pair.
	const std::vector<std::pair<int, size_t>> asks = {{10, SIZE_MAX}, {100, 5000},     {7, 1000},
	                                                  {2048, 30000},  {333, SIZE_MAX}, {50, 700}};

	for (const bool paired : {false, true}) {
		CAPTURE(paired);
		const auto open = [&](bool read_ahead) {
			return paired ? std::make_unique<miint::SequenceReader>(path1, std::string(path2), false, read_ahead)
			              : std::make_unique<miint::SequenceReader>(path1, std::nullopt, false, read_ahead);
		};
		auto plain = open(false);
		auto ahead = open(true);
		size_t total = 0;
		for (size_t call = 0;; call++) {
			const auto &ask = asks[call % asks.size()];
			CAPTURE(call);
			const auto expected = plain->read(ask.first, ask.second);
			const auto got = ahead->read(ask.first, ask.second);
			REQUIRE((got.read_ids == expected.read_ids));
			REQUIRE((got.comments == expected.comments));
			REQUIRE((got.sequences1 == expected.sequences1));
			REQUIRE((got.sequences2 == expected.sequences2));
			REQUIRE((QualStrings(got.quals1) == QualStrings(expected.quals1)));
			REQUIRE((QualStrings(got.quals2) == QualStrings(expected.quals2)));
			if (expected.empty()) {
				break;
			}
			total += expected.size();
		}
		REQUIRE((total == static_cast<size_t>(count)));
	}
}

TEST_CASE("SequenceReader reads a file ahead of its reads, up to its bytes", "[SequenceReader][read_ahead]") {
	TempFileFixture fixture;
	auto path = "read_ahead_se.fq";
	// About 12 MB of records: three times what the read-ahead holds.
	const int count = 40000;
	fixture.write_temp_fastq(path, ReadAheadRecords(fixture, "1", count));
	miint::SequenceReader reader(path, std::nullopt, false, true);

	// Nothing is read ahead before the first read, which sets the byte budget it works within.
	REQUIRE((reader.ReadAheadBytes(0) == 0));
	auto first = reader.read(10);
	REQUIRE((first.size() == 10));
	// Then the file is read on while nothing is pulled, up to READ_AHEAD_BYTES, and about a
	// record past them at most.
	REQUIRE(WaitIdle(reader, 0));
	REQUIRE((reader.ReadAheadBytes(0) >= miint::SequenceReader::READ_AHEAD_BYTES));
	REQUIRE((reader.ReadAheadBytes(0) < miint::SequenceReader::READ_AHEAD_BYTES + READ_AHEAD_RECORD_BYTES));
	// Taking one poll's records leaves it short of its bytes, and it reads on at once, not at the
	// next read.
	auto second = reader.read(SHORT_RECORDS_PER_POLL);
	REQUIRE((second.size() == SHORT_RECORDS_PER_POLL));
	REQUIRE(WaitIdle(reader, 0));
	REQUIRE((reader.ReadAheadBytes(0) >= miint::SequenceReader::READ_AHEAD_BYTES));

	// Every record comes out once, in order.
	size_t total = first.size() + second.size();
	while (true) {
		auto batch = reader.read(2048);
		if (batch.empty()) {
			break;
		}
		REQUIRE((batch.read_ids[0] == "r" + std::to_string(total)));
		total += batch.size();
	}
	REQUIRE((total == static_cast<size_t>(count)));
}

TEST_CASE("SequenceReader reads both files of a pair ahead and keeps the mates in step",
          "[SequenceReader][read_ahead]") {
	TempFileFixture fixture;
	auto path1 = "read_ahead_r1.fq";
	auto path2 = "read_ahead_r2.fq";
	const int count = 40000;
	fixture.write_temp_fastq(path1, ReadAheadRecords(fixture, "1", count));
	fixture.write_temp_fastq(path2, ReadAheadRecords(fixture, "2", count));
	miint::SequenceReader reader(path1, std::string(path2), false, true);

	auto first = reader.read(10);
	REQUIRE((first.size() == 10));
	REQUIRE(WaitIdle(reader, 0));
	REQUIRE(WaitIdle(reader, 1));
	REQUIRE((reader.ReadAheadBytes(0) >= miint::SequenceReader::READ_AHEAD_BYTES));
	REQUIRE((reader.ReadAheadBytes(1) >= miint::SequenceReader::READ_AHEAD_BYTES));

	// read() checks that each pair's ids agree, so mates out of step fail the read.
	size_t total = first.size();
	while (true) {
		auto batch = reader.read(2048);
		if (batch.empty()) {
			break;
		}
		for (size_t i = 0; i < batch.size(); i++) {
			REQUIRE((batch.read_ids[i] == "r" + std::to_string(total + i)));
		}
		total += batch.size();
	}
	REQUIRE((total == static_cast<size_t>(count)));
}

TEST_CASE("SequenceReader reads ahead no more than read()'s byte budget", "[SequenceReader][read_ahead]") {
	TempFileFixture fixture;
	auto path = "read_ahead_budget.fq";
	const int count = 40000;
	fixture.write_temp_fastq(path, ReadAheadRecords(fixture, "1", count));
	miint::SequenceReader reader(path, std::nullopt, false, true);
	// A budget far below READ_AHEAD_BYTES.
	const size_t budget = 32 * 1024;

	auto first = reader.read(10, budget);
	REQUIRE((first.size() == 10));
	REQUIRE(WaitIdle(reader, 0));
	REQUIRE((reader.ReadAheadBytes(0) >= budget));
	REQUIRE((reader.ReadAheadBytes(0) < budget + READ_AHEAD_RECORD_BYTES));

	// Batches the budget ends part-way through a poll: every record still comes out once, in order.
	size_t total = first.size();
	while (true) {
		auto batch = reader.read(2048, budget);
		if (batch.empty()) {
			break;
		}
		REQUIRE((batch.read_ids[0] == "r" + std::to_string(total)));
		total += batch.size();
	}
	REQUIRE((total == static_cast<size_t>(count)));
}

TEST_CASE("SequenceReader reads long records ahead a few at a time", "[SequenceReader][read_ahead]") {
	// A poll is sized by the room left under READ_AHEAD_BYTES, not by read()'s far larger default
	// budget nor by the whole of READ_AHEAD_BYTES: records of a quarter of it are read no more than
	// four at a time, not sixteen, and the read-ahead holds no more than four of them.
	TempFileFixture fixture;
	auto path = "read_ahead_long.fa";
	const size_t length = miint::SequenceReader::READ_AHEAD_BYTES / 4;
	std::vector<std::string> records;
	for (int i = 0; i < 9; i++) {
		records.push_back(fixture.simple_fasta("r" + std::to_string(i), std::string(length, 'G')));
	}
	fixture.write_temp_fastq(path, records);
	miint::SequenceReader reader(path, std::nullopt, false, true);

	REQUIRE((reader.read(2).size() == 2));
	REQUIRE(WaitIdle(reader, 0));
	REQUIRE((reader.MaxPollCount() <= 4));
	REQUIRE((reader.ReadAheadBytes(0) < miint::SequenceReader::READ_AHEAD_BYTES + length));
}

TEST_CASE("SequenceReader gets a read every record it asks for, more than the read-ahead holds",
          "[SequenceReader][read_ahead]") {
	// Each of mate 2's records is a quarter of READ_AHEAD_BYTES: its read-ahead stops holding
	// a few of them, short of the sixteen a read asks each file for, and must read on for that
	// read. Mate 1's are short, so its read-ahead holds plenty.
	TempFileFixture fixture;
	auto path1 = "read_ahead_long_r1.fq";
	auto path2 = "read_ahead_long_r2.fq";
	const int count = 12;
	const size_t length = miint::SequenceReader::READ_AHEAD_BYTES / 8;
	std::vector<std::string> mates1;
	std::vector<std::string> mates2;
	for (int i = 0; i < count; i++) {
		const std::string id = "r" + std::to_string(i);
		mates1.push_back(fixture.simple_read(id + "/1", std::string(150, 'A'), std::string(150, 'I')));
		mates2.push_back(fixture.simple_read(id + "/2", std::string(length, 'C'), std::string(length, 'I')));
	}
	fixture.write_temp_fastq(path1, mates1);
	fixture.write_temp_fastq(path2, mates2);
	miint::SequenceReader reader(path1, std::string(path2), false, true);

	// The first record of each file was read when the reader opened; the second is the first
	// a read-ahead hands out. Mate 2's then stops before the end of its file.
	REQUIRE((reader.read(2).size() == 2));
	REQUIRE(WaitIdle(reader, 1));
	REQUIRE((reader.ReadAheadBytes(1) < (count - 3) * 2 * length));
	auto batch = ReadWithin(reader, 16);
	REQUIRE((batch.size() == static_cast<size_t>(count - 2)));
	for (size_t i = 0; i < batch.size(); i++) {
		REQUIRE((batch.read_ids[i] == "r" + std::to_string(2 + i)));
		REQUIRE((batch.sequences2[i].size() == length));
	}
	REQUIRE(ReadWithin(reader, 16).empty());
}

#ifndef _WIN32
namespace {

//! Writes `text` into the FIFO at `path` on a thread of its own. Leaving scope waits for it,
//! having first opened the FIFO for reading itself: if no reader ever did, the writer would
//! otherwise wait in its open forever. `text` must fit in the pipe's buffer, or a writer blocked
//! on a full pipe would still hang that wait: under a page (4 KiB) fits on Linux even once a
//! user's pipes are cut to their minimum.
class FifoWriter {
public:
	FifoWriter(std::string path, std::string text)
	    : path_(std::move(path)), thread_([this, text]() {
		      std::ofstream out(path_);
		      out << text;
	      }) {
	}
	~FifoWriter() {
		const int fd = open(path_.c_str(), O_RDONLY | O_NONBLOCK);
		thread_.join();
		if (fd >= 0) {
			close(fd);
		}
	}

private:
	std::string path_;
	std::thread thread_;
};

} // namespace

TEST_CASE("SequenceReader reads a file whose read-ahead thread cannot start", "[SequenceReader][read_ahead]") {
	// A process or thread limit refuses the thread: the file is then read as without read-ahead,
	// rather than failing a read that would succeed without it.
	if (geteuid() == 0) {
		SKIP("root is not held to RLIMIT_NPROC");
	}
	TempFileFixture fixture;
	auto path = "read_ahead_no_thread.fq";
	const int count = 1000;
	fixture.write_temp_fastq(path, ReadAheadRecords(fixture, "1", count));
	// In a child, so that the limit, which no unprivileged process can raise again, ends with it.
	const pid_t child = fork();
	REQUIRE((child >= 0));
	if (child == 0) {
		// This user already runs more than one process, so no new thread should start; a thread
		// that still does (a capability, or a system that counts only processes) means the
		// case cannot be tested here.
		const struct rlimit one = {1, 1};
		int status = 2;
		if (setrlimit(RLIMIT_NPROC, &one) == 0) {
			bool refused = false;
			try {
				std::thread([]() {}).join();
			} catch (const std::system_error &) {
				refused = true;
			}
			if (!refused) {
				_exit(4);
			}
			try {
				miint::SequenceReader reader(path, std::nullopt, false, true);
				size_t total = 0;
				for (auto batch = reader.read(100); !batch.empty(); batch = reader.read(100)) {
					total += batch.size();
				}
				status = total == static_cast<size_t>(count) ? 0 : 1;
			} catch (...) {
				status = 3;
			}
		}
		_exit(status);
	}
	int status = 0;
	REQUIRE((waitpid(child, &status, 0) == child));
	REQUIRE(WIFEXITED(status));
	if (WEXITSTATUS(status) == 4) {
		SKIP("RLIMIT_NPROC does not refuse a thread here");
	}
	// 1: records lost; 2: the limit could not be set; 3: the read failed.
	REQUIRE((WEXITSTATUS(status) == 0));
}

TEST_CASE("SequenceReader reads a pipe as it is asked to, never ahead", "[SequenceReader][read_ahead]") {
	// Reading a pipe ahead would block on its writer, and a reader whose writer stays open
	// could then never be dropped.
	const std::string fifo = "read_ahead.fifo";
	std::filesystem::remove(fifo);
	REQUIRE((mkfifo(fifo.c_str(), 0600) == 0));
	TempFileFixture fixture;
	std::string text;
	// Twelve records, about 3.7 KB: under a page, see FifoWriter.
	for (const auto &record : ReadAheadRecords(fixture, "1", 12)) {
		text += record;
	}
	REQUIRE((text.size() < 4096));
	{
		FifoWriter writer(fifo, text);
		miint::SequenceReader reader(fifo, std::nullopt, false, true);
		REQUIRE((reader.read(2).size() == 2));
		REQUIRE(WaitIdle(reader, 0));
		REQUIRE((reader.ReadAheadBytes(0) == 0));
		REQUIRE((reader.read(100).size() == 10));
	}
	std::filesystem::remove(fifo);
}
#endif

TEST_CASE("SequenceReader dropped while reading ahead goes once its poll returns", "[SequenceReader][read_ahead]") {
	TempFileFixture fixture;
	auto path = "read_ahead_drop.fq";
	fixture.write_temp_fastq(path, ReadAheadRecords(fixture, "1", 40000));
	const int dropped_while_reading = WithinDeadline("dropping readers", [&]() {
		int dropped = 0;
		for (int i = 0; i < 20; i++) {
			miint::SequenceReader reader(path, std::nullopt, false, true);
			// The record read when the reader opened, then one from the read-ahead, which is
			// reading on when the reader goes.
			if (reader.read(2).size() == 2 && reader.ReadAheadBytes(0) > 0) {
				dropped++;
			}
		}
		return dropped;
	});
	REQUIRE((dropped_while_reading == 20));
}

#endif
