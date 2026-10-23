#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace miint {

// The TYPE letter of a TAG:TYPE:VALUE field, from the SAM specification's optional-field
// section (section 1.5), which GFA 1 adopts for its tags.
enum class TagType : char {
	PrintableChar = 'A',
	Integer = 'i',
	Float = 'f',
	String = 'Z',
	Json = 'J',
	HexBytes = 'H',
	NumericArray = 'B',
};

// One optional field of a SAM-style record. SAM, PAF and GFA all write these as
// TAG:TYPE:VALUE, and `value` is the text after the second ':'. An untyped key:value field
// has no `type`.
struct SamStyleTag {
	std::string key;
	std::string value;
	std::optional<TagType> type;
};

// True when `value` is valid for `type`: the specification's grammar for it, with an f value
// also allowed to be nan or an infinity (IsFloat in sam_style_tags.cpp).
bool IsValidTagValue(TagType type, std::string_view value);

enum class TagSyntax {
	// Every field must be TAG:TYPE:VALUE as the SAM and GFA specifications define it: a
	// type in A/i/f/Z/J/H/B and a value valid for that type. The type and value are what
	// matter, so the name follows one rule for every producer: any identifier
	// ([A-Za-z_][A-Za-z0-9_]*), not only the specifications' two characters. Real
	// assemblers write longer ones (myloasm: MEDIAN_DP1). An f value may also be nan or an
	// infinity, as htslib reads one (IsFloat in sam_style_tags.cpp).
	Typed,
	// Additionally accept an untyped key:value field, which some PAF writers emit
	// (myloasm writes `shared_mini:284`). A field that has the typed shape is still
	// validated as typed, so `NM:i:abc` is an error in either mode.
	TypedOrUntyped,
};

// True when every character of `s` is printable ASCII or a space ([ !-~]*), the SAM
// specification's class for a Z value.
bool AllPrintableOrSpace(std::string_view s);

// Parse fields[first..] into `out` (cleared first), in field order. Throws
// std::runtime_error naming the offending field when a field is malformed or a key
// appears twice; the caller adds the line number.
void ParseSamStyleTags(const std::vector<std::string_view> &fields, size_t first, TagSyntax syntax,
                       std::vector<SamStyleTag> &out);

} // namespace miint
