#pragma once
#include <string>
#include <htslib-1.22.1/htslib/hfile.h>

#ifdef MIINT_STATIC_BUILD
#include "duckdb/common/file_system.hpp"

namespace miint {

// Create an hFILE backed by DuckDB's FileHandle.
// Works for both local files and remote URLs (https://, s3://, etc.) — DuckDB's
// FileSystem dispatches to the appropriate I/O backend (local, httpfs, etc.).
//
// Caller passes result to SAMReader's hFILE constructor or to hts_hopen() directly.
// The hFILE takes ownership of the FileHandle; hts_close() will clean it up
// via the hFILE backend's close callback.
//
// Never returns nullptr. Throws DuckDB's exception when the file cannot be opened, so its reason (a
// missing extension, a missing object, credentials) reaches the user, and an IOException when htslib
// cannot allocate the hFILE.
hFILE *hfile_duckdb_open(duckdb::FileSystem &fs, const std::string &path);

} // namespace miint

#endif
