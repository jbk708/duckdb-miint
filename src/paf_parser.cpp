#include "paf_parser.hpp"

#include "line_splitter.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace miint {

namespace {

constexpr size_t kMandatoryFields = 12;

// A whole-field non-negative integer. from_chars reports a partial parse ("12x") and an
// overflow, either of which would otherwise land on a plausible coordinate.
int64_t ParseCount(std::string_view field, const char *what) {
	int64_t value = 0;
	const char *end = field.data() + field.size();
	auto [ptr, ec] = std::from_chars(field.data(), end, value);
	if (ec != std::errc {} || ptr != end || value < 0) {
		throw std::runtime_error(std::string(what) + " is not a non-negative integer: '" + std::string(field) + "'");
	}
	return value;
}

void CheckInterval(const char *sequence, int64_t start, int64_t stop, int64_t length) {
	if (start > stop || stop > length) {
		throw std::runtime_error(std::string(sequence) + " interval [" + std::to_string(start) + ", " +
		                         std::to_string(stop) + ") is not within the " + sequence + " length " +
		                         std::to_string(length));
	}
	if (stop == std::numeric_limits<int64_t>::max()) {
		throw std::runtime_error(std::string(sequence) + " end " + std::to_string(stop) +
		                         " is too large for a 1-based position");
	}
}

} // namespace

void PafParser::Parse(std::string_view line, PafRecord &record) {
	if (line.empty()) {
		throw std::runtime_error("empty line");
	}
	SplitOnTabs(line, fields_);
	if (fields_.size() < kMandatoryFields) {
		throw std::runtime_error("expected at least 12 tab-separated fields, found " + std::to_string(fields_.size()));
	}
	if (fields_[0].empty()) {
		throw std::runtime_error("query name is empty");
	}
	if (fields_[5].empty()) {
		throw std::runtime_error("target name is empty");
	}

	record.read_id.assign(fields_[0]);
	record.query_length = ParseCount(fields_[1], "query length");
	const int64_t query_start = ParseCount(fields_[2], "query start");
	const int64_t query_end = ParseCount(fields_[3], "query end");
	record.strand.assign(fields_[4]);
	record.reference.assign(fields_[5]);
	record.reference_length = ParseCount(fields_[6], "target length");
	int64_t target_start = ParseCount(fields_[7], "target start");
	int64_t target_end = ParseCount(fields_[8], "target end");
	record.residue_matches = ParseCount(fields_[9], "residue matches");
	record.block_length = ParseCount(fields_[10], "block length");
	int64_t mapq = ParseCount(fields_[11], "mapping quality");
	if (mapq > 255) {
		throw std::runtime_error("mapping quality must be at most 255, found " + std::to_string(mapq));
	}
	record.mapq = static_cast<uint8_t>(mapq);

	if (record.strand != "+" && record.strand != "-" && record.strand != "*") {
		throw std::runtime_error("strand must be '+', '-', or '*' for an unmapped record, found '" + record.strand +
		                         "'");
	}
	// minimap2 --paf-no-hit marks an unmapped query with '*' for both. One without the
	// other is not a record any aligner writes.
	const bool unmapped = record.strand == "*";
	if (unmapped != (record.reference == "*")) {
		throw std::runtime_error("an unmapped record has '*' as both strand and target name, found strand '" +
		                         record.strand + "' and target '" + record.reference + "'");
	}

	if (unmapped) {
		if (query_start != 0 || query_end != 0) {
			throw std::runtime_error("an unmapped record must have query start and end 0, found [" +
			                         std::to_string(query_start) + ", " + std::to_string(query_end) + ")");
		}
		if (target_start != 0 || target_end != 0) {
			throw std::runtime_error("an unmapped record must have target start and end 0, found [" +
			                         std::to_string(target_start) + ", " + std::to_string(target_end) + ")");
		}
		// read_alignments reports an unplaced unmapped record (RNAME *, POS 0) at position 0;
		// match it. The query has no aligned block either, so it gets the same 0.
		record.query_start = 0;
		record.query_stop = 0;
		record.position = 0;
		record.stop_position = 0;
	} else {
		CheckInterval("query", query_start, query_end, record.query_length);
		CheckInterval("target", target_start, target_end, record.reference_length);
		// PAF intervals are 0-based half-open; miint's are 1-based half-open.
		record.query_start = query_start + 1;
		record.query_stop = query_end + 1;
		record.position = target_start + 1;
		record.stop_position = target_end + 1;
	}

	ParseSamStyleTags(fields_, kMandatoryFields, TagSyntax::TypedOrUntyped, record.tags);
}

} // namespace miint
