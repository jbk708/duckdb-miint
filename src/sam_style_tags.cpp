#include "sam_style_tags.hpp"

#include <stdexcept>

namespace miint {

namespace {

// Character classes and value grammars from the SAM specification's optional-field
// section (section 1.5), which GFA 1 adopts for its tags.

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

bool IsAlpha(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// [!-~]
bool IsPrintable(char c) {
	return c >= '!' && c <= '~';
}

// [ !-~]
bool IsPrintableOrSpace(char c) {
	return c >= ' ' && c <= '~';
}

bool AllPrintableOrSpace(std::string_view s) {
	for (char c : s) {
		if (!IsPrintableOrSpace(c)) {
			return false;
		}
	}
	return true;
}

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
		if (!IsDigit(s[i])) {
			return false;
		}
	}
	return true;
}

// [-+]?[0-9]*\.?[0-9]+([eE][-+]?[0-9]+)?
bool IsFloat(std::string_view s) {
	size_t i = 0;
	const size_t n = s.size();
	if (i < n && (s[i] == '-' || s[i] == '+')) {
		++i;
	}
	size_t int_digits = 0;
	while (i < n && IsDigit(s[i])) {
		++i;
		++int_digits;
	}
	if (i < n && s[i] == '.') {
		++i;
		size_t frac_digits = 0;
		while (i < n && IsDigit(s[i])) {
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
		while (i < n && IsDigit(s[i])) {
			++i;
			++exp_digits;
		}
		if (exp_digits == 0) {
			return false;
		}
	}
	return i == n;
}

// ([0-9A-F][0-9A-F])*
bool IsHexBytes(std::string_view s) {
	if (s.size() % 2 != 0) {
		return false;
	}
	for (char c : s) {
		if (!IsDigit(c) && !(c >= 'A' && c <= 'F')) {
			return false;
		}
	}
	return true;
}

// [cCsSiIf](,[-+]?[0-9]*\.?[0-9]+([eE][-+]?[0-9]+)?)*, with integer elements for the
// integer subtypes.
bool IsNumericArray(std::string_view s) {
	if (s.empty()) {
		return false;
	}
	const char subtype = s[0];
	bool is_float;
	switch (subtype) {
	case 'c':
	case 'C':
	case 's':
	case 'S':
	case 'i':
	case 'I':
		is_float = false;
		break;
	case 'f':
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

bool IsValidValue(char type, std::string_view value) {
	switch (type) {
	case 'A':
		return value.size() == 1 && IsPrintable(value[0]);
	case 'i':
		return IsInteger(value);
	case 'f':
		return IsFloat(value);
	case 'Z':
		return AllPrintableOrSpace(value);
	case 'J':
		return !value.empty() && AllPrintableOrSpace(value);
	case 'H':
		return IsHexBytes(value);
	case 'B':
		return IsNumericArray(value);
	default:
		return false;
	}
}

// TAG:TYPE:VALUE with a [A-Za-z][A-Za-z0-9] tag and a known type letter.
bool HasTypedShape(std::string_view field) {
	return field.size() >= 5 && IsAlpha(field[0]) && (IsAlpha(field[1]) || IsDigit(field[1])) && field[2] == ':' &&
	       std::string_view("AifZJHB").find(field[3]) != std::string_view::npos && field[4] == ':';
}

// [A-Za-z_][A-Za-z0-9_]*
bool IsUntypedKey(std::string_view key) {
	if (key.empty() || !(IsAlpha(key[0]) || key[0] == '_')) {
		return false;
	}
	for (char c : key) {
		if (!(IsAlpha(c) || IsDigit(c) || c == '_')) {
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
		std::string_view key;
		std::string_view value;
		if (HasTypedShape(field)) {
			key = field.substr(0, 2);
			value = field.substr(5);
			if (!IsValidValue(field[3], value)) {
				throw std::runtime_error(Quoted(field) + " has a value that is not valid for type '" +
				                         std::string(1, field[3]) + "'");
			}
		} else {
			size_t colon = field.find(':');
			bool untyped_ok = syntax == TagSyntax::TypedOrUntyped && colon != std::string_view::npos &&
			                  IsUntypedKey(field.substr(0, colon)) && AllPrintableOrSpace(field.substr(colon + 1));
			if (!untyped_ok) {
				throw std::runtime_error(Quoted(field) + " is not a tag (expected TAG:TYPE:VALUE" +
				                         std::string(syntax == TagSyntax::TypedOrUntyped ? " or key:value" : "") + ")");
			}
			key = field.substr(0, colon);
			value = field.substr(colon + 1);
		}
		for (const auto &existing : out) {
			if (existing.key == key) {
				throw std::runtime_error("tag " + Quoted(key) + " appears more than once");
			}
		}
		out.push_back({std::string(key), std::string(value)});
	}
}

} // namespace miint
