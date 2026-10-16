#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sc.h"

namespace miint {

//! Export `values` as an Arrow `Float64` / `Utf8` array that owns its buffers:
//! its release callback frees them. sc's imports borrow and never release, so
//! the caller does, after the sc call -- an `OwnedArrowArray` on scope exit.
void ExportFloat64(ArrowArray &array, ArrowSchema &schema, std::vector<double> values);
void ExportUtf8(ArrowArray &array, ArrowSchema &schema, const std::vector<std::string> &values);

//! Owns every buffer behind an `sc_coo_table_t` and releases them on destruction.
//!
//! sc borrows across the Arrow C Data Interface -- it reads the caller's buffers
//! in place and never invokes `release` -- so this object must outlive any sc
//! call that reads it.
class ScCooTable {
public:
	ScCooTable() = default;
	~ScCooTable();
	ScCooTable(const ScCooTable &) = delete;
	ScCooTable &operator=(const ScCooTable &) = delete;

	//! Borrowed; valid while this object lives.
	const sc_coo_table_t *get() const {
		return &table_;
	}
	int64_t NumSamples() const {
		return table_.n_samples;
	}
	int64_t NumFeatures() const {
		return table_.n_features;
	}
	//! Number of stored triples. Duplicates are not collapsed here; sc sums them,
	//! as scipy's COO does.
	int64_t NumNonZeros() const {
		return table_.rows.length;
	}

	//! Per sample, the fraction of its observed cells whose feature the model
	//! knows: `matched / observed`, in `SampleIds()` order.
	//!
	//! The denominator is the *sample's* features, not the model's. A fraction
	//! of the model's vocabulary is always tiny in sparse data -- a sample
	//! legitimately carries a handful of 200k features -- so it would flag
	//! everything. This ratio instead answers "how much of what I observed here
	//! can the model use", where a low value means the model is being
	//! applied to different data: another reference database, another pipeline,
	//! another 16S region.
	//!
	//! Always 1.0 without a fixed vocabulary, where every feature is known by
	//! construction.
	const std::vector<double> &KnownFeatureFraction() const {
		return known_feature_fraction_;
	}

	//! Dictionaries, in the sorted order the `cols` / `rows` indices refer to.
	const std::vector<std::string> &SampleIds() const {
		return sample_ids_;
	}
	const std::vector<std::string> &FeatureIds() const {
		return feature_ids_;
	}

private:
	friend class ScCooBuilder;
	friend class ScCooBatcher;
	sc_coo_table_t table_ {};
	std::vector<double> known_feature_fraction_;
	std::vector<std::string> sample_ids_;
	std::vector<std::string> feature_ids_;
};

//! Cuts a finalized table into standalone tables of consecutive samples.
//!
//! For a caller whose sc output grows with the samples in one call -- sc_shap's
//! dense attribution matrix -- and so must bound it. sc scores each sample
//! independently, so how samples are grouped changes no result.
//!
//! The cells are indexed by sample once, one `size_t` per cell; each batch then
//! costs only its own cells rather than a scan of the whole table. `table` must
//! outlive this object.
class ScCooBatcher {
public:
	explicit ScCooBatcher(const ScCooTable &table);

	//! Samples `[first, first + count)` as their own table: rows renumbered from
	//! 0, the same feature columns and vocabulary, known fractions carried along. A
	//! batch whose samples have no cells is valid -- all-zero rows.
	std::unique_ptr<ScCooTable> Batch(size_t first, size_t count) const;

private:
	const ScCooTable &table_;
	//! Sample s's cells are cell_order_[sample_start_[s] .. sample_start_[s + 1]].
	std::vector<size_t> sample_start_;
	std::vector<size_t> cell_order_;
};

//! One `(sample_id, feature_id)` pair that was appended more than once, with the
//! values that were seen for it.
//!
//! The values tell a caller which repair applies (see RequireNoDuplicateCells),
//! so a report should show them.
struct DuplicateCell {
	std::string sample_id;
	std::string feature_id;
	size_t count = 0;
	std::vector<double> values;
};

//! What [`ScCooBuilder::FindDuplicateCells`] found.
struct DuplicateReport {
	//! Distinct `(sample_id, feature_id)` pairs appearing more than once.
	size_t duplicate_cells = 0;
	//! A bounded sample of them, for an error message.
	std::vector<DuplicateCell> examples;

	bool Empty() const {
		return duplicate_cells == 0;
	}
};

//! Accumulates long-format `(sample_id, feature_id, value)` cells and emits the
//! COO feature table sc expects.
//!
//! This is the bridge between the shape DuckDB has and the shape sc wants.
//! `read_biom`, `woltka_ogu`, and anything else projected to those
//! three columns all arrive here — sc is coupled to the triple, not to a file
//! format.
//!
//! Both dictionaries are sorted, so a given set of ids always produces the same
//! integer encoding whatever order the rows arrive in, and the same seed gives
//! the same model. It also matches the reference pipeline sc's own fixtures
//! were generated with, which sorts both axes. The exception is a fixed
//! vocabulary (SetFeatureVocabulary): the feature dictionary is then the
//! model's own order.
class ScCooBuilder {
public:
	//! Ids are copied on first sight and interned thereafter.
	//!
	//! With a fixed vocabulary (see [`SetFeatureVocabulary`]) a feature the
	//! model never saw is dropped and counted. The sample is interned either
	//! way, so a sample whose every feature was dropped still gets a row: an
	//! all-zero one, the sparse form of "none of the model's features were
	//! observed here". It still receives a prediction rather than vanishing
	//! from the output.
	void Append(std::string_view sample_id, std::string_view feature_id, double value);

	//! Encode features against a model's training vocabulary instead of
	//! deriving one from the data.
	//!
	//! This is what makes a model reusable across datasets. Column `i` means
	//! `vocab[i]` because that is what the model was trained on -- so the
	//! vocabulary is used in the model's own order and is not re-sorted. Two
	//! datasets that differ by one feature in each direction have the same
	//! width, and sc validates only the width, never which feature a column
	//! holds, so re-deriving an encoding here would give predictions from the
	//! wrong columns with no error.
	//!
	//! So, for a sparse matrix:
	//!   * a feature not in `vocab` is dropped -- the model has no column for it
	//!   * a feature in `vocab` but absent from the data is not stored, and
	//!     absent already means zero
	//!   * `n_features` is always `vocab.size()`, never what the data happened
	//!     to contain
	//!
	//! Must be called before the first [`Append`].
	void SetFeatureVocabulary(std::vector<std::string> vocab);

	//! Cells dropped because their feature is not in the fixed vocabulary.
	//!
	//! Callers report it: a prediction table sharing almost no features with the
	//! model still yields confident predictions from a near-empty matrix, and
	//! nothing else in the pipeline says so.
	size_t DroppedCells() const {
		return dropped_cells_;
	}

	//! Report `(sample_id, feature_id)` pairs appended more than once.
	//!
	//! Reports without a policy: sc sums duplicates, and whether that is right
	//! is the caller's decision (see RequireNoDuplicateCells).
	//!
	//! Costs one temporary `uint64` per stored cell, freed on return, plus an
	//! O(n log n) sort over a packed (row, col) key, rather than a hash set's
	//! allocation per entry. The wasm build shares one 4 GiB linear memory
	//! between DuckDB and this extension, so the temporary competes with
	//! DuckDB's own memory there.
	//!
	//! Safe to call before [`Finalize`]; it reads the interned indices and does
	//! not modify them.
	DuplicateReport FindDuplicateCells(size_t max_examples = 5) const;

	//! Sorts the dictionaries, remaps the interned indices, and builds the
	//! Arrow arrays. The builder is left empty and reusable.
	//!
	//! With a fixed vocabulary only the sample dictionary is sorted; the
	//! feature columns already mean what the model says they mean.
	//!
	//! Returns nullptr if nothing was appended: sc rejects a 0-row or 0-column
	//! matrix outright, so an empty table has no valid representation.
	std::unique_ptr<ScCooTable> Finalize();

	size_t NumNonZeros() const {
		return vals_.size();
	}
	size_t NumSamples() const {
		return sample_index_.size();
	}
	size_t NumFeatures() const {
		return feature_index_.size();
	}

private:
	//! Intern `id`, returning its provisional (insertion-order) index.
	int64_t Intern(std::unordered_map<std::string, int64_t> &index, std::vector<std::string> &ids, std::string_view id);

	//! The key every lookup is made with, reused so that an id longer than the
	//! small-string buffer costs no allocation per cell.
	std::string key_;

	bool has_fixed_features_ = false;
	size_t dropped_cells_ = 0;

	std::unordered_map<std::string, int64_t> sample_index_;
	std::unordered_map<std::string, int64_t> feature_index_;
	std::vector<std::string> sample_ids_;
	std::vector<std::string> feature_ids_;
	//! Indexed by provisional sample index; remapped alongside the dictionary.
	std::vector<int64_t> sample_cells_;
	std::vector<int64_t> sample_matched_;
	std::vector<int64_t> rows_;
	std::vector<int64_t> cols_;
	std::vector<double> vals_;
};

} // namespace miint
