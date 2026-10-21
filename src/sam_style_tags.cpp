#include "sam_style_tags.hpp"

#include "duckdb/common/string_util.hpp"

#include <stdexcept>

namespace miint {

using duckdb::StringUtil;

namespace {

// Character classes and value grammars from the SAM specification's optional-field
// section (section 1.5), which GFA 1 adopts for its tags.

// The first character of a B value: the type of every element after it.
enum class ArrayElementType : char {
	Int8 = 'c',
	UInt8 = 'C',
	Int16 = 's',
	UInt16 = 'S',
	Int32 = 'i',
	UInt32 = 'I',
	Float = 'f',
};

bool IsTagType(char c) {
	switch (static_cast<TagType>(c)) {
	case TagType::PrintableChar:
	case TagType::Integer:
	case TagType::Float:
	case TagType::String:
	case TagType::Json:
	case TagType::HexBytes:
	case TagType::NumericArray:
		return true;
	default:
		return false;
	}
}

// [!-~]. <cctype>'s isgraph matches it only in the C locale.
bool IsPrintable(char c) {
	return c >= '!' && c <= '~';
}

// [ !-~]. <cctype>'s isprint matches it only in the C locale.
bool IsPrintableOrSpace(char c) {
	return c >= ' ' && c <= '~';
}

} // namespace

bool AllPrintableOrSpace(std::string_view s) {
	for (char c : s) {
		if (!IsPrintableOrSpace(c)) {
			return false;
		}
	}
	return true;
}

namespace {

// [-+]?[0-9]+
bool IsInteger(std::string_view s) {
	size_t i = 0;
	if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
		++i;
	}
	if (i == s.size()) {
		return false;
	}
	for (; i < s.size(); ++i) {
		if (!StringUtil::CharacterIsDigit(s[i])) {
			return false;
		}
	}
	return true;
}

// `s` equals the lowercase `word`, ignoring the case of ASCII letters.
bool EqualsLowercase(std::string_view s, std::string_view word) {
	if (s.size() != word.size()) {
		return false;
	}
	for (size_t i = 0; i < s.size(); ++i) {
		if (StringUtil::CharacterToLower(s[i]) != word[i]) {
			return false;
		}
	}
	return true;
}

// [-+]?[0-9]*\.?[0-9]+([eE][-+]?[0-9]+)?, or an optional sign then nan, inf or infinity in
// any case.
// The specification's grammar stops at the first form, but htslib parses floats with strtod,
// which accepts the named ones, and tools write them. strtod's nan(chars) and hex floats are
// not accepted.
bool IsFloat(std::string_view s) {
	size_t i = 0;
	const size_t n = s.size();
	if (i < n && (s[i] == '-' || s[i] == '+')) {
		++i;
	}
	const std::string_view named = s.substr(i);
	if (EqualsLowercase(named, "nan") || EqualsLowercase(named, "inf") || EqualsLowercase(named, "infinity")) {
		return true;
	}
	size_t int_digits = 0;
	while (i < n && StringUtil::CharacterIsDigit(s[i])) {
		++i;
		++int_digits;
	}
	if (i < n && s[i] == '.') {
		++i;
		size_t frac_digits = 0;
		while (i < n && StringUtil::CharacterIsDigit(s[i])) {
			++i;
			++frac_digits;
		}
		if (frac_digits == 0) {
			return false;
		}
	} else if (int_digits == 0) {
		return false;
	}
	if (i < n && (s[i] == 'e' || s[i] == 'E')) {
		++i;
		if (i < n && (s[i] == '-' || s[i] == '+')) {
			++i;
		}
		size_t exp_digits = 0;
		while (i < n && StringUtil::CharacterIsDigit(s[i])) {
			++i;
			++exp_digits;
		}
		if (exp_digits == 0) {
			return false;
		}
	}
	return i == n;
}

// ([0-9A-F][0-9A-F])*: uppercase only, so not StringUtil::CharacterIsHex.
bool IsHexBytes(std::string_view s) {
	if (s.size() % 2 != 0) {
		return false;
	}
	for (char c : s) {
		if (!StringUtil::CharacterIsDigit(c) && !(c >= 'A' && c <= 'F')) {
			return false;
		}
	}
	return true;
}

// [cCsSiIf](,<element>)*: integer elements for the integer element types, IsFloat ones for f.
bool IsNumericArray(std::string_view s) {
	if (s.empty()) {
		return false;
	}
	bool is_float;
	switch (static_cast<ArrayElementType>(s[0])) {
	case ArrayElementType::Int8:
	case ArrayElementType::UInt8:
	case ArrayElementType::Int16:
	case ArrayElementType::UInt16:
	case ArrayElementType::Int32:
	case ArrayElementType::UInt32:
		is_float = false;
		break;
	case ArrayElementType::Float:
		is_float = true;
		break;
	default:
		return false;
	}
	size_t pos = 1;
	while (pos < s.size()) {
		if (s[pos] != ',') {
			return false;
		}
		size_t next = s.find(',', pos + 1);
		auto element = s.substr(pos + 1, next == std::string_view::npos ? std::string_view::npos : next - pos - 1);
		if (is_float ? !IsFloat(element) : !IsInteger(element)) {
			return false;
		}
		pos = next == std::string_view::npos ? s.size() : next;
	}
	return true;
}

} // namespace

bool IsValidTagValue(TagType type, std::string_view value) {
	switch (type) {
	case TagType::PrintableChar:
		return value.size() == 1 && IsPrintable(value[0]);
	case TagType::Integer:
		return IsInteger(value);
	case TagType::Float:
		return IsFloat(value);
	case TagType::String:
		return AllPrintableOrSpace(value);
	case TagType::Json:
		return !value.empty() && AllPrintableOrSpace(value);
	case TagType::HexBytes:
		return IsHexBytes(value);
	case TagType::NumericArray:
		return IsNumericArray(value);
	default:
		return false;
	}
}

namespace {

// [A-Za-z_][A-Za-z0-9_]*, for typed and untyped fields alike (why: TagSyntax::Typed).
bool IsTagName(std::string_view key) {
	if (key.empty() || !(StringUtil::CharacterIsAlpha(key[0]) || key[0] == '_')) {
		return false;
	}
	for (char c : key) {
		if (!(StringUtil::CharacterIsAlphaNumeric(c) || c == '_')) {
			return false;
		}
	}
	return true;
}

std::string Quoted(std::string_view field) {
	return "'" + std::string(field) + "'";
}

} // namespace

void ParseSamStyleTags(const std::vector<std::string_view> &fields, size_t first, TagSyntax syntax,
                       std::vector<SamStyleTag> &out) {
	out.clear();
	for (size_t i = first; i < fields.size(); ++i) {
		std::string_view field = fields[i];
		const size_t colon = field.find(':');
		const bool named = colon != std::string_view::npos && IsTagName(field.substr(0, colon));
		// NAME:TYPE:VALUE: a known type letter between the first two colons.
		const bool typed = named && colon + 2 < field.size() && IsTagType(field[colon + 1]) && field[colon + 2] == ':';
		std::string_view value;
		std::optional<TagType> type;
		if (typed) {
			value = field.substr(colon + 3);
			type = static_cast<TagType>(field[colon + 1]);
			if (!IsValidTagValue(*type, value)) {
				throw std::runtime_error(Quoted(field) + " has a value that is not valid for type '" +
				                         std::string(1, field[colon + 1]) + "'");
			}
		} else {
			if (syntax != TagSyntax::TypedOrUntyped || !named || !AllPrintableOrSpace(field.substr(colon + 1))) {
				throw std::runtime_error(Quoted(field) + " is not a tag (expected TAG:TYPE:VALUE" +
				                         std::string(syntax == TagSyntax::TypedOrUntyped ? " or key:value" : "") + ")");
			}
			value = field.substr(colon + 1);
		}
		// Every unnamed field has thrown above.
		const std::string_view key = field.substr(0, colon);
		for (const auto &existing : out) {
			if (existing.key == key) {
				throw std::runtime_error("tag " + Quoted(key) + " appears more than once");
			}
		}
		out.push_back({std::string(key), std::string(value), type});
	}
}

} // namespace miint
