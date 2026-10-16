#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! sc_version() -> VARCHAR: the version of the linked sc random-forest library.
//!
//! The thinnest binding to sc: no Arrow, no context, no model. It shows the
//! sc_* symbols are linked and callable without exercising anything else.
class ScVersionFunction {
public:
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
