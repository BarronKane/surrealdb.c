#![allow(non_camel_case_types)]

//! Connection options and the capability sandbox.

use std::collections::HashSet;
use std::ffi::{c_char, c_int, CStr};
use std::hash::Hash;

use surrealdb_core::dbs::capabilities::{
    ArbitraryQueryTarget, Capabilities, EvalQueryTarget, ExperimentalTarget, FuncTarget,
    MethodTarget, NetTarget, RouteTarget, Targets,
};

/// A three-state switch.
///
/// Zero means "leave the library default alone", so a zero-initialised
/// `Options` changes nothing -- which is the only safe reading for a
/// sandbox.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Toggle {
    /// Keep SurrealDB's default.
    SR_TOGGLE_DEFAULT = 0,
    SR_TOGGLE_ON = 1,
    SR_TOGGLE_OFF = 2,
}

/// Which members of a capability set are allowed or denied.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TargetMode {
    /// Keep SurrealDB's default for this set.
    SR_TARGET_DEFAULT = 0,
    /// Empty set.
    SR_TARGET_NONE = 1,
    /// Exactly the names listed in `items`.
    SR_TARGET_SOME = 2,
    /// Everything.
    SR_TARGET_ALL = 3,
}

/// A capability target set.
///
/// `items` is read only when `mode` is `SR_TARGET_SOME`, and holds `len`
/// null-terminated strings. The accepted spellings are SurrealDB's own -- for
/// example `"http::get"` for a function, `"1.2.3.4/8"` or `"example.com:80"`
/// for a network target, `"select"` for an RPC method, `"gql"` for an
/// experimental feature. An unrecognised name is reported as an error rather
/// than ignored, so a typo cannot silently widen or narrow the sandbox.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct TargetSet {
    pub mode: TargetMode,
    pub items: *const *const c_char,
    pub len: c_int,
}

/// The capability sandbox.
///
/// Every field defaults to "leave SurrealDB's default alone", so
/// `CapabilitySet caps = {0};` is a no-op. Where both an allow and a deny
/// set are given, deny wins -- that is SurrealDB's rule, not this library's.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CapabilitySet {
    /// Embedded scripting functions. Off by default.
    pub scripting: Toggle,
    /// Unauthenticated access. Off by default.
    pub guest_access: Toggle,
    /// Live query notifications. On by default.
    pub live_query_notifications: Toggle,

    /// Built-in and custom functions. Allowed by default.
    pub allow_functions: TargetSet,
    pub deny_functions: TargetSet,

    /// Outbound network access. **Denied by default**; this is the field to set
    /// before anything can reach the network.
    pub allow_network: TargetSet,
    pub deny_network: TargetSet,

    /// RPC methods. Allowed by default.
    pub allow_rpc_methods: TargetSet,
    pub deny_rpc_methods: TargetSet,

    /// HTTP routes. Allowed by default.
    pub allow_http_routes: TargetSet,
    pub deny_http_routes: TargetSet,

    /// Experimental features -- `"gql"`, `"files"`, `"surrealism"`. Denied by
    /// default. `gql` is required by the `gql`/`graphql` RPC methods, and
    /// `files` by `file://` values (`sr_value_file`).
    pub allow_experimental: TargetSet,
    pub deny_experimental: TargetSet,

    /// Which authentication levels may run arbitrary queries.
    /// Names are `"guest"`, `"record"`, `"system"`.
    pub allow_arbitrary_query: TargetSet,
    pub deny_arbitrary_query: TargetSet,

    /// Which authentication levels may run `eval`-style queries.
    pub allow_eval_query: TargetSet,
    pub deny_eval_query: TargetSet,
}

/// Connection options.
///
/// Zero-initialise and set only what you need: every field's zero value means
/// "SurrealDB's default".
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct Options {
    /// Query timeout in seconds. Zero leaves the default in place.
    pub query_timeout: u8,
    /// Transaction timeout in seconds. Zero leaves the default in place.
    pub transaction_timeout: u8,
    /// The capability sandbox.
    pub capabilities: CapabilitySet,
    /// Directory in which to persist RPC sessions, or null to disable.
    ///
    /// When set, a session attached to an RPC context is written here as JSON
    /// and is rehydrated on demand, so sessions survive the context being torn
    /// down and rebuilt. Ignored by `sr_connect_with_options`, which has no
    /// session map of its own.
    ///
    /// # These files hold credentials
    ///
    /// A session is serialised whole, and a session carries its authentication
    /// token, its record-authentication data and its variables. They are
    /// written as plain JSON: nothing here is encrypted or obfuscated, and
    /// anything that can read the file can replay the session.
    ///
    /// The library restricts the directory and its files to the current user
    /// (0700 / 0600 on unix; on other platforms the inherited ACL is all there
    /// is). That is sufficient on a server, which is what upstream built this
    /// for. It is not a disk-encryption scheme, so treat the directory as
    /// credential storage: keep it off shared or synced volumes, and think hard
    /// before enabling it on hardware the end user controls.
    pub session_dir: *const c_char,

    // ------------------------------------------------------------------
    // Tokio runtime
    // ------------------------------------------------------------------
    //
    // This library builds the runtime, which no Rust consumer needs -- they
    // arrive with one they already sized. The C boundary had to invent one and
    // it inherited a server's shape: `Runtime::new()` claims a worker per core.
    // Inside a host process that is the wrong default, so these exist and the
    // default is conservative. See `sr_runtime_init` for the pool that is not
    // per context.
    /// Tokio worker threads. Zero means the library default (see
    /// `SR_DEFAULT_WORKER_THREADS`), not the core count.
    pub worker_threads: c_int,
    /// Run the whole runtime on one thread. Not a count but a mode: the
    /// scheduler itself is single-threaded, which is the genuinely slim option
    /// for intermittent workloads. Overrides `worker_threads` when set.
    pub current_thread: bool,
    /// Ceiling on lazily-spawned blocking threads. Zero means tokio's default
    /// of 512. This is a cap, not residency -- they spawn on demand and retire
    /// after `thread_keep_alive_ms`.
    pub max_blocking_threads: c_int,
    /// How long an idle blocking thread lingers, in milliseconds. Zero means
    /// tokio's default of 10s. Lower means less sawtooth after a burst.
    pub thread_keep_alive_ms: c_int,
    /// Stack size per worker thread, in bytes. Zero means the platform default,
    /// which on Linux reserves 8 MiB of address space per thread.
    pub thread_stack_size: c_int,
    /// Skip the IO driver. Zero-cost for an embedded-only context, but it is
    /// what `http://` and `ws://` endpoints -- and SurrealQL's `http::*`
    /// functions -- are built on, so setting this on a context that reaches the
    /// network makes those fail. Off by default for that reason.
    pub disable_io: bool,

    // ------------------------------------------------------------------
    // Datastore
    // ------------------------------------------------------------------
    /// Directory for temporary files, or null for the platform default.
    ///
    /// The platform temp directory is not always the right answer, or writable
    /// at all, on console and mobile targets, and a host application usually
    /// has its own sanctioned scratch location.
    pub temporary_directory: *const c_char,
    // There is no `lazy_surrealism` knob here on purpose. SurrealDB gates
    // `Builder::with_lazy_surrealism` behind its `surrealism` feature, which
    // this library does not enable -- so exposing it would mean compiling in
    // the extension system in order to defer initialising it, and with the
    // feature off there is nothing to defer. A field that silently does
    // nothing is worse than no field.
    /// Log queries slower than this many milliseconds. Zero disables it.
    ///
    /// Ignored by `sr_connect_with_options`, which does not build the datastore
    /// directly.
    ///
    /// # Slow-query logs include bound parameters
    ///
    /// SurrealDB logs the statement *and its parameters*, and parameters are
    /// how credentials travel -- a `signin` carries its password as one. The
    /// log goes wherever the host application's `tracing` subscriber points,
    /// which on a client machine may be a file that outlives the process.
    /// Treat enabling this as a decision about credential handling, not just
    /// verbosity.
    pub slow_log_ms: c_int,
}

/// Worker threads a context takes when `worker_threads` is left at zero.
///
/// Deliberately not the core count. A database embedded in someone else's
/// process should not claim every core by default for work the host does
/// intermittently; a caller who wants more can say so, and the failure mode of
/// this default is a slow query rather than a starved host.
pub const SR_DEFAULT_WORKER_THREADS: usize = 4;

impl Options {
    /// Build the tokio runtime a context will own.
    ///
    /// `None` means the caller passed no options at all, which takes the same
    /// conservative defaults rather than reverting to `Runtime::new()` -- the
    /// silent path should not be the maximal one.
    pub(crate) fn build_runtime(opts: Option<&Options>) -> Result<tokio::runtime::Runtime, String> {
        use tokio::runtime::Builder;

        let single = opts.map(|o| o.current_thread).unwrap_or(false);
        let mut b = if single {
            Builder::new_current_thread()
        } else {
            let mut b = Builder::new_multi_thread();
            let n = match opts.map(|o| o.worker_threads).unwrap_or(0) {
                0 => SR_DEFAULT_WORKER_THREADS.min(num_cpus_or(SR_DEFAULT_WORKER_THREADS)),
                n if n < 0 => return Err("worker_threads cannot be negative".to_string()),
                n => n as usize,
            };
            b.worker_threads(n.max(1));
            b
        };

        // Named so a host application's profiler can tell whose threads these
        // are. Free, and the alternative is a wall of `tokio-rt-worker`.
        b.thread_name("surrealdb-c");

        if let Some(o) = opts {
            if o.disable_io {
                // Time only. The IO driver is what network endpoints need; an
                // embedded-only context never touches it.
                b.enable_time();
            } else {
                b.enable_all();
            }
            if o.max_blocking_threads > 0 {
                b.max_blocking_threads(o.max_blocking_threads as usize);
            }
            if o.thread_keep_alive_ms > 0 {
                b.thread_keep_alive(std::time::Duration::from_millis(
                    o.thread_keep_alive_ms as u64,
                ));
            }
            if o.thread_stack_size > 0 {
                b.thread_stack_size(o.thread_stack_size as usize);
            }
        } else {
            b.enable_all();
        }

        b.build().map_err(|e| format!("error creating runtime: {e}"))
    }

    /// The temporary directory, if one was supplied.
    pub fn temporary_directory_path(&self) -> Option<std::path::PathBuf> {
        cstr(self.temporary_directory).map(std::path::PathBuf::from)
    }
}

fn num_cpus_or(fallback: usize) -> usize {
    std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(fallback)
}

/// Process-wide settings, applied once before any connection is opened.
///
/// Separate from `sr_option_t` because the lifetime is different. A per-context
/// field carrying a process-global setting is a trap: the second context's
/// value is silently ignored, and nothing at the call site says so.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct RuntimeOptions {
    /// Size of SurrealDB's shared blocking pool. Zero leaves it alone.
    ///
    /// The default is one worker per core on hosts with 16 or more cores, and
    /// 16 below that -- so a 32-core machine spends 32 threads here before a
    /// single query runs, and an 8-core one still spends 16. Worse for a host
    /// application, the default *pins* one worker per core when the size equals
    /// the core count and that count is at least 16, which fights an engine
    /// that manages its own affinity. Any value that differs from the core
    /// count drops pinning, so setting this is worth doing for that alone.
    ///
    /// Clamped to a minimum of 4 by SurrealDB itself.
    pub kvs_threadpool_size: c_int,
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

/// Turn a C target set into a `Targets<T>`, or `None` to leave the default.
///
/// # Safety
///
/// `items` must hold `len` valid C strings when `mode` is `SR_TARGET_SOME`.
fn to_targets<T>(t: &TargetSet, what: &str) -> Result<Option<Targets<T>>, String>
where
    T: std::str::FromStr + Hash + Eq + Ord,
    <T as std::str::FromStr>::Err: std::fmt::Display,
{
    match t.mode {
        TargetMode::SR_TARGET_DEFAULT => Ok(None),
        TargetMode::SR_TARGET_NONE => Ok(Some(Targets::None)),
        TargetMode::SR_TARGET_ALL => Ok(Some(Targets::All)),
        TargetMode::SR_TARGET_SOME => {
            if t.items.is_null() || t.len <= 0 {
                // An explicitly empty SOME is the same as NONE, and saying so
                // is friendlier than rejecting it.
                return Ok(Some(Targets::None));
            }
            let mut set: HashSet<T> = HashSet::new();
            for i in 0..t.len as usize {
                let raw = unsafe { *t.items.add(i) };
                let Some(s) = cstr(raw) else {
                    return Err(format!("{what}: entry {i} is null or not valid UTF-8"));
                };
                match s.parse::<T>() {
                    Ok(v) => {
                        set.insert(v);
                    }
                    // Reported rather than skipped: silently dropping a name
                    // the caller meant to allow or deny would change the
                    // sandbox without telling anyone.
                    Err(e) => return Err(format!("{what}: cannot parse '{s}': {e}")),
                }
            }
            Ok(Some(Targets::Some(set)))
        }
    }
}

fn apply_toggle(current: bool, t: Toggle) -> bool {
    match t {
        Toggle::SR_TOGGLE_DEFAULT => current,
        Toggle::SR_TOGGLE_ON => true,
        Toggle::SR_TOGGLE_OFF => false,
    }
}

impl CapabilitySet {
    /// Build a `Capabilities` from this description, or `None` if nothing was
    /// set (in which case the caller should not touch the builder at all).
    pub fn to_capabilities(&self) -> Result<Option<Capabilities>, String> {
        let mut touched = false;
        let d = Capabilities::default();
        let mut caps = Capabilities::default();

        let scripting = apply_toggle(false, self.scripting);
        let guest = apply_toggle(false, self.guest_access);
        let lqn = apply_toggle(true, self.live_query_notifications);
        if self.scripting != Toggle::SR_TOGGLE_DEFAULT
            || self.guest_access != Toggle::SR_TOGGLE_DEFAULT
            || self.live_query_notifications != Toggle::SR_TOGGLE_DEFAULT
        {
            touched = true;
        }
        caps = caps
            .with_scripting(scripting)
            .with_guest_access(guest)
            .with_live_query_notifications(lqn);

        macro_rules! set {
            ($field:ident, $setter:ident, $ty:ty, $label:expr) => {
                if let Some(t) = to_targets::<$ty>(&self.$field, $label)? {
                    caps = caps.$setter(t);
                    touched = true;
                }
            };
        }

        set!(allow_functions, with_functions, FuncTarget, "allow_functions");
        set!(deny_functions, without_functions, FuncTarget, "deny_functions");
        set!(allow_network, with_network_targets, NetTarget, "allow_network");
        set!(deny_network, without_network_targets, NetTarget, "deny_network");
        set!(allow_rpc_methods, with_rpc_methods, MethodTarget, "allow_rpc_methods");
        set!(deny_rpc_methods, without_rpc_methods, MethodTarget, "deny_rpc_methods");
        set!(allow_http_routes, with_http_routes, RouteTarget, "allow_http_routes");
        set!(deny_http_routes, without_http_routes, RouteTarget, "deny_http_routes");
        set!(allow_experimental, with_experimental, ExperimentalTarget, "allow_experimental");
        set!(deny_experimental, without_experimental, ExperimentalTarget, "deny_experimental");
        set!(
            allow_arbitrary_query,
            with_arbitrary_query,
            ArbitraryQueryTarget,
            "allow_arbitrary_query"
        );
        set!(
            deny_arbitrary_query,
            without_arbitrary_query,
            ArbitraryQueryTarget,
            "deny_arbitrary_query"
        );
        set!(allow_eval_query, with_eval_query, EvalQueryTarget, "allow_eval_query");
        set!(deny_eval_query, without_eval_query, EvalQueryTarget, "deny_eval_query");

        let _ = d;
        Ok(if touched { Some(caps) } else { None })
    }
}

impl Options {
    pub fn session_dir_path(&self) -> Option<std::path::PathBuf> {
        cstr(self.session_dir).map(std::path::PathBuf::from)
    }
}

/// Apply process-wide settings. Call once, before opening any connection.
///
/// Returns 1 when the settings were applied, `SR_NONE` when there was nothing
/// to do, and `SR_ERROR` with a message when a value is rejected.
///
/// # This is not idempotent, and cannot be
///
/// SurrealDB's blocking pool is built once per process, on first use, from an
/// environment variable read behind a `LazyLock`. So this must run before the
/// first `sr_connect*` or `sr_surreal_rpc_new` call. Afterwards it has no
/// effect -- and, because the pool is already built, no way to report that it
/// had none. Calling it first is the caller's side of the bargain; the
/// alternative was a per-context field that lies on every context after the
/// first.
///
/// # Safety
///
/// - `err_ptr` must be a valid pointer or null
/// - `opts` must be a valid pointer to a `sr_runtime_options_t`, or null (no-op)
#[export_name = "sr_runtime_init"]
pub extern "C" fn runtime_init(
    err_ptr: *mut crate::string::string_t,
    opts: *const RuntimeOptions,
) -> c_int {
    if opts.is_null() {
        return crate::SR_NONE;
    }
    let opts = unsafe { &*opts };

    if opts.kvs_threadpool_size < 0 {
        crate::write_error(err_ptr, "kvs_threadpool_size cannot be negative");
        return crate::SR_ERROR;
    }
    if opts.kvs_threadpool_size == 0 {
        return crate::SR_NONE;
    }

    // The env var is the only control SurrealDB exposes for this; there is no
    // builder method and no per-datastore override.
    //
    // SAFETY: documented as "before any connection", which is also before any
    // thread this library creates. A caller that ignores that is racing their
    // own setup, not ours.
    unsafe {
        std::env::set_var(
            "SURREAL_KVS_THREADPOOL_SIZE",
            opts.kvs_threadpool_size.to_string(),
        );
    }
    1
}
