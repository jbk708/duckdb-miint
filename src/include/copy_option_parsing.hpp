#pragma once

// How every miint COPY format reads its options. Each format declares its options, with DeclareCopyOption,
// in its CopyFunction::copy_options callback, and DuckDB refuses an option the format does not declare,
// and a NULL for any option. For an option declared with a type, DuckDB also refuses no argument (unless
// BOOLEAN), a list, or an argument that does not cast to the type, with the same errors as for its own
// formats, and casts the rest before the format's bind sees them. An option declared ANY reaches the bind
// with its arguments as written; read it with CopyOptionValue.

#include "duckdb/function/copy_function.hpp"
#include "remote_file_helper.hpp"

namespace duckdb {

//! A BOOLEAN option. Bare (no argument) means true, as for CSV's HEADER.
inline bool CopyOptionFlag(const vector<Value> &values) {
	return values.empty() || BooleanValue::Get(values[0]);
}

//! An option declared ANY, whose arguments DuckDB does not count or type-check.
inline const Value &CopyOptionValue(const Identifier &name, const vector<Value> &values) {
	if (values.size() != 1) {
		throw BinderException("Copy option \"%s\" requires a single argument", name.GetIdentifierName());
	}
	return values[0];
}

inline void DeclareCopyOption(CopyOptionsInput &input, const char *name, const LogicalType &type) {
	input.options[name] = CopyOption(type, CopyOptionMode::WRITE_ONLY);
}

//! COMPRESSION_LEVEL, for every format that writes BGZF. Declared BIGINT, so DuckDB hands
//! CopyOptionCompressionLevel exactly one non-NULL BIGINT.
inline void DeclareCompressionLevelCopyOption(CopyOptionsInput &input) {
	DeclareCopyOption(input, "compression_level", LogicalType::BIGINT);
}

inline int CopyOptionCompressionLevel(const vector<Value> &values) {
	const auto level = values[0].GetValue<int64_t>();
	if (level < 0 || level > 9) {
		throw BinderException("COMPRESSION_LEVEL must be between 0 and 9, got %d", level);
	}
	return static_cast<int>(level);
}

//! For formats whose writer opens the target itself and can write only local files: htslib (SAM, BAM,
//! UBAM), built without libcurl, and HDF5 (BIOM). miint's DuckDB-backed hFILE reads only.
inline void RefuseRemoteCopyTarget(const char *format, const string &file_path) {
	if (miint::RemoteFileHelper::IsRemotePath(file_path)) {
		throw BinderException("COPY FORMAT %s writes local files only, not '%s'", format, file_path);
	}
}

//! For the end of a bind's if-chain: an option the format declares but no branch reads.
inline void ThrowUnreadCopyOption(const char *format, const Identifier &name) {
	throw InternalException("COPY FORMAT %s declares option \"%s\" but does not read it", format,
	                        name.GetIdentifierName());
}

} // namespace duckdb
