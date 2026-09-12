//! Rust driver for the C test corpus.
//!
//! The tests themselves are C, in `c_test/src/tests/`, and are shared with the
//! CTest driver — this package only provides a second way to run them, so that
//! `cargo test` is a complete verification path with no CMake step.
//!
//! Granularity is per group rather than per test: Unity's `TEST()` macro emits
//! a `static` function, but `TEST_GROUP_RUNNER()` emits an externally linkable
//! `RunTestGroup_*`, which is what `sr_run_test_group` dispatches to.

/// Keeps `surrealdb_c` in the link graph so the C objects can resolve the
/// `sr_*` symbols they call.
#[doc(hidden)]
pub fn link_anchor() -> usize {
    surrealdb_c::Surreal::connect as usize
}

#[cfg(test)]
mod tests {
    use std::ffi::CString;
    use std::os::raw::{c_char, c_int};
    use std::sync::Mutex;

    extern "C" {
        /// Runs one Unity group and returns the number of failed assertions.
        fn sr_run_test_group(group: *const c_char) -> c_int;
    }

    /// Unity accumulates results in globals and is not thread safe, so the
    /// groups run one at a time. The whole corpus takes seconds, so there is
    /// nothing to gain from running them concurrently.
    static UNITY: Mutex<()> = Mutex::new(());

    fn run_group(group: &str) {
        // A panic in one group poisons the lock; the remaining groups should
        // still report their own results rather than cascade.
        let _guard = UNITY.lock().unwrap_or_else(|poisoned| poisoned.into_inner());

        super::link_anchor();

        let name = CString::new(group).expect("group name");
        let failures = unsafe { sr_run_test_group(name.as_ptr()) };

        assert_eq!(failures, 0, "{group}: {failures} failing test(s)");
    }

    macro_rules! test_group {
        ($name:ident, $group:literal) => {
            #[test]
            fn $name() {
                run_group($group);
            }
        };
    }

    // The legacy api_tests corpus, driven through the same bridge.
    test_group!(legacy_api, "LegacyApi");

    test_group!(connection, "Connection");
    test_group!(transaction, "Transaction");
    test_group!(crud, "CRUD");
    test_group!(query, "Query");
    test_group!(io, "IO");
    test_group!(auth, "Auth");
    test_group!(variable, "Variable");
    test_group!(rpc, "RPC");
    test_group!(stream, "Stream");
    test_group!(object, "Object");
    test_group!(memory, "Memory");
    test_group!(utility, "Utility");
}
