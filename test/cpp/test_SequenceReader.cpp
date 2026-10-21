#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>
#include <fstream>
#include <filesystem>
#include "QualScore.hpp"
#include "SequenceRecord.hpp"
#include "SequenceReader.hpp"
#include <vector>
#include <zlib.h>

// getrlimit/geteuid and permission bits that actually deny the owner are POSIX-only; the
// open-failure test below is compiled out elsewhere. Same guard as test_NewickParser.cpp.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
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

	// Write `records` as one gzip member, or append them as a further member ("ab").
	void write_temp_gz(const std::string &path, const std::vector<std::string> &records, const char *mode = "wb") {
		gzFile out = gzopen(path.c_str(), mode);
		if (!out) {
			throw std::runtime_error("Failed to create temp file: " + path);
		}
		for (const auto &r : records) {
			gzwrite(out, r.data(), static_cast<unsigned>(r.size()));
		}
		gzclose(out);
		temp_files_.push_back(path);
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

// kseq++ fails its stream both on a malformed record and at the end of the input, so a reader
// that takes every failure for the end returns a short result, missing the bad record and every
// record after it, with nothing to say so. Each case must stop the read with an error naming
// the file and the record, so the user can find and fix the input.
TEST_CASE("SequenceReader rejects a quality string longer than its sequence", "[SequenceReader][error]") {
	TempFileFixture fixture;
	auto path = "malformed_long_qual.fq";
	fixture.write_temp_fastq(path, {fixture.simple_read("a", "ACGT", "IIII"), fixture.simple_read("b", "ACG", "IIIII"),
	                                fixture.simple_read("c", "ACGT", "IIII")});

	miint::SequenceReader reader(path);
	REQUIRE_THROWS_WITH(
	    reader.read(5),
	    "malformed_long_qual.fq: record 'b' (record 2): sequence length 3 does not match quality length 5");
}

TEST_CASE("SequenceReader rejects a record whose quality line is missing at end of file", "[SequenceReader][error]") {
	TempFileFixture fixture;
	auto path = "malformed_missing_qual.fq";
	fixture.write_temp_fastq(path, {fixture.simple_read("a", "ACGT", "IIII"), "@b\nACGT\n+\n"});

	miint::SequenceReader reader(path);
	REQUIRE_THROWS_WITH(
	    reader.read(5),
	    "malformed_missing_qual.fq: record 'b' (record 2): sequence length 4 does not match quality length 0");
}

TEST_CASE("SequenceReader names the mate file when only R2 is malformed", "[SequenceReader][error]") {
	TempFileFixture fixture;
	auto r1 = "malformed_mate_r1.fq";
	auto r2 = "malformed_mate_r2.fq";
	fixture.write_temp_fastq(r1,
	                         {fixture.simple_read("a/1", "ACGT", "IIII"), fixture.simple_read("b/1", "ACGT", "IIII")});
	fixture.write_temp_fastq(r2,
	                         {fixture.simple_read("a/2", "ACGT", "IIII"), fixture.simple_read("b/2", "ACGT", "III")});

	miint::SequenceReader reader(r1, r2);
	REQUIRE_THROWS_WITH(
	    reader.read(5),
	    "malformed_mate_r2.fq: record 'b/2' (record 2): sequence length 4 does not match quality length 3");
}

TEST_CASE("SequenceReader reports a malformed first record as malformed, not as an empty file",
          "[SequenceReader][error]") {
	// The first record is read while the reader is constructed, to detect the format. A
	// failure there must not read as a file with no records at all.
	TempFileFixture fixture;
	auto path = "malformed_first.fq";
	fixture.write_temp_fastq(path,
	                         {fixture.simple_read("a", "ACGT", "IIIIII"), fixture.simple_read("b", "ACGT", "IIII")});

	REQUIRE_THROWS_WITH(miint::SequenceReader(path),
	                    "malformed_first.fq: record 'a' (record 1): sequence length 4 does not match quality length 6");
}

// Records with varied sequence, so the gzip stream is long enough that cutting or damaging it
// in the middle lands well inside the compressed data rather than in the header or trailer.
static std::vector<std::string> varied_reads(TempFileFixture &fixture, int n) {
	std::vector<std::string> records;
	uint32_t state = 12345;
	for (int i = 0; i < n; i++) {
		std::string seq(100, 'A');
		for (auto &c : seq) {
			state = state * 1664525u + 1013904223u;
			c = "ACGT"[state >> 30];
		}
		records.push_back(fixture.simple_read("r" + std::to_string(i), seq, std::string(100, 'I')));
	}
	return records;
}

static size_t read_all(miint::SequenceReader &reader) {
	size_t total = 0;
	while (true) {
		auto batch = reader.read(1000);
		if (batch.empty()) {
			return total;
		}
		total += batch.size();
	}
}

TEST_CASE("SequenceReader rejects a truncated gzip file", "[SequenceReader][error][compression]") {
	// zlib does not fail a read when a gzip file ends early: it returns what it could inflate
	// and then 0, exactly as at a real end of file. Without asking zlib, a file cut in half
	// reads back as a smaller valid file.
	TempFileFixture fixture;
	auto path = "truncated.fq.gz";
	fixture.write_temp_gz(path, varied_reads(fixture, 4000));
	std::filesystem::resize_file(path, std::filesystem::file_size(path) / 2);

	miint::SequenceReader reader(path);
	REQUIRE_THROWS_WITH(read_all(reader), Catch::Matchers::StartsWith("truncated.fq.gz: read failed after record "));
}

TEST_CASE("SequenceReader rejects a corrupt gzip file", "[SequenceReader][error][compression]") {
	// Damaged deflate data can inflate to garbage before zlib notices -- its checksum is only
	// checked at the end of the member -- so the first check to trip may be a garbage record
	// rather than zlib. Either way the read must fail, naming the file.
	TempFileFixture fixture;
	auto path = "corrupt.fq.gz";
	fixture.write_temp_gz(path, varied_reads(fixture, 4000));
	{
		std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
		f.seekp(static_cast<std::streamoff>(std::filesystem::file_size(path) / 2));
		const std::string zeros(64, '\0');
		f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
	}

	miint::SequenceReader reader(path);
	REQUIRE_THROWS_WITH(read_all(reader), Catch::Matchers::StartsWith("corrupt.fq.gz: "));
}

TEST_CASE("SequenceReader rejects a FASTA record cut short by a truncated gzip file",
          "[SequenceReader][error][compression]") {
	// A FASTA record has no terminator, so one that the cut ends looks complete. Unless the
	// input is asked why it stopped before that record is kept, a read that stops there -- the
	// format peek here, or a scan under a LIMIT -- returns a truncated sequence and no error.
	TempFileFixture fixture;
	auto path = "truncated_one_record.fa.gz";
	std::string record = ">a\n";
	uint32_t state = 12345;
	for (int line = 0; line < 125; line++) {
		for (int i = 0; i < 80; i++) {
			state = state * 1664525u + 1013904223u;
			record += "ACGT"[state >> 30];
		}
		record += '\n';
	}
	fixture.write_temp_gz(path, {record});
	std::filesystem::resize_file(path, std::filesystem::file_size(path) / 2);

	REQUIRE_THROWS_WITH(miint::SequenceReader(path),
	                    "truncated_one_record.fa.gz: read failed after record 0: unexpected end of file");
}

TEST_CASE("SequenceReader reads a multi-member gzip file to the end", "[SequenceReader][compression]") {
	// Concatenated gzip members (cat a.gz b.gz, bgzip output) are one valid file. The end of a
	// member is not the end of the input, and must not be reported as a truncation.
	TempFileFixture fixture;
	auto path = "multi_member.fq.gz";
	auto records = varied_reads(fixture, 3000);
	fixture.write_temp_gz(path, std::vector<std::string>(records.begin(), records.begin() + 1000));
	fixture.write_temp_gz(path, std::vector<std::string>(records.begin() + 1000, records.end()), "ab");

	miint::SequenceReader reader(path);
	REQUIRE(read_all(reader) == 3000);
}

// FASTQ is read as strict four-line records: header, sequence, '+', quality, one line each.
// Reading wrapped FASTQ, as kseq++ does, appends quality lines until the quality is as long as
// the sequence, so a quality line that is too short takes the next record's header as quality
// and that record is lost. Requiring one quality line makes it an error at the record that is
// actually wrong. FASTA keeps wrapped sequence lines.
TEST_CASE("SequenceReader rejects a quality string shorter than its sequence", "[SequenceReader][error]") {
	TempFileFixture fixture;

	SECTION("the next header would make up the length") {
		auto path = "malformed_short_qual.fq";
		fixture.write_temp_fastq(
		    path, {fixture.simple_read("a", "ACGT", "IIII"), fixture.simple_read("b", "ACGTACGT", "IIIIII"),
		           fixture.simple_read("c", "ACGT", "IIII"), fixture.simple_read("d", "ACGT", "IIII")});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(
		    reader.read(5),
		    "malformed_short_qual.fq: record 'b' (record 2): sequence length 8 does not match quality length 6");
	}

	SECTION("the next header would overshoot the length") {
		auto path = "malformed_short_qual2.fq";
		fixture.write_temp_fastq(path,
		                         {fixture.simple_read("a", "ACGT", "IIII"), fixture.simple_read("b", "ACGTA", "III"),
		                          fixture.simple_read("c", "ACGT", "IIII")});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(
		    reader.read(5),
		    "malformed_short_qual2.fq: record 'b' (record 2): sequence length 5 does not match quality length 3");
	}
}

TEST_CASE("SequenceReader rejects wrapped FASTQ", "[SequenceReader][error]") {
	TempFileFixture fixture;

	SECTION("sequence on two lines") {
		auto path = "wrapped_seq.fq";
		fixture.write_temp_fastq(path, {fixture.simple_read("a", "ACGT", "IIII"), "@b\nACGT\nACGT\n+\nIIIIIIII\n"});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(reader.read(5), "wrapped_seq.fq: record 'b' (record 2): the sequence spans 2 lines; FASTQ "
		                                    "records must be four lines (wrapped FASTQ is not supported)");
	}

	SECTION("quality on two lines") {
		auto path = "wrapped_qual.fq";
		fixture.write_temp_fastq(path, {fixture.simple_read("a", "ACGT", "IIII"), "@b\nACGTACGT\n+\nIIII\nIIII\n"});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(
		    reader.read(5),
		    "wrapped_qual.fq: record 'b' (record 2): sequence length 8 does not match quality length 4");
	}
}

TEST_CASE("SequenceReader rejects a line between FASTQ records that is not a header", "[SequenceReader][error]") {
	// Skipping anything up to the next '@', as kseq++ does, silently drops a record whose header
	// line has lost its '@'.
	TempFileFixture fixture;
	auto path = "stray_line.fq";
	fixture.write_temp_fastq(path, {fixture.simple_read("a", "ACGT", "IIII"), "b\nACGT\n+\nIIII\n",
	                                fixture.simple_read("c", "ACGT", "IIII")});
	miint::SequenceReader reader(path);
	REQUIRE_THROWS_WITH(reader.read(5), "stray_line.fq: the line after record 1 is not a record header: 'b'");
}

TEST_CASE("SequenceReader accepts blank lines between and after FASTQ records", "[SequenceReader]") {
	TempFileFixture fixture;
	auto path = "blank_lines.fq";
	fixture.write_temp_fastq(path,
	                         {"@a\nACGT\n+\nIIII\n\n", "@b\r\nTTGG\r\n+\r\nHHHH\r\n\r\n", "@c\nCCAA\n+\nGGGG\n\n\n"});
	miint::SequenceReader reader(path);
	auto batch = reader.read(5);
	REQUIRE(batch.size() == 3);
	CHECK(batch.read_ids[1] == "b");
	CHECK(batch.sequences1[1] == "TTGG");
	CHECK(batch.quals1[1].as_string() == "HHHH");
	CHECK(batch.read_ids[2] == "c");
}

TEST_CASE("SequenceReader still skips anything before the first FASTA header", "[SequenceReader][FASTA]") {
	// Old FASTA files may open with ';' comment lines; kseq++ has always skipped them, and
	// strict FASTQ does not change FASTA.
	TempFileFixture fixture;
	auto path = "leading_comment.fa";
	fixture.write_temp_fastq(path, {";a comment line\n", fixture.simple_fasta("a", "ACGT"), ">b\nAC\nGT\n"});
	miint::SequenceReader reader(path);
	auto batch = reader.read(5);
	REQUIRE(batch.size() == 2);
	CHECK(batch.read_ids[0] == "a");
	CHECK(batch.sequences1[1] == "ACGT");
}

TEST_CASE("SequenceReader rejects a FASTQ record that ends before its '+' line", "[SequenceReader][error]") {
	// An uncompressed file cut short has no gzip trailer to say so, and a cut inside the last
	// record's sequence leaves a record that looks like FASTA: read as one, it comes back with
	// part of its sequence and no quality. A '@' header starts a FASTQ record, which has four lines.
	TempFileFixture fixture;

	SECTION("the input ends inside the sequence") {
		auto path = "cut_in_seq.fq";
		fixture.write_temp_fastq(path, {fixture.simple_read("a", "ACGT", "IIII"), "@b\nACG"});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(reader.read(5), "cut_in_seq.fq: record 'b' (record 2): the input ends before the "
		                                    "record's '+' line; FASTQ records must be four lines");
	}

	SECTION("the input ends after the header") {
		auto path = "cut_after_header.fq";
		fixture.write_temp_fastq(path, {fixture.simple_read("a", "ACGT", "IIII"), "@b\n"});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(reader.read(5), "cut_after_header.fq: record 'b' (record 2): the input ends before the "
		                                    "record's '+' line; FASTQ records must be four lines");
	}

	SECTION("the input ends just after the header's '@'") {
		auto path = "cut_after_at.fq";
		fixture.write_temp_fastq(path, {fixture.simple_read("a", "ACGT", "IIII"), "@"});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(reader.read(5), "cut_after_at.fq: record 2: the input ends inside its header");
	}

	SECTION("the next record starts instead") {
		auto path = "no_plus.fq";
		fixture.write_temp_fastq(
		    path, {fixture.simple_read("a", "ACGT", "IIII"), "@b\nACGT\n", fixture.simple_read("c", "ACGT", "IIII")});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(reader.read(5), "no_plus.fq: record 'b' (record 2): no '+' line before the next record; "
		                                    "FASTQ records must be four lines");
	}
}

TEST_CASE("SequenceReader rejects a NUL byte where a record should start", "[SequenceReader][error]") {
	// kseq++'s getc() returns 0 both at the end of the input and for a NUL byte. Unless the two
	// are told apart, a zero-filled block -- what a crash can leave in a file -- reads as the end
	// of the file, and every record after it is lost without a word.
	TempFileFixture fixture;
	const std::string zeros(64, '\0');

	SECTION("after a FASTQ record") {
		auto path = "nul_after_fastq.fq";
		fixture.write_temp_fastq(
		    path, {fixture.simple_read("a", "ACGT", "IIII"), zeros, fixture.simple_read("b", "ACGT", "IIII")});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(reader.read(5), "nul_after_fastq.fq: unexpected NUL byte after record 1");
	}

	SECTION("after a FASTA record") {
		auto path = "nul_after_fasta.fa";
		fixture.write_temp_fastq(path, {fixture.simple_fasta("a", "ACGT"), zeros, fixture.simple_fasta("b", "ACGT")});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(reader.read(5), "nul_after_fasta.fa: unexpected NUL byte after record 1");
	}

	SECTION("inside a FASTQ record, named as a NUL") {
		auto path = "nul_in_fastq.fq";
		fixture.write_temp_fastq(path, {fixture.simple_read("a", "ACGT", "IIII"), "@b\nACGT\n", zeros});
		miint::SequenceReader reader(path);
		REQUIRE_THROWS_WITH(reader.read(5),
		                    "nul_in_fastq.fq: record 'b' (record 2): unexpected NUL byte before the record's '+' line");
	}

	SECTION("a file of nothing but NUL bytes is not an empty file") {
		auto path = "all_nul.fq";
		fixture.write_temp_fastq(path, {zeros});
		REQUIRE_THROWS_WITH(miint::SequenceReader(path), "all_nul.fq: unexpected NUL byte after record 0");
	}
}

TEST_CASE("SequenceReader takes a line of only spaces or tabs between FASTQ records as blank", "[SequenceReader]") {
	// kseq++ skips such lines, so files that have them must keep reading.
	TempFileFixture fixture;
	auto path = "whitespace_lines.fq";
	fixture.write_temp_fastq(path, {"@a\nACGT\n+\nIIII\n  \n", "@b\nTTGG\n+\nHHHH\n\t\n \r\n"});
	miint::SequenceReader reader(path);
	auto batch = reader.read(5);
	REQUIRE(batch.size() == 2);
	CHECK(batch.read_ids[1] == "b");
	CHECK(batch.quals1[1].as_string() == "HHHH");
}

TEST_CASE("SequenceReader reads a blank line inside a CRLF FASTQ record as it does an LF one", "[SequenceReader]") {
	// A blank line inside a record is skipped. With CRLF endings it is a lone '\r', which must
	// not count as a second sequence line and get the record rejected as wrapped FASTQ.
	TempFileFixture fixture;
	auto path = "blank_in_record.fq";
	fixture.write_temp_fastq(path, {"@a\n\nACGT\n+\nIIII\n", "@b\r\n\r\nTTGG\r\n+\r\nHHHH\r\n"});
	miint::SequenceReader reader(path);
	auto batch = reader.read(5);
	REQUIRE(batch.size() == 2);
	CHECK(batch.sequences1[0] == "ACGT");
	CHECK(batch.sequences1[1] == "TTGG");
	CHECK(batch.quals1[1].as_string() == "HHHH");
}

TEST_CASE("SequenceReader rejects text before the first FASTQ record", "[SequenceReader][error]") {
	// Skipping to the first header is for FASTA's ';' comments. FASTQ has no preamble, so text
	// there is a first record that has lost its '@', and skipping it would lose the record.
	TempFileFixture fixture;

	SECTION("the first header has lost its '@'") {
		auto path = "lost_first_header.fq";
		fixture.write_temp_fastq(path, {"a\nACGT\n+\nIIII\n", fixture.simple_read("b", "ACGT", "IIII")});
		REQUIRE_THROWS_WITH(miint::SequenceReader(path),
		                    "lost_first_header.fq: text before the first record; FASTQ files must start with a '@' "
		                    "header line");
	}

	SECTION("the skip would stop at a quality line starting with '@'") {
		auto path = "lost_first_header_at_qual.fq";
		fixture.write_temp_fastq(path, {"a\nACGT\n+\n@III\n", fixture.simple_read("b", "ACGT", "IIII")});
		REQUIRE_THROWS_WITH(miint::SequenceReader(path),
		                    "lost_first_header_at_qual.fq: text before the first record; FASTQ files must start with "
		                    "a '@' header line");
	}

	SECTION("blank lines before the first record are not text") {
		auto path = "leading_blank_lines.fq";
		fixture.write_temp_fastq(path, {"\n \r\n", fixture.simple_read("a", "ACGT", "IIII")});
		miint::SequenceReader reader(path);
		REQUIRE(reader.read(5).size() == 1);
	}
}
