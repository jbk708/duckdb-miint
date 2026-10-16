// Umbrella staticlib for rype + sylph + sc. Cargo treats the upstream crates
// as rlib dependencies, rustc emits one staticlib that links them through a
// single Rust std, and their `#[no_mangle] extern "C"` symbols survive into the
// archive's export table. `extern crate ... as _` forces the linkage without
// exposing names. The only code of its own is miint_rust_glue_init below.

extern crate rype as _;

#[cfg(feature = "with-sylph")]
extern crate sylph as _;

extern crate sc_capi as _;

/// Seed rayon's global thread pool for a build that cannot spawn threads.
///
/// rype and sc run their parallel work on rayon's global pool. DuckDB-Wasm's
/// single-threaded builds have no threads to give it, and Emscripten's
/// `pthread_create` stub reports EAGAIN rather than "unsupported" -- the one
/// error rayon falls back to the calling thread on -- so the first parallel
/// call into either crate panics instead. A one-thread pool that *is* the
/// calling thread runs the same work inline, and sc's results do not depend on
/// the thread count.
///
/// Called once when the extension loads, before any rype or sc call. A no-op
/// outside Emscripten, where rayon builds its default pool on first use.
#[no_mangle]
pub extern "C" fn miint_rust_glue_init() {
    #[cfg(target_os = "emscripten")]
    {
        // Err only if a global pool already exists, which is then the one to keep.
        let _ = rayon::ThreadPoolBuilder::new()
            .num_threads(1)
            .use_current_thread()
            .build_global();
    }
}
