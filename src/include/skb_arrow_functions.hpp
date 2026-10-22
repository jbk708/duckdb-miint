#pragma once

#include "duckdb.hpp"

namespace duckdb {

/// Registers the table functions that call skb-arrow (skb_arrow/client.hpp).
void RegisterSkbArrowFunctions(ExtensionLoader &loader);

} // namespace duckdb
