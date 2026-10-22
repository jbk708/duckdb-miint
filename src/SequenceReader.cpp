#include <SequenceReader.hpp>
#include <algorithm>
#include <cerrno>
#include <string_view>
#include <type_traits>
#include <system_error>

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

std::string CheckedSeqStreamIn::EndOfInputError() const {
	int errnum = Z_OK;
	const char *msg = gzerror(this->f, &errnum);
	if (errnum == Z_OK) {
		return std::string();
	}
	std::string error = msg != nullptr ? msg : "zlib error " + std::to_string(errnum);
	// gzerror() leads with the path the file was opened with; the caller names the file itself.
	const std::string prefix = path_ + ": ";
	if (error.compare(0, prefix.size(), prefix) == 0) {
		error.erase(0, prefix.size());
	}
	return error;
}

// Helper function to extract base read ID by stripping /[1-9] suffix and comments.
// Returns a view into `id`, so comparing two IDs allocates nothing.
static std::string_view base_read_id(std::string_view id) {
	// First, strip comments (everything after first space)
	std::string_view base = id.substr(0, id.find(' '));

	// Check if it ends with /[1-9] (single digit 1-9 only)
	// Need at least 3 chars for pattern "x/1"
	if (base.length() >= 3) {
		size_t len = base.length();
		char last_char = base[len - 1];
		char second_last = base[len - 2];

		// If ends with /[1-9], strip that suffix
		if (second_last == '/' && last_char >= '1' && last_char <= '9') {
			base.remove_suffix(2);
		}
	}

	return base;
}

// Helper function to check if two read IDs match after normalization
static void check_ids(const std::string &name1, const std::string &name2) {
	if (base_read_id(name1) != base_read_id(name2)) {
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

// The bytes std::isspace accepts in the "C" locale: ' ' and '\t' through '\r'. Spelled out
// rather than calling std::isspace, which is an out-of-line, locale-dependent call per byte,
// and branch-free, so the loops over a whole sequence that use it vectorize.
static inline bool is_ascii_space(unsigned char c) {
	return (c == ' ') | (static_cast<unsigned char>(c - '\t') <= '\r' - '\t');
}

// kseq++'s KStream separators, protected there: split at any whitespace, or read to the end
// of the line (dropping a trailing '\r').
static constexpr char SEP_SPACE = 0;
static constexpr char SEP_LINE = 2;

// The first byte of a FASTA header line, of a FASTQ header line, and of the FASTQ line between
// the sequence and the quality.
static constexpr char FASTA_HEADER = '>';
static constexpr char FASTQ_HEADER = '@';
static constexpr char FASTQ_SEPARATOR = '+';

// Parse one record over kseq++'s buffered getc/getuntil. A FASTA record is read as kseq++'s own
// parser reads it, wrapped sequence lines included. A record whose header starts with '@' is
// FASTQ and is exactly four lines: header, one sequence line, '+', and one quality line as long
// as the sequence. kseq++'s parser differs: it appends quality lines until the quality is long
// enough, so a short quality line takes the next header as quality, and it skips any text
// before a header. For FASTQ each of those is MALFORMED here, since each loses or misreads a
// record; FASTA keeps the skip before its first header.
template <class Stream>
SequenceReader::ParseResult SequenceReader::parse_record(Stream &stream, klibpp::KSeq &rec, StreamState &state,
                                                         std::string &problem) {
	char c = state.pending_header;
	state.pending_header = 0;
	if (!c) {
		if (state.at_start) {
			// Skip to the first header: old FASTA files can open with ';' comments. FASTQ has no
			// such preamble, so text before a first '@' is a record that has lost its '@'.
			bool skipped_text = false;
			while ((c = stream.getc()) && c != FASTA_HEADER && c != FASTQ_HEADER) {
				skipped_text |= !is_ascii_space(c);
			}
			if (c == FASTQ_HEADER && skipped_text) {
				problem = "text before the first record; FASTQ files must start with a '@' header line";
				return ParseResult::MALFORMED;
			}
		} else {
			// The previous record did not end at the next header: it was FASTQ, or it ended at a
			// NUL or the end of the input. The next line must be a header, though blank lines,
			// and lines of nothing but whitespace, may come first.
			while (true) {
				while ((c = stream.getc()) && (c == '\n' || c == '\r')) {
				}
				if (!c || c == FASTA_HEADER || c == FASTQ_HEADER) {
					break;
				}
				std::string line(1, c);
				stream.getuntil(SEP_LINE, line, nullptr, true);
				if (!std::all_of(line.begin(), line.end(), is_ascii_space)) {
					problem = "the line after record " + std::to_string(state.records_read) +
					          " is not a record header: '" + line.substr(0, 40) + "'";
					return ParseResult::MALFORMED;
				}
			}
		}
		state.at_start = false;
		if (!c) {
			return ParseResult::END;
		}
	}
	const bool fastq = c == FASTQ_HEADER;
	auto record = [&]() {
		return "record '" + rec.name + "' (record " + std::to_string(state.records_read + 1) + "): ";
	};
	rec.clear();
	if (!stream.getuntil(SEP_SPACE, rec.name, &c)) {
		if (fastq) {
			problem = "record " + std::to_string(state.records_read + 1) + ": the input ends inside its header";
			return ParseResult::MALFORMED;
		}
		return ParseResult::END;
	}
	if (c != '\n') {
		stream.getuntil(SEP_LINE, rec.comment, nullptr);
	}
	size_t sequence_lines = 0;
	while ((c = stream.getc()) && c != FASTA_HEADER && c != FASTQ_HEADER && c != FASTQ_SEPARATOR) {
		if (c == '\n' || c == '\r') {
			continue; // skip empty lines, LF or CRLF
		}
		rec.seq += c;
		stream.getuntil(SEP_LINE, rec.seq, nullptr, true); // the rest of the line
		sequence_lines++;
	}
	if (c == FASTA_HEADER || c == FASTQ_HEADER) {
		state.pending_header = c; // the next record's header character is already read
	}
	if (c != FASTQ_SEPARATOR) {
		if (fastq) {
			if (!c && !stream.eof()) {
				problem = record() + "unexpected NUL byte before the record's '+' line";
			} else {
				problem = record() +
				          (c ? "no '+' line before the next record" : "the input ends before the record's '+' line") +
				          "; FASTQ records must be four lines";
			}
			return ParseResult::MALFORMED;
		}
		return ParseResult::RECORD; // FASTA
	}
	while ((c = stream.getc()) && c != '\n') {
	} // the rest of the '+' line
	if (sequence_lines > 1) {
		problem = record() + "the sequence spans " + std::to_string(sequence_lines) +
		          " lines; FASTQ records must be four lines (wrapped FASTQ is not supported)";
		return ParseResult::MALFORMED;
	}
	stream.getuntil(SEP_LINE, rec.qual, nullptr);
	if (rec.qual.size() != rec.seq.size()) {
		problem = record() + "sequence length " + std::to_string(rec.seq.size()) + " does not match quality length " +
		          std::to_string(rec.qual.size());
		return ParseResult::MALFORMED;
	}
	return ParseResult::RECORD;
}

// How a failed read is reported, whichever stream it came from: the source, how many records
// were read before it, and why it failed.
static std::string read_failure(const std::string &source, uint64_t records_read, const std::string &why) {
	return source + ": read failed after record " + std::to_string(records_read) + ": " + why;
}

// Read from whichever stream variant is active, one record at a time, asking the input why it
// stopped whenever a record fails or ends with the input.
//
// Errors that originate in our kseq++ read callbacks (duckdb_seq_read /
// aspera_seq_read) are reported via the thread-local g_seq_read_error
// channel: the callback writes a message and returns -1, which getc() reads
// as the end of the input, and we raise IOException from here. This keeps the
// throw out of kseq++'s template-instantiated frames: a throw from inside them
// can end in a recursive terminate on the aspera path.
std::vector<klibpp::KSeq> SequenceReader::read_stream(StreamVar &var, int n, const std::string &source,
                                                      StreamState &state) {
#ifdef MIINT_STATIC_BUILD
	g_seq_read_error.clear();
#endif
	std::vector<klibpp::KSeq> result;
	result.reserve(static_cast<size_t>(n));
	std::visit(
	    [&](auto &reader) {
		    for (int i = 0; i < n; i++) {
			    std::string problem;
			    const auto parsed = parse_record(*reader, result.emplace_back(), state, problem);
			    if (parsed == ParseResult::RECORD && !reader->eof()) {
				    state.records_read++;
				    continue;
			    }
			// The record is malformed, or the input has stopped. A record that ends with the
			// input is kept only once the input says it ended cleanly: a FASTA record has no
			// terminator, so one that a truncation or a read error cut short looks complete.
#ifdef MIINT_STATIC_BUILD
			    if (!g_seq_read_error.empty()) {
				    result.pop_back();
				    return; // the callback's own message is raised below
			    }
#endif
			    // The input itself failing outranks the record: a file cut off mid-record also
			    // leaves that record malformed, and the truncation is the cause to report.
			    std::string input_error;
			    if constexpr (std::is_same_v<std::decay_t<decltype(*reader)>, CheckedSeqStreamIn>) {
				    input_error = reader->EndOfInputError();
			    }
			    if (input_error.empty() && reader->err()) {
				    input_error = "the input reported a read error";
			    }
			    if (!input_error.empty()) {
				    throw std::runtime_error(read_failure(source, state.records_read, input_error));
			    }
			    if (parsed == ParseResult::RECORD) {
				    state.records_read++;
				    continue; // the input ended cleanly just after it
			    }
			    result.pop_back();
			    if (parsed == ParseResult::MALFORMED) {
				    throw std::runtime_error(source + ": " + problem);
			    }
			    // getc() returns 0 for a NUL byte as well as at the end of the input, and only eof()
			    // tells them apart. A NUL where a record should start is damage, such as the
			    // zero-filled block a crash can leave, not the end of the file.
			    if (!reader->eof()) {
				    throw std::runtime_error(source + ": unexpected NUL byte after record " +
				                             std::to_string(state.records_read));
			    }
			    return; // end of input
		    }
	    },
	    var);
#ifdef MIINT_STATIC_BUILD
	if (!g_seq_read_error.empty()) {
		std::string msg = read_failure(source, state.records_read, g_seq_read_error);
		g_seq_read_error.clear();
		throw duckdb::IOException(msg);
	}
#endif
	return result;
}

SequenceReader::SequenceReader(const std::string &path1, const std::optional<std::string> &path2,
                               bool allow_format_mismatch)
    : first_read_(true) {
	source1_ = path1;
	sequence1_reader_ = std::make_unique<SeqStreamIn>(path1);

	// Check if first file is empty by attempting to peek at first record
	buffered_read1_ = read_stream(sequence1_reader_, 1, source1_, state1_);
	bool is_empty1 = buffered_read1_.empty();
	bool is_fasta1 = !is_empty1 && buffered_read1_[0].qual.empty();

	if (is_empty1) {
		throw std::runtime_error("Empty file: " + path1);
	}

	// Keep buffered_read1_ to return on first read() call instead of recreating reader

	paired_ = path2.has_value() && (path2->length() > 0);
	if (paired_) {
		source2_ = path2.value();
		sequence2_reader_.emplace(std::make_unique<SeqStreamIn>(path2.value()));

		// Check if second file is empty and detect format
		buffered_read2_ = read_stream(sequence2_reader_.value(), 1, source2_, state2_);
		bool is_empty2 = buffered_read2_.empty();
		bool is_fasta2 = !is_empty2 && buffered_read2_[0].qual.empty();

		if (is_empty2) {
			throw std::runtime_error("Empty file: " + path2.value());
		}

		validate_format_consistency(is_fasta1, is_fasta2, allow_format_mismatch);

		// Keep buffered_read2_ to return on first read() call instead of recreating reader
	}
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
	if (stream1 && stream1->handle) {
		source1_ = stream1->handle->GetPath();
	}
	auto kstream1 = std::make_unique<DuckDBSeqStreamIn>(stream1.get(), duckdb_seq_read, duckdb_seq_close);
	stream1.release();
	sequence1_reader_ = std::move(kstream1);

	buffered_read1_ = read_stream(sequence1_reader_, 1, source1_, state1_);
	bool is_empty1 = buffered_read1_.empty();
	bool is_fasta1 = !is_empty1 && buffered_read1_[0].qual.empty();

	if (is_empty1) {
		throw std::runtime_error("Empty stream");
	}

	paired_ = (stream2_or_null.get() != nullptr);
	if (paired_) {
		if (stream2_or_null->handle) {
			source2_ = stream2_or_null->handle->GetPath();
		}
		auto kstream2 = std::make_unique<DuckDBSeqStreamIn>(stream2_or_null.get(), duckdb_seq_read, duckdb_seq_close);
		stream2_or_null.release();
		sequence2_reader_.emplace(std::move(kstream2));

		buffered_read2_ = read_stream(sequence2_reader_.value(), 1, source2_, state2_);
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
	if (stream1 && !stream1->source.empty()) {
		source1_ = stream1->source;
	}
	auto kstream1 = std::make_unique<AsperaSeqStreamIn>(stream1.get(), aspera_seq_read, aspera_seq_close);
	stream1.release();
	sequence1_reader_ = std::move(kstream1);

	paired_ = (stream2_or_null.get() != nullptr);
	if (paired_) {
		if (!stream2_or_null->source.empty()) {
			source2_ = stream2_or_null->source;
		}
		auto kstream2 = std::make_unique<AsperaSeqStreamIn>(stream2_or_null.get(), aspera_seq_read, aspera_seq_close);
		stream2_or_null.release();
		sequence2_reader_.emplace(std::move(kstream2));
	}

	buffered_read1_ = read_stream(sequence1_reader_, 1, source1_, state1_);
	if (buffered_read1_.empty()) {
		throw std::runtime_error("Empty stream");
	}
	bool is_fasta1 = buffered_read1_[0].qual.empty();

	if (paired_) {
		buffered_read2_ = read_stream(sequence2_reader_.value(), 1, source2_, state2_);
		if (buffered_read2_.empty()) {
			throw std::runtime_error("Empty stream (sequence2)");
		}
		bool is_fasta2 = buffered_read2_[0].qual.empty();

		validate_format_consistency(is_fasta1, is_fasta2, allow_format_mismatch);
	}
}
#endif // MIINT_ASPERA_SUPPORTED
#endif // MIINT_STATIC_BUILD

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

// Strip whitespace from a sequence in place. Nearly every sequence has none, so look first
// with a branch-free pass the compiler can vectorize and rewrite only when one turns up.
// The accumulator is unsigned char, not bool: GCC 14 leaves a bool |= reduction scalar.
static void strip_whitespace(std::string &seq) {
	unsigned char any = 0;
	for (unsigned char c : seq) {
		any |= is_ascii_space(c);
	}
	if (any) {
		seq.erase(std::remove_if(seq.begin(), seq.end(), is_ascii_space), seq.end());
	}
}

// Append a KSeq into an SE batch, stripping sequence whitespace in place.
static void append_se(SequenceRecordBatch &batch, klibpp::KSeq &rec) {
	strip_whitespace(rec.seq);
	batch.read_ids.emplace_back(base_read_id(rec.name));
	batch.comments.emplace_back(std::move(rec.comment));
	batch.sequences1.emplace_back(std::move(rec.seq));
	batch.quals1.emplace_back(std::move(rec.qual));
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
		auto more = read_stream(sequence1_reader_, poll_n, source1_, state1_);
		max_poll_count_ = more.size() > max_poll_count_ ? more.size() : max_poll_count_;
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
	strip_whitespace(rec1.seq);
	strip_whitespace(rec2.seq);

	batch.read_ids.emplace_back(base_read_id(rec1.name));
	batch.comments.emplace_back(std::move(rec1.comment));
	batch.sequences1.emplace_back(std::move(rec1.seq));
	batch.sequences2.emplace_back(std::move(rec2.seq));
	batch.quals1.emplace_back(std::move(rec1.qual));
	batch.quals2.emplace_back(std::move(rec2.qual));
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
		auto more1 = read_stream(sequence1_reader_, poll_n, source1_, state1_);
		auto more2 = read_stream(sequence2_reader_.value(), poll_n, source2_, state2_);
		max_poll_count_ = more1.size() > max_poll_count_ ? more1.size() : max_poll_count_;

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
