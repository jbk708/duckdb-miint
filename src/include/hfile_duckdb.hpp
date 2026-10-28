#pragma once
#include <string>
#include "SAMReader.hpp"

#ifdef MIINT_STATIC_BUILD
#include "duckdb/common/file_system.hpp"

namespace miint {

// Create an hFILE backed by DuckDB's FileHandle.
// Works for both local files and remote URLs (https://, s3://, etc.) — DuckDB's
// FileSystem dispatches to the appropriate I/O backend (local, httpfs, etc.).
//
// Caller passes result to SAMReader's hFILE constructor, or releases it to hts_hopen().
// The hFILE takes ownership of the FileHandle; closing the hFILE (hclose(), or hts_close()
// once htslib has it) cleans it up via the hFILE backend's close callback.
//
// Never returns nullptr. Throws DuckDB's exception when the file cannot be opened, so its reason (a
// missing extension, a missing object, credentials) reaches the user, and an IOException when htslib
// cannot allocate the hFILE.
HFilePtr hfile_duckdb_open(duckdb::FileSystem &fs, const std::string &path);

} // namespace miint

#endif
