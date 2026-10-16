#include "sc_coo_builder.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace miint {

namespace {

//! Backing storage for one exported Arrow array, owned by its `private_data`.
//!
//! sc accepts only the simplest possible arrays -- it rejects `null_count != 0`
//! and `offset != 0` -- so there is never a validity bitmap to build.
//! `buffers[0]` is always NULL, which is Arrow's encoding for "this array has
//! no nulls at all".
struct BufferBag {
	std::vector<int64_t> i64;
	std::vector<double> f64;
	std::vector<int32_t> offsets;
	std::vector<char> chars;
	const void *buffers[3] = {nullptr, nullptr, nullptr};
};

// The array's release callback: frees the BufferBag behind its buffers.
void ReleaseArray(ArrowArray *array) {
	if (!array->release) {
		return;
	}
	delete static_cast<BufferBag *>(array->private_data);
	array->private_data = nullptr;
	// Setting release to NULL is how a consumer detects an already-released
	// array; it makes double-release a no-op rather than a double-free.
	array->release = nullptr;
}

void ReleaseSchema(ArrowSchema *schema) {
	if (!schema->release) {
		return;
	}
	// `format` points at a static string literal and `name` is NULL, so there
	// is nothing to free — only the released marker to set.
	schema->release = nullptr;
}

void InitSchema(ArrowSchema &schema, const char *format) {
	schema.format = format;
	schema.name = nullptr;
	schema.metadata = nullptr;
	// 0 rather than ARROW_FLAG_NULLABLE: these arrays carry no nulls, and sc
	// rejects any that claim to.
	schema.flags = 0;
	schema.n_children = 0;
	schema.children = nullptr;
	schema.dictionary = nullptr;
	schema.release = ReleaseSchema;
	schema.private_data = nullptr;
}

// Fill `array` over `bag`'s buffers; the array owns the bag from here on.
void InitArray(ArrowArray &array, int64_t length, int64_t n_buffers, BufferBag *bag) {
	array.length = length;
	array.null_count = 0;
	array.offset = 0;
	array.n_buffers = n_buffers;
	array.n_children = 0;
	array.buffers = bag->buffers;
	array.children = nullptr;
	array.dictionary = nullptr;
	array.release = ReleaseArray;
	array.private_data = bag;
}

//! Export `values` as an Arrow `Int64` array (format "l"): [validity, data].
void ExportInt64(ArrowArray &array, ArrowSchema &schema, std::vector<int64_t> values) {
	auto bag = std::make_unique<BufferBag>();
	bag->i64 = std::move(values);
	bag->buffers[0] = nullptr;
	bag->buffers[1] = bag->i64.data();
	const auto length = static_cast<int64_t>(bag->i64.size());
	InitArray(array, length, 2, bag.release());
	InitSchema(schema, "l");
}

} // namespace

//! Export `values` as an Arrow `Float64` array (format "g"): [validity, data].
void ExportFloat64(ArrowArray &array, ArrowSchema &schema, std::vector<double> values) {
	auto bag = std::make_unique<BufferBag>();
	bag->f64 = std::move(values);
	bag->buffers[0] = nullptr;
	bag->buffers[1] = bag->f64.data();
	const auto length = static_cast<int64_t>(bag->f64.size());
	InitArray(array, length, 2, bag.release());
	InitSchema(schema, "g");
}

//! Export `values` as an Arrow `Utf8` array (format "u"): [validity, offsets, data].
//!
//! Offsets hold `n + 1` entries and count bytes, not characters; row `i` is
//! `chars[offsets[i] .. offsets[i + 1]]`.
void ExportUtf8(ArrowArray &array, ArrowSchema &schema, const std::vector<std::string> &values) {
	// Owned here until the array takes it, so a throw on the way (an allocation,
	// or the size check below) frees it.
	auto bag = std::make_unique<BufferBag>();
	size_t total = 0;
	for (const auto &v : values) {
		total += v.size();
	}
	// Utf8 offsets are int32: past 2 GiB of text they would wrap and point
	// every later id at the wrong bytes.
	if (total > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
		throw std::length_error("ExportUtf8: " + std::to_string(total) +
		                        " bytes of ids exceed the 2 GiB an Arrow Utf8 array can address");
	}
	bag->offsets.reserve(values.size() + 1);
	bag->chars.reserve(total);
	int32_t cursor = 0;
	bag->offsets.push_back(cursor);
	for (const auto &v : values) {
		bag->chars.insert(bag->chars.end(), v.begin(), v.end());
		cursor += static_cast<int32_t>(v.size());
		bag->offsets.push_back(cursor);
	}
	// [validity, offsets, chars]. No nulls, so no validity buffer.
	bag->buffers[0] = nullptr;
	bag->buffers[1] = bag->offsets.data();
	// An all-empty-string column allocates nothing; a NULL data buffer with
	// every offset at 0 is well-formed, and sc reads no data pointer when the
	// column's total byte length is 0.
	bag->buffers[2] = bag->chars.empty() ? nullptr : bag->chars.data();
	InitArray(array, static_cast<int64_t>(values.size()), 3, bag.release());
	InitSchema(schema, "u");
}

namespace {

void ReleaseIfLive(ArrowArray &array, ArrowSchema &schema) {
	if (array.release) {
		array.release(&array);
	}
	if (schema.release) {
		schema.release(&schema);
	}
}

} // namespace

// sc borrows these arrays and never releases them, so the table does.
ScCooTable::~ScCooTable() {
	ReleaseIfLive(table_.rows, table_.rows_schema);
	ReleaseIfLive(table_.cols, table_.cols_schema);
	ReleaseIfLive(table_.vals, table_.vals_schema);
	ReleaseIfLive(table_.sample_ids, table_.sample_ids_schema);
	ReleaseIfLive(table_.feature_ids, table_.feature_ids_schema);
}

// `ids` keeps first-seen order; Finalize sorts it and remaps the indices.
int64_t ScCooBuilder::Intern(std::unordered_map<std::string, int64_t> &index, std::vector<std::string> &ids,
                             std::string_view id) {
	key_.assign(id.data(), id.size());
	auto it = index.find(key_);
	if (it != index.end()) {
		return it->second;
	}
	const auto next = static_cast<int64_t>(ids.size());
	ids.emplace_back(id);
	index.emplace(ids.back(), next);
	return next;
}

void ScCooBuilder::SetFeatureVocabulary(std::vector<std::string> vocab) {
	feature_ids_ = std::move(vocab);
	feature_index_.clear();
	for (size_t i = 0; i < feature_ids_.size(); i++) {
		// First occurrence wins, so the index always points at the model's own
		// column for that feature.
		feature_index_.emplace(feature_ids_[i], static_cast<int64_t>(i));
	}
	has_fixed_features_ = true;
	dropped_cells_ = 0;
}

void ScCooBuilder::Append(std::string_view sample_id, std::string_view feature_id, double value) {
	// ------------------ samples ------------------
	// Intern the sample first, unconditionally: see Append's doc for the
	// sample whose every feature is unknown.
	const auto row = Intern(sample_index_, sample_ids_, sample_id);
	// Intern hands out sequential indices, so the per-sample counters only ever
	// need to grow by one.
	if (static_cast<size_t>(row) == sample_cells_.size()) {
		sample_cells_.push_back(0);
		sample_matched_.push_back(0);
	}
	sample_cells_[static_cast<size_t>(row)]++;

	// ------------------ features ------------------
	int64_t col;
	if (has_fixed_features_) {
		// A hash lookup, not a binary search: a vocabulary this extension fit is
		// sorted, but one from a model fit through sc's C API need not be, and a
		// binary search over it would resolve features wrongly without an error.
		key_.assign(feature_id.data(), feature_id.size());
		const auto it = feature_index_.find(key_);
		if (it == feature_index_.end()) {
			dropped_cells_++; // the model has no column for it; counted for the warning
			return;
		}
		col = it->second;
	} else {
		col = Intern(feature_index_, feature_ids_, feature_id);
	}
	sample_matched_[static_cast<size_t>(row)]++;

	rows_.push_back(row);
	cols_.push_back(col);
	vals_.push_back(value);
}

namespace {

//! Sort `ids` in place by their bytes and return remap[old index] = new index,
//! for rewriting the indices interned in arrival order. For ids
//! ['Zebra', 'Apple', 'Mango'] it returns [2, 0, 1].
std::vector<int64_t> SortDictionary(std::vector<std::string> &ids) {
	// Argsort by the ids' bytes.
	std::vector<int64_t> order(ids.size());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(),
	          [&ids](int64_t a, int64_t b) { return ids[static_cast<size_t>(a)] < ids[static_cast<size_t>(b)]; });

	// order[new] = old, so invert it into remap[old] = new.
	std::vector<int64_t> remap(ids.size());
	std::vector<std::string> sorted;
	sorted.reserve(ids.size());
	for (size_t newpos = 0; newpos < order.size(); newpos++) {
		const auto oldpos = static_cast<size_t>(order[newpos]);
		remap[oldpos] = static_cast<int64_t>(newpos);
		sorted.push_back(std::move(ids[oldpos]));
	}
	// The strings were already moved into `sorted`; moving the vector back hands
	// its buffer to `ids` without copying them again.
	ids = std::move(sorted);
	return remap;
}

} // namespace

namespace {

//! Pack a (row, col) index pair into one sortable key: row in the upper 32 bits,
//! col in the lower. Both are dictionary
//! positions, so they are bounded by the number of distinct samples / features
//! and fit in 32 bits for any table that could be built in memory.
inline uint64_t PackCell(int64_t row, int64_t col) {
	return (static_cast<uint64_t>(row) << 32) | static_cast<uint32_t>(col);
}

} // namespace

DuplicateReport ScCooBuilder::FindDuplicateCells(size_t max_examples) const {
	DuplicateReport report;
	if (rows_.size() < 2) {
		return report;
	}

	std::vector<uint64_t> keys;
	keys.reserve(rows_.size());
	for (size_t i = 0; i < rows_.size(); i++) {
		keys.push_back(PackCell(rows_[i], cols_[i]));
	}
	std::sort(keys.begin(), keys.end());

	// Duplicates are adjacent once sorted, so walk the runs of equal keys. Count
	// every run longer than one; keep only the first few keys for the report.
	std::vector<uint64_t> wanted;
	for (size_t i = 0; i < keys.size();) {
		size_t j = i + 1;
		while (j < keys.size() && keys[j] == keys[i]) {
			j++;
		}
		if (j - i > 1) {
			report.duplicate_cells++;
			if (wanted.size() < max_examples) {
				wanted.push_back(keys[i]);
			}
		}
		i = j;
	}
	if (wanted.empty()) {
		return report;
	}

	// Second pass over the triples, collecting values for the sampled keys only,
	// so the report can show whether they are identical or differ.
	std::vector<DuplicateCell> cells(wanted.size());
	for (size_t i = 0; i < rows_.size(); i++) {
		const auto key = PackCell(rows_[i], cols_[i]);
		for (size_t w = 0; w < wanted.size(); w++) {
			if (wanted[w] != key) {
				continue;
			}
			auto &cell = cells[w];
			if (cell.count == 0) {
				cell.sample_id = sample_ids_[static_cast<size_t>(rows_[i])];
				cell.feature_id = feature_ids_[static_cast<size_t>(cols_[i])];
			}
			cell.count++;
			cell.values.push_back(vals_[i]);
			break;
		}
	}
	report.examples = std::move(cells);
	return report;
}

std::unique_ptr<ScCooTable> ScCooBuilder::Finalize() {
	// sc rejects a 0 x N or N x 0 matrix ("dimensions must be > 0"), so an
	// empty input has no representable table. Say so here rather than
	// building one sc will refuse.
	if (sample_ids_.empty() || feature_ids_.empty()) {
		return nullptr;
	}

	// Sorted ids, so arrival order cannot reach sc (see the class comment). For
	// samples it matters because bootstrap draws and fold assignment pick rows
	// by position: an unstable order would give one seed different models.
	const auto sample_remap = SortDictionary(sample_ids_);
	for (auto &r : rows_) {
		r = sample_remap[static_cast<size_t>(r)];
	}
	// Features are sorted too, unless the vocabulary was fixed to a model's: then
	// the column order is the model's definition of what each column means, and
	// re-sorting would re-point every learned split at a different feature.
	if (!has_fixed_features_) {
		const auto feature_remap = SortDictionary(feature_ids_);
		for (auto &c : cols_) {
			c = feature_remap[static_cast<size_t>(c)];
		}
	}

	// The known fraction rides along the same remap as the dictionary, so it stays
	// aligned with SampleIds().
	std::vector<double> known_fraction(sample_ids_.size(), 1.0);
	for (size_t old_pos = 0; old_pos < sample_cells_.size(); old_pos++) {
		const auto seen = sample_cells_[old_pos];
		const auto matched = sample_matched_[old_pos];
		const auto at = static_cast<size_t>(sample_remap[old_pos]);
		known_fraction[at] = seen > 0 ? static_cast<double>(matched) / static_cast<double>(seen) : 0.0;
	}

	// Owned from here, so an Export* that throws leaves nothing behind.
	auto out = std::make_unique<ScCooTable>();
	out->known_feature_fraction_ = std::move(known_fraction);
	out->table_.n_samples = static_cast<int64_t>(sample_ids_.size());
	out->table_.n_features = static_cast<int64_t>(feature_ids_.size());
	ExportInt64(out->table_.rows, out->table_.rows_schema, std::move(rows_));
	ExportInt64(out->table_.cols, out->table_.cols_schema, std::move(cols_));
	ExportFloat64(out->table_.vals, out->table_.vals_schema, std::move(vals_));
	ExportUtf8(out->table_.sample_ids, out->table_.sample_ids_schema, sample_ids_);
	ExportUtf8(out->table_.feature_ids, out->table_.feature_ids_schema, feature_ids_);
	out->sample_ids_ = std::move(sample_ids_);
	out->feature_ids_ = std::move(feature_ids_);

	sample_index_.clear();
	feature_index_.clear();
	has_fixed_features_ = false;
	dropped_cells_ = 0;
	sample_cells_.clear();
	sample_matched_.clear();
	sample_ids_.clear();
	feature_ids_.clear();
	rows_.clear();
	cols_.clear();
	vals_.clear();
	return out;
}

ScCooBatcher::ScCooBatcher(const ScCooTable &table) : table_(table) {
	const auto n_samples = static_cast<size_t>(table.table_.n_samples);
	const auto nnz = static_cast<size_t>(table.table_.rows.length);
	const auto *rows = static_cast<const int64_t *>(table.table_.rows.buffers[1]);

	// Counting sort of cell indices by sample: count, prefix-sum, place. Stable,
	// so a sample's cells keep the order they had in the full table.
	sample_start_.assign(n_samples + 1, 0);
	for (size_t i = 0; i < nnz; i++) {
		sample_start_[static_cast<size_t>(rows[i]) + 1]++;
	}
	for (size_t s = 0; s < n_samples; s++) {
		sample_start_[s + 1] += sample_start_[s];
	}
	cell_order_.resize(nnz);
	std::vector<size_t> next(sample_start_.begin(), sample_start_.end() - 1);
	for (size_t i = 0; i < nnz; i++) {
		cell_order_[next[static_cast<size_t>(rows[i])]++] = i;
	}
}

std::unique_ptr<ScCooTable> ScCooBatcher::Batch(size_t first, size_t count) const {
	const size_t n_samples = sample_start_.size() - 1;
	if (count == 0 || first > n_samples || count > n_samples - first) {
		throw std::out_of_range("ScCooBatcher::Batch: samples [" + std::to_string(first) + ", " +
		                        std::to_string(first + count) + ") are outside a table of " +
		                        std::to_string(n_samples));
	}
	const auto &src = table_.table_;
	const auto *src_rows = static_cast<const int64_t *>(src.rows.buffers[1]);
	const auto *src_cols = static_cast<const int64_t *>(src.cols.buffers[1]);
	const auto *src_vals = static_cast<const double *>(src.vals.buffers[1]);

	const size_t begin = sample_start_[first];
	const size_t end = sample_start_[first + count];
	std::vector<int64_t> rows, cols;
	std::vector<double> vals;
	rows.reserve(end - begin);
	cols.reserve(end - begin);
	vals.reserve(end - begin);
	for (size_t k = begin; k < end; k++) {
		const size_t i = cell_order_[k];
		rows.push_back(src_rows[i] - static_cast<int64_t>(first));
		cols.push_back(src_cols[i]);
		vals.push_back(src_vals[i]);
	}

	auto out = std::make_unique<ScCooTable>();
	const auto lo = static_cast<std::ptrdiff_t>(first);
	const auto hi = static_cast<std::ptrdiff_t>(first + count);
	out->sample_ids_.assign(table_.sample_ids_.begin() + lo, table_.sample_ids_.begin() + hi);
	out->known_feature_fraction_.assign(table_.known_feature_fraction_.begin() + lo,
	                                    table_.known_feature_fraction_.begin() + hi);
	out->feature_ids_ = table_.feature_ids_;
	out->table_.n_samples = static_cast<int64_t>(count);
	out->table_.n_features = src.n_features;
	ExportInt64(out->table_.rows, out->table_.rows_schema, std::move(rows));
	ExportInt64(out->table_.cols, out->table_.cols_schema, std::move(cols));
	ExportFloat64(out->table_.vals, out->table_.vals_schema, std::move(vals));
	ExportUtf8(out->table_.sample_ids, out->table_.sample_ids_schema, out->sample_ids_);
	ExportUtf8(out->table_.feature_ids, out->table_.feature_ids_schema, out->feature_ids_);
	return out;
}

} // namespace miint
