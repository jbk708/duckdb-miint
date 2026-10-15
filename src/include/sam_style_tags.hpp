#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace miint {

// One optional field of a SAM-style record. SAM, PAF and GFA all write these as
// TAG:TYPE:VALUE; the type letter is validated and then dropped, so `value` is the text
// after the second ':'.
struct SamStyleTag {
	std::string key;
	std::string value;
};

enum class TagSyntax {
	// Every field must be TAG:TYPE:VALUE as the SAM and GFA specifications define it:
	// a two-character tag, a type in A/i/f/Z/J/H/B, and a value valid for that type.
	Typed,
	// Additionally accept an untyped key:value field, which some PAF writers emit
	// (myloasm writes `shared_mini:284`). A field that has the typed shape is still
	// validated as typed, so `NM:i:abc` is an error in either mode.
	TypedOrUntyped,
};

// Parse fields[first..] into `out` (cleared first), in field order. Throws
// std::runtime_error naming the offending field when a field is malformed or a key
// appears twice; the caller adds the line number.
void ParseSamStyleTags(const std::vector<std::string_view> &fields, size_t first, TagSyntax syntax,
                       std::vector<SamStyleTag> &out);

} // namespace miint
