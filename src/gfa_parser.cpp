#include "gfa_parser.hpp"

#include "line_splitter.hpp"

#include <optional>
#include <stdexcept>

namespace miint {

namespace {

constexpr char COMMENT = '#';

// The record types the GFA 1.x specification defines (1.2 adds J), by their letter.
enum class RecordType : char {
	Header = 'H',
	Segment = 'S',
	Link = 'L',
	Containment = 'C',
	Path = 'P',
	Walk = 'W',
	Jump = 'J',
};

struct RecordLayout {
	// Mandatory positional fields after the record type.
	int mandatory_fields;
	// The record joins two segments, each with an orientation: fields 2 and 4 after the type.
	bool joins_oriented_segments;
};

// The layout of a record type the specification defines; nullopt for any other.
std::optional<RecordLayout> LayoutOf(std::string_view type) {
	if (type.size() != 1) {
		return std::nullopt;
	}
	switch (static_cast<RecordType>(type[0])) {
	case RecordType::Header:
		return RecordLayout {0, false};
	case RecordType::Segment:
		return RecordLayout {2, false};
	case RecordType::Link:
		return RecordLayout {5, true};
	case RecordType::Containment:
		return RecordLayout {6, true};
	case RecordType::Path:
		return RecordLayout {3, false};
	case RecordType::Walk:
		return RecordLayout {6, false};
	case RecordType::Jump:
		return RecordLayout {5, true};
	default:
		return std::nullopt;
	}
}

bool IsOrientation(std::string_view s) {
	return s == "+" || s == "-";
}

// "an S record", "a C record": the article follows how the letter is spoken.
std::string RecordName(RecordType type) {
	const bool vowel_sound = type == RecordType::Header || type == RecordType::Link || type == RecordType::Segment;
	return std::string(vowel_sound ? "an " : "a ") + static_cast<char>(type) + " record";
}

} // namespace

bool GfaParser::Parse(std::string_view line, GfaRecord &record) {
	if (line.empty()) {
		throw std::runtime_error("empty line");
	}
	if (line[0] == COMMENT) {
		return false;
	}
	SplitOnTabs(line, fields_);
	if (fields_[0].empty()) {
		throw std::runtime_error("record type is empty");
	}
	// An unknown record type is kept as written, so this is what stops compressed bytes read as
	// text (a gzipped graph whose name does not end in .gz) from becoming rows.
	if (!AllPrintableOrSpace(fields_[0])) {
		throw std::runtime_error(
		    "record type is not text; is the file compressed under a name that does not end in .gz?");
	}
	record.record_type.assign(fields_[0]);
	record.fields.clear();
	record.tags.clear();

	const auto layout = LayoutOf(fields_[0]);
	if (!layout) {
		for (size_t i = 1; i < fields_.size(); i++) {
			record.fields.emplace_back(fields_[i]);
		}
		record.has_tags = false;
		return true;
	}
	const auto type = static_cast<RecordType>(fields_[0][0]);
	const int mandatory = layout->mandatory_fields;

	const size_t available = fields_.size() - 1;
	if (available < static_cast<size_t>(mandatory)) {
		throw std::runtime_error(RecordName(type) + " needs " + std::to_string(mandatory) +
		                         " fields after the record type, found " + std::to_string(available));
	}
	if (layout->joins_oriented_segments && !(IsOrientation(fields_[2]) && IsOrientation(fields_[4]))) {
		throw std::runtime_error(RecordName(type) + "'s orientations must be + or -, found '" +
		                         std::string(fields_[2]) + "' and '" + std::string(fields_[4]) + "'");
	}
	for (int i = 1; i <= mandatory; i++) {
		record.fields.emplace_back(fields_[i]);
	}
	ParseSamStyleTags(fields_, 1 + mandatory, TagSyntax::Typed, record.tags);
	record.has_tags = true;

	// Only GFA 1's record layout is known, so a header naming any other version, anything but
	// "1" or a value starting "1.", is refused. GFA 2 ("2" or "2.") gets its own message.
	if (type == RecordType::Header) {
		for (const auto &tag : record.tags) {
			if (tag.key == "VN" && tag.value != "1" && tag.value.rfind("1.", 0) != 0) {
				const bool gfa2 = tag.value == "2" || tag.value.rfind("2.", 0) == 0;
				throw std::runtime_error(std::string(gfa2 ? "GFA 2 is not supported" : "only GFA 1 is supported") +
				                         " (the header declares VN:Z:" + tag.value + ")");
			}
		}
	}
	return true;
}

} // namespace miint
