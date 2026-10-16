#pragma once

#include "sc_coo_builder.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"

#include <string>
#include <vector>

#include "sc.h"

namespace miint {

//! An sc context, freed on destruction.
//!
//! sc's handles are opaque heap allocations with their own destructors. Wrapping
//! them keeps the many error paths in the table functions from leaking on a
//! throw.
struct ScContext {
	sc_context_t *ptr = nullptr;
	ScContext() = default;
	~ScContext();
	// Not copyable: two copies would free the same context.
	ScContext(const ScContext &) = delete;
	ScContext &operator=(const ScContext &) = delete;
};

//! A trained sc model, freed on destruction.
struct ScModel {
	sc_model_t *ptr = nullptr;
	ScModel() = default;
	~ScModel();
	ScModel(const ScModel &) = delete;
	ScModel &operator=(const ScModel &) = delete;
};

//! An Arrow array sc exported to us, released on destruction.
//!
//! Note the ownership direction, which is the reverse of `ScCooBuilder`'s. sc's
//! *imports* borrow -- it reads our buffers in place and never releases them --
//! but its *exports* move: `to_ffi` installs a release callback and the consumer
//! must invoke it exactly once. Skip it and the buffers leak; call it twice and
//! it is a double free.
class OwnedArrowArray {
public:
	OwnedArrowArray() = default;
	~OwnedArrowArray();
	OwnedArrowArray(const OwnedArrowArray &) = delete;
	OwnedArrowArray &operator=(const OwnedArrowArray &) = delete;

	//! Out-slots for an sc `export_*` call.
	ArrowArray *array() {
		return &array_;
	}
	ArrowSchema *schema() {
		return &schema_;
	}

	//! The Arrow format string sc stamped on the export, or nullptr if the
	//! out-slot was never filled. "u" = Utf8, "g" = Float64.
	const char *Format() const {
		return schema_.format;
	}

	//! Decode a `Utf8` array (format "u"): buffers are [validity, offsets, data]
	//! and row i is data[offsets[i] .. offsets[i + 1]].
	std::vector<std::string> ReadUtf8(const char *what) const;
	//! Decode a `Float64` array (format "g"): buffers are [validity, data].
	std::vector<double> ReadFloat64(const char *what) const;

	//! Decode a `FixedSizeList<Float64>[width]` (format "+w:N") as one flat
	//! row-major vector of `length * width` doubles, returning `width`.
	//!
	//! Unlike the flat arrays above, this layout is nested: the outer array
	//! carries no values of its own -- `n_buffers` is 1 (validity only) and the doubles
	//! live in `children[0]`, a plain Float64 array of `length * width`. The
	//! width is not a field: it is encoded in the format string itself, after
	//! the "+w:" prefix.
	//!
	//! Kept flat rather than nested because every caller immediately walks it
	//! row by row -- proba is one row per sample of n_classes, SHAP one row per
	//! sample of n_outputs * n_features.
	std::vector<double> ReadFixedSizeListFloat64(const char *what, int64_t &width) const;
	//! The same values read in place: a pointer to the first of `length * width`
	//! doubles inside the child buffer, valid while this object lives, or nullptr
	//! when there are none. For outputs too large to copy -- SHAP's
	//! samples x outputs x features matrix.
	const double *FixedSizeListFloat64Data(const char *what, int64_t &width) const;

private:
	ArrowArray array_ {};
	ArrowSchema schema_ {};
};

//! Take ownership of an array sc moved into a result struct. The C Data
//! Interface permits moving these structs: copy the bytes, then mark the source
//! released so nothing frees it twice. Call it for every field before checking
//! the status, so an error path still cleans up.
void TakeArray(ArrowArray &src, ArrowSchema &src_schema, OwnedArrowArray &dst);

//! Turn a non-zero `sc_status_t` into a DuckDB error, preferring sc's own
//! message from the context's last-error slot. Pass the context only for a
//! call that took it: sc records nothing for one that did not, so the slot
//! could only hold an earlier, unrelated message.
[[noreturn]] void ThrowSc(const char *what, sc_context_t *ctx, sc_status_t status);

//! Open an sc context with a pool of `n_threads` threads, throwing as `caller`
//! on failure. Callers that run sc in parallel pass the count bind resolved
//! from `threads`; 0 builds no pool, for a context only used to load a model.
//!
//! `n_threads` is ignored under Emscripten. DuckDB-Wasm has one thread, the
//! global pool is seeded on it at load (miint_rust_glue_init), and a dedicated
//! pool would need threads it cannot spawn. sc's results are identical at any
//! thread count, so only speed differs.
void OpenScContext(int32_t n_threads, const char *caller, ScContext &out);

//! The `task` values a fit writes into its model row and bind reads back.
constexpr const char *kClassification = "classification";
constexpr const char *kRegression = "regression";

inline const char *TaskName(bool classification) {
	return classification ? kClassification : kRegression;
}

//! Refuse a NULL named parameter. Omitting one means its default; a NULL would
//! otherwise read as that default, or for `name` select the relation's only
//! model.
void RequireNotNull(const std::string &parameter, const duckdb::Value &v, const char *caller);

//! What bind reads from a model row to type its output.
struct ScModelHeader {
	//! "classification" or "regression"; empty when not read.
	std::string task;
	//! The type of the feature ids the model was fit on.
	duckdb::LogicalType feature_id_type;
};

//! Read a model row's `feature_id_type`, and its `task` when `with_task`, in one
//! query: every read of a view re-runs it.
//!
//! A table function must declare its return types at bind time, and a
//! prediction column is VARCHAR for a classifier and DOUBLE for a regressor.
//! The model already knows which it is -- sc's `sc_predict` dispatches on
//! `forest.task()` -- but sc exposes no C accessor for it, so the fit writes it
//! into the model relation and this reads it back. Deciding the schema from the
//! data is the same thing `read_csv` does when it sniffs a file.
ScModelHeader ReadModelHeader(duckdb::Connection &conn, const std::string &relation, const std::string &name,
                              const char *caller, bool with_task);

//! Build the `WHERE name = '...'` clause for a model lookup, or an empty string
//! when no name was given.
//!
//! Only the *value* comes from the caller, and it goes through
//! `KeywordHelper::WriteQuoted`. That is what makes this safe where a general
//! `where :=` parameter would not be: the predicate's shape is ours, so there is
//! no way to inject a second clause.
std::string ModelNameFilter(const std::string &name);

//! The SQL types of a data relation's id columns.
struct ScIdTypes {
	duckdb::LogicalType sample_id = duckdb::LogicalType::VARCHAR;
	duckdb::LogicalType feature_id = duckdb::LogicalType::VARCHAR;
};

//! Check a data relation's COO triplet shape and read its id column types
//! without scanning a row, rejecting any id type outside VARCHAR/BIGINT/UUID.
//!
//! Ids travel through sc as text either way; the types are what each function
//! casts its id output back to, so a BIGINT id comes back BIGINT.
ScIdTypes ProbeIdTypes(duckdb::Connection &conn, const std::string &relation, const char *caller);

//! Stream a data relation's `(sample_id, feature_id, value)` cells into
//! `builder`, rejecting NULLs. The one intake shared by every sc function that
//! reads counts, so they all accept and reject the same input.
void ScanCooRelation(duckdb::ClientContext &context, duckdb::Connection &conn, const std::string &relation,
                     const char *caller, ScCooBuilder &builder);

//! Scan a data relation against a model's feature vocabulary: the intake every
//! function that applies a model shares, so they accept, drop and warn alike.
//! Cells of features the model was not trained on are dropped and reported in
//! a warning; a sample left with none is counted in it, followed by
//! `empty_effect`, the consequence for this caller's output. Data sharing no
//! feature with the model is refused.
std::unique_ptr<ScCooTable> ScanForModel(duckdb::ClientContext &context, duckdb::Connection &conn, const ScModel &model,
                                         const std::string &relation, const char *caller, const char *empty_effect);

//! Read sc's class probabilities, one row of `n_classes` per sample, flat and
//! row-major, checking the width and length before any caller indexes them.
std::vector<double> ReadProbaMatrix(const OwnedArrowArray &proba, size_t n_classes, size_t n_samples, const char *what);

//! Reject a `(sample_id, feature_id)` pair that appears more than once.
//!
//! sc sums duplicates, which is right for repeat measurements and wrong for a
//! join fanout, and it cannot tell them apart -- so fitting and applying a
//! model both refuse rather than guess. The message shows the values: identical
//! ones point at a fanout (deduplicate), differing ones at repeat measurements
//! (aggregate deliberately).
void RequireNoDuplicateCells(const ScCooBuilder &builder, const char *caller, const std::string &relation);

//! Reject data whose feature_id type differs from the model's.
//!
//! Features are matched by their text, so a BIGINT 7 would silently match a
//! VARCHAR model's '7' but not its '007'. Requiring the exact type makes the
//! caller choose the cast instead.
void RequireModelFeatureIdType(const duckdb::LogicalType &model_type, const duckdb::LogicalType &data_type,
                               const char *caller, const std::string &model_relation, const std::string &model_name,
                               const std::string &data_relation);

//! Read a serialized model out of `relation`'s `model_blob` column and
//! deserialize it.
//!
//! With `name` empty the relation itself must hold exactly one row. With a name,
//! rows are filtered to that name first -- so one table can be a registry of
//! many models and a call still names exactly one, without the caller having to
//! create a view to narrow it.
//!
//! Bind already read the model's `task` and `feature_id_type`, and typed the
//! output from them. They are read again here, in the same query as the blob,
//! and a change is an error: the relation returned a different model at
//! execution than at planning. `task` is empty for a caller that bound none.
void LoadModelFromRelation(duckdb::Connection &conn, const std::string &relation, const std::string &name,
                           const char *caller, const std::string &task, const duckdb::LogicalType &feature_id_type,
                           sc_context_t *ctx, ScModel &out);

} // namespace miint
