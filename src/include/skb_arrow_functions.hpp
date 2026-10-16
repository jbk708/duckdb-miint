#pragma once

#include "duckdb.hpp"

namespace duckdb {

/// Registers the table functions that call skb-arrow (skb_arrow/client.hpp):
/// - `skb_mantel(x, y, method, permutations, alternative, seed, threads)`:
///   the Mantel test between two condensed `(sample_a, sample_b, distance)`
///   relations. Returns one row: `statistic DOUBLE, pvalue DOUBLE, n BIGINT`.
void RegisterSkbArrowFunctions(ExtensionLoader &loader);

} // namespace duckdb
