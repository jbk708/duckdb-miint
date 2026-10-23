#include <SequenceReader.hpp>
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef MIINT_STATIC_BUILD
#include "duckdb/common/exception.hpp"
#endif

namespace miint {

gzFile CheckedSeqStreamIn::OpenOrThrow(const std::string &path) {
	// Cleared first because zlib does not promise to set errno on every failure path
	// (an allocation failure inside gz_open can return NULL without touching it).
	errno = 0;
	gzFile handle = gzopen(path.c_str(), "r");
	if (handle == nullptr) {
		const int open_errno = errno;
		// generic_category().message() rather than strerror(): readers are opened from
		// several scan threads at once, and strerror's shared buffer is not thread-safe.
		throw std::runtime_error(
		    "Failed to open file: " + path + ": " +
		    (open_errno != 0 ? std::generic_category().message(open_errno) : std::string("gzopen failed")));
	}
	return handle;
}

// Helper function to extract base read ID by stripping /[1-9] suffix and comments
static std::string base_read_id(const std::string &id) {
	// First, strip comments (everything after first space)
	size_t space_pos = id.find(' ');
	std::string base = (space_pos == std::string::npos) ? id : id.substr(0, space_pos);

	// Check if it ends with /[1-9] (single digit 1-9 only)
	// Need at least 3 chars for pattern "x/1"
	if (base.length() >= 3) {
		size_t len = base.length();
		char last_char = base[len - 1];
		char second_last = base[len - 2];

		// If ends with /[1-9], strip that suffix
		if (second_last == '/' && last_char >= '1' && last_char <= '9') {
			base = base.substr(0, len - 2);
		}
	}

	return base;
}

// Helper function to check if two read IDs match after normalization
static void check_ids(const std::string &name1, const std::string &name2) {
	std::string base_id1 = base_read_id(name1);
	std::string base_id2 = base_read_id(name2);

	if (base_id1 != base_id2) {
		throw std::runtime_error("Mismatched read IDs: " + name1 + " vs " + name2);
	}
}

// Validate that paired-end files use the same format (both FASTA or both FASTQ)
static void validate_format_consistency(bool is_fasta1, bool is_fasta2, bool allow_format_mismatch) {
	if (!allow_format_mismatch && is_fasta1 != is_fasta2) {
		throw std::runtime_error("Cannot mix FASTA and FASTQ formats: sequence1 is " +
		                         std::string(is_fasta1 ? "FASTA" : "FASTQ") + ", sequence2 is " +
		                         std::string(is_fasta2 ? "FASTA" : "FASTQ"));
	}
}

// Read from whichever stream variant is active.
//
// Errors that originate in our kseq++ read callbacks (duckdb_seq_read /
// aspera_seq_read) are reported via the thread-local g_seq_read_error
// channel: the callback writes a message and returns -1, kseq++ surfaces -1
// as err()/empty-result, and we raise IOException from here. This keeps the
// throw out of kseq++'s template-instantiated frames, which previously caused
// a recursive-terminate abort on the aspera path under some failure modes.
std::vector<klibpp::KSeq> SequenceReader::read_stream(StreamVar &var, int n) {
#ifdef MIINT_STATIC_BUILD
	g_seq_read_error.clear();
#endif
	auto result = std::visit(
	    [n](auto &reader) { return reader->read(static_cast<std::vector<klibpp::KSeq>::size_type>(n)); }, var);
#ifdef MIINT_STATIC_BUILD
	if (!g_seq_read_error.empty()) {
		std::string msg = std::move(g_seq_read_error);
		g_seq_read_error.clear();
		throw duckdb::IOException(msg);
	}
#endif
	return result;
}

// Records polled per stream call. Larger values amortize per-call overhead (vector
// allocation) across more records; smaller values cap the transient memory overshoot
// when a single poll pulls in records that together exceed max_bytes. 16 is a
// compromise: for short-read FASTQ a 2048-record chunk needs ~128 polls instead of
// 2048 (cuts ~94% of per-record allocation overhead), while for bacterial genomes
// (~5 MB/record) the worst-case overshoot is 16 * 5 MB = 80 MB -- negligible next to
// a 512 MB default budget. Any tail records beyond the stop point are pushed back
// into buffered_read1_ / buffered_read2_ for the next call, so nothing is dropped.
static constexpr int POLL_CHUNK_SIZE = 16;

// Bytes contributed by a single record. Counted against the per-chunk budget so that
// FASTQ quality (and paired mates) are accounted for alongside the sequence itself.
static size_t record_bytes(const klibpp::KSeq &rec) {
	return rec.name.size() + rec.comment.size() + rec.seq.size() + rec.qual.size();
}

// Size the next poll so a single read_stream call materializes at most ~max_bytes worth of
// records, rather than a fixed POLL_CHUNK_SIZE that ignores record size. Without a byte budget
// (SIZE_MAX) or before any record is observed, fall back to POLL_CHUNK_SIZE -- preserving the
// short-read fast path. Never exceeds POLL_CHUNK_SIZE and never returns less than 1. Capped at
// `remaining` by the caller's batch limit.
static int dynamic_poll_n(size_t observed_record_bytes, size_t max_bytes, int remaining) {
	int poll_n = POLL_CHUNK_SIZE;
	if (observed_record_bytes > 0 && max_bytes != SIZE_MAX) {
		const size_t by_budget = max_bytes / observed_record_bytes;
		if (by_budget < static_cast<size_t>(POLL_CHUNK_SIZE)) {
			poll_n = by_budget < 1 ? 1 : static_cast<int>(by_budget);
		}
	}
	return remaining < poll_n ? remaining : poll_n;
}

// Reads one stream ahead of read(), on a thread of its own, into a queue of records. Take(n)
// hands records out as read_stream(n) would return them: in order, and n of them, fewer only at
// the end of the stream. The thread stops while the queue holds READ_AHEAD_BYTES, or read()'s
// budget if that is smaller, unless read() is waiting on more records than that. It sizes each
// poll by that same cap and the last record read, as read() sizes its polls by its budget: a
// poll passes the cap by about a record, or by more where records suddenly grow. An error
// the stream raises is raised by the first Take the records before it cannot satisfy. As with
// read_stream, a poll that fails loses the records it read before the error, and since the
// thread's polls are not read()'s, read() can fail a few records sooner than without it.
class SequenceReader::ReadAhead {
public:
	ReadAhead(StreamVar &stream, size_t peeked_record_bytes, size_t max_bytes)
	    : stream_(stream), observed_record_bytes_(peeked_record_bytes), budget_(max_bytes) {
		thread_ = std::thread([this]() { Run(); });
	}

	~ReadAhead() {
		{
			std::lock_guard<std::mutex> guard(lock_);
			stopping_ = true;
		}
		progress_.notify_all();
		// A poll in flight finishes first: a regular file's read returns.
		thread_.join();
	}

	std::vector<klibpp::KSeq> Take(int n, size_t max_bytes) {
		std::unique_lock<std::mutex> guard(lock_);
		budget_ = max_bytes;
		wanted_ = static_cast<size_t>(n);
		progress_.notify_all();
		progress_.wait(guard, [&]() { return queue_.size() >= wanted_ || ended_; });
		wanted_ = 0;
		if (queue_.size() < static_cast<size_t>(n) && error_) {
			std::rethrow_exception(error_);
		}
		const size_t count = std::min(queue_.size(), static_cast<size_t>(n));
		std::vector<klibpp::KSeq> taken;
		taken.reserve(count);
		for (size_t i = 0; i < count; i++) {
			queued_bytes_ -= record_bytes(queue_.front());
			taken.push_back(std::move(queue_.front()));
			queue_.pop_front();
		}
		progress_.notify_all();
		return taken;
	}

	size_t QueuedBytes() {
		std::lock_guard<std::mutex> guard(lock_);
		return queued_bytes_;
	}

	size_t MaxPollCount() {
		std::lock_guard<std::mutex> guard(lock_);
		return max_poll_count_;
	}

	bool Idle() {
		std::lock_guard<std::mutex> guard(lock_);
		return ended_ || (waiting_ && !WantsMoreLocked());
	}

private:
	size_t CapLocked() const {
		return std::min(READ_AHEAD_BYTES, budget_);
	}

	bool WantsMoreLocked() const {
		return queue_.size() < wanted_ || queued_bytes_ < CapLocked();
	}

	void Run() {
		// Whatever is thrown here goes to read(): escaping the thread would end the process.
		try {
			ReadUntilStopped();
		} catch (...) {
			std::lock_guard<std::mutex> guard(lock_);
			error_ = std::current_exception();
			ended_ = true;
			progress_.notify_all();
		}
	}

	void ReadUntilStopped() {
		for (;;) {
			int poll_n;
			{
				std::unique_lock<std::mutex> guard(lock_);
				waiting_ = true;
				progress_.wait(guard, [&]() { return stopping_ || WantsMoreLocked(); });
				waiting_ = false;
				if (stopping_) {
					return;
				}
				poll_n = dynamic_poll_n(observed_record_bytes_, CapLocked(), POLL_CHUNK_SIZE);
			}
			auto chunk = read_stream(stream_, poll_n);
			std::lock_guard<std::mutex> guard(lock_);
			max_poll_count_ = std::max(max_poll_count_, chunk.size());
			if (chunk.empty()) {
				ended_ = true;
				progress_.notify_all();
				return;
			}
			for (auto &rec : chunk) {
				observed_record_bytes_ = record_bytes(rec);
				queued_bytes_ += observed_record_bytes_;
				queue_.push_back(std::move(rec));
			}
			progress_.notify_all();
		}
	}

	StreamVar &stream_;
	std::mutex lock_;
	std::condition_variable progress_;
	// Guarded by lock_.
	std::deque<klibpp::KSeq> queue_;
	size_t queued_bytes_ = 0;
	size_t observed_record_bytes_;
	size_t budget_;
	//! Records read() is waiting on; the queue fills to at least this many.
	size_t wanted_ = 0;
	//! The thread is waiting for the queue to want more records.
	bool waiting_ = false;
	bool ended_ = false;
	bool stopping_ = false;
	std::exception_ptr error_;
	size_t max_poll_count_ = 0;
	// Started last, once everything it reads is initialized.
	std::thread thread_;
};

SequenceReader::SequenceReader(const std::string &path1, const std::optional<std::string> &path2,
                               bool allow_format_mismatch, bool read_ahead)
    : first_read_(true) {
	sequence1_reader_ = std::make_unique<SeqStreamIn>(path1);

	// Check if first file is empty by attempting to peek at first record
	buffered_read1_ = read_stream(sequence1_reader_, 1);
	bool is_empty1 = buffered_read1_.empty();
	bool is_fasta1 = !is_empty1 && buffered_read1_[0].qual.empty();

	if (is_empty1) {
		throw std::runtime_error("Empty file: " + path1);
	}

	// Keep buffered_read1_ to return on first read() call instead of recreating reader

	paired_ = path2.has_value() && (path2->length() > 0);
	if (paired_) {
		sequence2_reader_.emplace(std::make_unique<SeqStreamIn>(path2.value()));

		// Check if second file is empty and detect format
		buffered_read2_ = read_stream(sequence2_reader_.value(), 1);
		bool is_empty2 = buffered_read2_.empty();
		bool is_fasta2 = !is_empty2 && buffered_read2_[0].qual.empty();

		if (is_empty2) {
			throw std::runtime_error("Empty file: " + path2.value());
		}

		validate_format_consistency(is_fasta1, is_fasta2, allow_format_mismatch);

		// Keep buffered_read2_ to return on first read() call instead of recreating reader
	}

#ifndef __EMSCRIPTEN__
	// Regular files only (see READ_AHEAD_BYTES). Not in wasm, where a thread is a Web Worker
	// that may not start while the thread that asked for it waits on it.
	const auto regular = [](const std::string &path) {
		std::error_code ec;
		return std::filesystem::is_regular_file(path, ec);
	};
	read_ahead_enabled_ = read_ahead && regular(path1) && (!paired_ || regular(path2.value()));
	peeked_record_bytes_[0] = record_bytes(buffered_read1_[0]);
	if (paired_) {
		peeked_record_bytes_[1] = record_bytes(buffered_read2_[0]);
	}
#endif
}

#ifdef MIINT_STATIC_BUILD
SequenceReader::SequenceReader(DuckDBSeqStreamHandle stream1, DuckDBSeqStreamHandle stream2_or_null,
                               bool allow_format_mismatch)
    : first_read_(true) {
	// Ownership protocol: each input handle owns its raw pointer until the
	// matching make_unique<KStreamIn>() succeeds. Holding the kstream in a
	// local before `release()` and the variant move-assign closes any window
	// where two owners could exist: if make_unique throws, the input handle
	// still owns the stream and frees it on unwind; if make_unique succeeds,
	// `release()` runs before any variant op, and the variant move-assign
	// from rvalue unique_ptr is noexcept. On any later peek throwing, the
	// kstream wrapper (now inside the variant) is the sole owner.
	auto kstream1 = std::make_unique<DuckDBSeqStreamIn>(stream1.get(), duckdb_seq_read, duckdb_seq_close);
	stream1.release();
	sequence1_reader_ = std::move(kstream1);

	buffered_read1_ = read_stream(sequence1_reader_, 1);
	bool is_empty1 = buffered_read1_.empty();
	bool is_fasta1 = !is_empty1 && buffered_read1_[0].qual.empty();

	if (is_empty1) {
		throw std::runtime_error("Empty stream");
	}

	paired_ = (stream2_or_null.get() != nullptr);
	if (paired_) {
		auto kstream2 = std::make_unique<DuckDBSeqStreamIn>(stream2_or_null.get(), duckdb_seq_read, duckdb_seq_close);
		stream2_or_null.release();
		sequence2_reader_.emplace(std::move(kstream2));

		buffered_read2_ = read_stream(sequence2_reader_.value(), 1);
		bool is_empty2 = buffered_read2_.empty();
		bool is_fasta2 = !is_empty2 && buffered_read2_[0].qual.empty();

		if (is_empty2) {
			throw std::runtime_error("Empty stream (sequence2)");
		}

		validate_format_consistency(is_fasta1, is_fasta2, allow_format_mismatch);
	}
}

#if MIINT_ASPERA_SUPPORTED
SequenceReader::SequenceReader(AsperaSeqStreamHandle stream1, AsperaSeqStreamHandle stream2_or_null,
                               bool allow_format_mismatch)
    : first_read_(true) {
	// See ownership protocol on the DuckDB+DuckDB ctor above. Unlike that
	// ctor, both kstream wrappers are constructed up front before either
	// peek: Aspera streams are backed by live ascp pipes, and ordering the
	// transfers/peeks per-stream would interleave reads from the two
	// processes in ways the original (pre-handle) code took care to avoid.
	auto kstream1 = std::make_unique<AsperaSeqStreamIn>(stream1.get(), aspera_seq_read, aspera_seq_close);
	stream1.release();
	sequence1_reader_ = std::move(kstream1);

	paired_ = (stream2_or_null.get() != nullptr);
	if (paired_) {
		auto kstream2 = std::make_unique<AsperaSeqStreamIn>(stream2_or_null.get(), aspera_seq_read, aspera_seq_close);
		stream2_or_null.release();
		sequence2_reader_.emplace(std::move(kstream2));
	}

	buffered_read1_ = read_stream(sequence1_reader_, 1);
	if (buffered_read1_.empty()) {
		throw std::runtime_error("Empty stream");
	}
	bool is_fasta1 = buffered_read1_[0].qual.empty();

	if (paired_) {
		buffered_read2_ = read_stream(sequence2_reader_.value(), 1);
		if (buffered_read2_.empty()) {
			throw std::runtime_error("Empty stream (sequence2)");
		}
		bool is_fasta2 = buffered_read2_[0].qual.empty();

		validate_format_consistency(is_fasta1, is_fasta2, allow_format_mismatch);
	}
}
#endif // MIINT_ASPERA_SUPPORTED
#endif // MIINT_STATIC_BUILD

SequenceReader::~SequenceReader() = default;

std::vector<klibpp::KSeq> SequenceReader::poll(int segment, int n, size_t max_bytes) {
	StreamVar &stream = segment == 0 ? sequence1_reader_ : sequence2_reader_.value();
	auto &ahead = read_ahead_[segment];
	if (read_ahead_enabled_ && !ahead && !read_ahead_unavailable_[segment]) {
		// Started here rather than at construction: the first poll brings read()'s budget.
		try {
			ahead = std::make_unique<ReadAhead>(stream, peeked_record_bytes_[segment], max_bytes);
		} catch (const std::system_error &) {
			// No thread to be had (a process or thread limit): this segment is read here, as
			// without read-ahead. The other segment's read-ahead, if it started, carries on.
			read_ahead_unavailable_[segment] = true;
		}
	}
	if (!ahead) {
		auto chunk = read_stream(stream, n);
		max_poll_count_ = std::max(max_poll_count_, chunk.size());
		return chunk;
	}
	return ahead->Take(n, max_bytes);
}

size_t SequenceReader::MaxPollCount() const {
	size_t polled = max_poll_count_;
	for (const auto &ahead : read_ahead_) {
		if (ahead) {
			polled = std::max(polled, ahead->MaxPollCount());
		}
	}
	return polled;
}

size_t SequenceReader::ReadAheadBytes(int segment) const {
	return read_ahead_[segment] ? read_ahead_[segment]->QueuedBytes() : 0;
}

bool SequenceReader::ReadAheadIdle(int segment) const {
	return read_ahead_[segment] ? read_ahead_[segment]->Idle() : true;
}

// Append a KSeq into an SE batch, stripping sequence whitespace in place.
static void append_se(SequenceRecordBatch &batch, klibpp::KSeq &rec) {
	rec.seq.erase(std::remove_if(rec.seq.begin(), rec.seq.end(), [](unsigned char c) { return std::isspace(c); }),
	              rec.seq.end());
	batch.read_ids.emplace_back(base_read_id(rec.name));
	batch.comments.emplace_back(rec.comment);
	batch.sequences1.emplace_back(std::move(rec.seq));
	batch.quals1.emplace_back(rec.qual);
}

SequenceRecordBatch SequenceReader::read_se(const int n, const size_t max_bytes) {
	SequenceRecordBatch batch(false);
	batch.reserve(n);

	size_t cumulative_bytes = 0;

	// Drain a polled chunk into `batch`, pushing any unused tail into buffered_read1_ when
	// a stop condition (budget hit or batch full) fires mid-chunk. Returns true when the
	// whole chunk was consumed and caller should poll again, false otherwise.
	auto drain_chunk = [&](std::vector<klibpp::KSeq> &chunk) -> bool {
		for (size_t i = 0; i < chunk.size(); i++) {
			auto &rec = chunk[i];
			const size_t rec_bytes = record_bytes(rec);
			// Starvation guard: if batch is still empty, accept at least one record even
			// when it alone exceeds max_bytes -- otherwise the pipeline stalls.
			if (!batch.empty() && cumulative_bytes + rec_bytes > max_bytes) {
				buffered_read1_.clear();
				buffered_read1_.reserve(chunk.size() - i);
				for (size_t j = i; j < chunk.size(); j++) {
					buffered_read1_.push_back(std::move(chunk[j]));
				}
				first_read_ = true;
				return false;
			}
			append_se(batch, rec);
			cumulative_bytes += rec_bytes;
			observed_record_bytes_ = rec_bytes;
			const bool done = (int)batch.size() >= n || cumulative_bytes >= max_bytes;
			if (done) {
				if (i + 1 < chunk.size()) {
					buffered_read1_.clear();
					buffered_read1_.reserve(chunk.size() - (i + 1));
					for (size_t j = i + 1; j < chunk.size(); j++) {
						buffered_read1_.push_back(std::move(chunk[j]));
					}
					first_read_ = true;
				}
				return false;
			}
		}
		return true;
	};

	// Replay pre-buffered records first (peeked at construction, or tail from a prior call).
	if (first_read_) {
		first_read_ = false;
		auto buffered = std::move(buffered_read1_);
		buffered_read1_.clear();
		if (!drain_chunk(buffered)) {
			return batch;
		}
	}

	// Poll the stream in chunks so the budget can short-circuit before the next I/O round.
	while ((int)batch.size() < n && cumulative_bytes < max_bytes) {
		const int remaining = n - (int)batch.size();
		const int poll_n = dynamic_poll_n(observed_record_bytes_, max_bytes, remaining);
		auto more = poll(0, poll_n, max_bytes);
		if (more.empty()) {
			break;
		}
		if (!drain_chunk(more)) {
			break;
		}
	}

	return batch;
}

// Append a paired KSeq into a PE batch, stripping sequence whitespace on both mates.
static void append_pe(SequenceRecordBatch &batch, klibpp::KSeq &rec1, klibpp::KSeq &rec2) {
	rec1.seq.erase(std::remove_if(rec1.seq.begin(), rec1.seq.end(), [](unsigned char c) { return std::isspace(c); }),
	               rec1.seq.end());
	rec2.seq.erase(std::remove_if(rec2.seq.begin(), rec2.seq.end(), [](unsigned char c) { return std::isspace(c); }),
	               rec2.seq.end());

	batch.read_ids.emplace_back(base_read_id(rec1.name));
	batch.comments.emplace_back(rec1.comment);
	batch.sequences1.emplace_back(std::move(rec1.seq));
	batch.sequences2.emplace_back(std::move(rec2.seq));
	batch.quals1.emplace_back(rec1.qual);
	batch.quals2.emplace_back(rec2.qual);
}

SequenceRecordBatch SequenceReader::read_pe(const int n, const size_t max_bytes) {
	SequenceRecordBatch batch(true);
	batch.reserve(n);

	size_t cumulative_bytes = 0;

	// Push [i..end) of both chunks into buffered_read1_ / buffered_read2_ so the next call
	// replays them. Kept in a helper to keep mate buffers in lockstep.
	auto push_back_tail = [&](std::vector<klibpp::KSeq> &c1, std::vector<klibpp::KSeq> &c2, size_t i) {
		buffered_read1_.clear();
		buffered_read2_.clear();
		const size_t tail = c1.size() - i;
		buffered_read1_.reserve(tail);
		buffered_read2_.reserve(tail);
		for (size_t j = i; j < c1.size(); j++) {
			buffered_read1_.push_back(std::move(c1[j]));
			buffered_read2_.push_back(std::move(c2[j]));
		}
		first_read_ = true;
	};

	// Drain paired chunks into `batch`, validating mate-id parity and enforcing the budget.
	auto drain_chunks = [&](std::vector<klibpp::KSeq> &c1, std::vector<klibpp::KSeq> &c2) -> bool {
		if (c1.size() != c2.size()) {
			const std::string &id = !c1.empty() ? c1.back().name : c2.back().name;
			throw std::runtime_error("Mismatched number of records: missing mate for " + id);
		}
		for (size_t i = 0; i < c1.size(); i++) {
			auto &rec1 = c1[i];
			auto &rec2 = c2[i];
			check_ids(rec1.name, rec2.name);
			const size_t pair_bytes = record_bytes(rec1) + record_bytes(rec2);
			if (!batch.empty() && cumulative_bytes + pair_bytes > max_bytes) {
				push_back_tail(c1, c2, i);
				return false;
			}
			append_pe(batch, rec1, rec2);
			cumulative_bytes += pair_bytes;
			observed_record_bytes_ = pair_bytes;
			const bool done = (int)batch.size() >= n || cumulative_bytes >= max_bytes;
			if (done) {
				if (i + 1 < c1.size()) {
					push_back_tail(c1, c2, i + 1);
				}
				return false;
			}
		}
		return true;
	};

	if (first_read_) {
		first_read_ = false;
		auto buf1 = std::move(buffered_read1_);
		auto buf2 = std::move(buffered_read2_);
		buffered_read1_.clear();
		buffered_read2_.clear();

		if (buf1.size() != buf2.size()) {
			// Should not happen -- peek-at-construction and push_back_tail keep them in sync.
			throw std::runtime_error("Buffered paired records out of sync (internal error)");
		}
		if (!drain_chunks(buf1, buf2)) {
			return batch;
		}
	}

	while ((int)batch.size() < n && cumulative_bytes < max_bytes) {
		const int remaining = n - (int)batch.size();
		const int poll_n = dynamic_poll_n(observed_record_bytes_, max_bytes, remaining);
		auto more1 = poll(0, poll_n, max_bytes);
		auto more2 = poll(1, poll_n, max_bytes);

		if (more1.empty() && more2.empty()) {
			break;
		}
		if (!drain_chunks(more1, more2)) {
			break;
		}
	}

	return batch;
}

SequenceRecordBatch SequenceReader::read(const int n, const size_t max_bytes) {
	if (paired_) {
		return read_pe(n, max_bytes);
	} else {
		return read_se(n, max_bytes);
	}
}
}; // namespace miint
