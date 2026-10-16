#include "gfa_parser.hpp"

#include "line_splitter.hpp"

#include <stdexcept>

namespace miint {

namespace {

// Mandatory positional fields after the record type, per the GFA 1.x specification; -1
// for a record type it does not define.
int MandatoryFieldCount(std::string_view type) {
	if (type.size() != 1) {
		return -1;
	}
	switch (type[0]) {
	case 'H':
		return 0;
	case 'S':
		return 2;
	case 'L':
		return 5;
	case 'C':
		return 6;
	case 'P':
		return 3;
	case 'W':
		return 6;
	case 'J':
		return 5;
	default:
		return -1;
	}
}

// "an S record", "a C record": the article follows how the letter is spoken.
std::string RecordName(std::string_view type) {
	bool vowel_sound = type == "H" || type == "L" || type == "S";
	return std::string(vowel_sound ? "an " : "a ") + std::string(type) + " record";
}

// Printable ASCII. An unknown record type is kept as written, so this is what stops compressed
// bytes read as text (a gzipped graph whose name does not end in .gz) from becoming rows.
bool IsText(std::string_view s) {
	for (unsigned char c : s) {
		if (c < 0x20 || c > 0x7e) {
			return false;
		}
	}
	return true;
}

} // namespace

bool GfaParser::Parse(std::string_view line, GfaRecord &record) {
	if (line.empty()) {
		throw std::runtime_error("empty line");
	}
	if (line[0] == '#') {
		return false;
	}
	SplitOnTabs(line, fields_);
	if (fields_[0].empty()) {
		throw std::runtime_error("record type is empty");
	}
	if (!IsText(fields_[0])) {
		throw std::runtime_error(
		    "record type is not text; is the file compressed under a name that does not end in .gz?");
	}
	record.record_type.assign(fields_[0]);
	record.fields.clear();
	record.tags.clear();

	const int mandatory = MandatoryFieldCount(fields_[0]);
	if (mandatory < 0) {
		for (size_t i = 1; i < fields_.size(); i++) {
			record.fields.emplace_back(fields_[i]);
		}
		record.has_tags = false;
		return true;
	}

	const size_t available = fields_.size() - 1;
	if (available < static_cast<size_t>(mandatory)) {
		throw std::runtime_error(RecordName(fields_[0]) + " needs " + std::to_string(mandatory) +
		                         " fields after the record type, found " + std::to_string(available));
	}
	for (int i = 1; i <= mandatory; i++) {
		record.fields.emplace_back(fields_[i]);
	}
	ParseSamStyleTags(fields_, 1 + mandatory, TagSyntax::Typed, record.tags);
	record.has_tags = true;

	// Only GFA 1's record layout is known, so a header naming any other version is refused.
	if (record.record_type == "H") {
		for (const auto &tag : record.tags) {
			if (tag.key == "VN" && (tag.value.empty() || tag.value[0] != '1')) {
				const bool gfa2 = !tag.value.empty() && tag.value[0] == '2';
				throw std::runtime_error(std::string(gfa2 ? "GFA 2 is not supported" : "only GFA 1 is supported") +
				                         " (the header declares VN:Z:" + tag.value + ")");
			}
		}
	}
	return true;
}

} // namespace miint
