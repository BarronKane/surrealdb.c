<br>

<p align="center">
    <img width=120 src="https://raw.githubusercontent.com/surrealdb/icons/main/surreal.svg" />
    &nbsp;
    <img width=120 src="https://raw.githubusercontent.com/surrealdb/icons/main/c.svg" />
</p>

<h3 align="center">The official SurrealDB SDK for C.</h3>

<br>

<p align="center">
    <a href="https://github.com/surrealdb/surrealdb.c"><img src="https://img.shields.io/badge/status-beta-ff00bb.svg?style=flat-square"></a>
    &nbsp;
    <a href="https://surrealdb.com/docs/integration/libraries/c"><img src="https://img.shields.io/badge/docs-view-44cc11.svg?style=flat-square"></a>
    &nbsp;
</p>

<p align="center">
    <a href="https://surrealdb.com/discord"><img src="https://img.shields.io/discord/902568124350599239?label=discord&style=flat-square&color=5a66f6"></a>
    &nbsp;
    <a href="https://twitter.com/surrealdb"><img src="https://img.shields.io/badge/twitter-follow_us-1d9bf0.svg?style=flat-square"></a>
    &nbsp;
    <a href="https://www.linkedin.com/company/surrealdb/"><img src="https://img.shields.io/badge/linkedin-connect_with_us-0a66c2.svg?style=flat-square"></a>
    &nbsp;
    <a href="https://www.youtube.com/channel/UCjf2teVEuYVvvVC-gFZNq6w"><img src="https://img.shields.io/badge/youtube-subscribe-fc1c1c.svg?style=flat-square"></a>
</p>

# surrealdb.c

The official SurrealDB SDK for C.

## Getting started

```c
#include "surrealdb.h"

sr_surreal_t *db;
sr_string_t err;

// "mem://"                   in-memory
// "surrealkv://database.skv" local file
// "ws://localhost:8000"      remote server
if (sr_connect(&err, &db, "mem://") < 0)
{
    printf("failed to connect: %s\n", err);
    sr_string_free(err);
    return 1;
}

sr_use_ns(db, &err, "test");
sr_use_db(db, &err, "test");

sr_surreal_disconnect(db);
```

Calls return a negative status on failure — `SR_ERROR`, `SR_CLOSED` or
`SR_FATAL` — and write a message to `err_ptr` for the caller to release with
`sr_string_free`. The sign carries the meaning: positive is a result, `SR_AGAIN`
(zero) means nothing was available and the call is worth repeating, and negative
means stop. Notification streams report their end with `SR_CLOSED`. Passing `NULL` for `err_ptr` discards the message. Anything a
`sr_*_new` or `sr_value_*` constructor hands back is released by the matching
`sr_*_free`.

Requires SurrealDB 3.2.

## Runtime options

`sr_connect_with_options` takes timeouts and a capability sandbox. Every field's
zero value means "SurrealDB's default", so a zero-initialised `sr_option_t`
behaves exactly like `sr_connect`.

```c
sr_option_t opts = {0};
opts.query_timeout = 30;                              // seconds

// Capabilities are a sandbox: outbound network access is denied by default,
// and scripting is off. Name what you want to change and nothing else.
opts.capabilities.scripting = SR_TOGGLE_ON;

static const char *const allowed[] = { "example.com:443" };
opts.capabilities.allow_network.mode  = SR_TARGET_SOME;
opts.capabilities.allow_network.items = allowed;
opts.capabilities.allow_network.len   = 1;

if (sr_connect_with_options(&err, &db, "mem://", opts) < 0)
{
    printf("failed to connect: %s\n", err);   // e.g. an unparseable target
    sr_string_free(err);
    return 1;
}
```

Each set takes `SR_TARGET_DEFAULT`, `_NONE`, `_SOME` or `_ALL`, with `items`
read only for `_SOME`. Where both an allow and a deny set name the same thing,
deny wins. A name that cannot be parsed is reported as an error rather than
skipped, so a typo cannot quietly widen or narrow the sandbox.

The experimental features are opted into the same way — `"gql"` is required by
the `gql` and `graphql` RPC methods, and `"files"` by `file://` values.

## Thread footprint

A database embedded in someone else's process should not claim every core, so
since 0.3.2 it does not. One `mem://` context on a 32-core host:

```
0.3.1                       68 threads   (32 tokio + 32 KVS + 4)
0.3.2 defaults              40 threads   (4 tokio  + 32 KVS + 4)
  + sr_runtime_init(pool 4) 12 threads
  + current_thread           8 threads
```

Sessions are free: three `sr_rpc_session_attach` calls leave residency
unchanged. The cost is per context.

Two settings, in two places, because the lifetimes differ.

### Per context — `sr_option_t`

```c
sr_option_t opts = {0};
opts.worker_threads = 2;        // 0 = SR_DEFAULT_WORKER_THREADS (4), not core count
opts.current_thread = true;     // a mode, not a count: the slimmest option
opts.max_blocking_threads = 8;  // a cap on lazily-spawned threads, not residency
opts.thread_keep_alive_ms = 500;// how fast burst threads retire
opts.thread_stack_size = 0;     // bytes; Linux reserves 8 MiB per thread by default
opts.disable_io = false;        // see below
opts.temporary_directory = "/path/for/scratch";
opts.slow_log_ms = 0;           // see the warning in the header before enabling
```

`disable_io` drops tokio's IO driver, which an embedded-only context never
needs — but it is also what `http://` and `ws://` endpoints and SurrealQL's
`http::*` functions are built on, so it is off by default.

`temporary_directory` matters beyond tidiness: the platform temp directory is
not always writable, or correct, on console and mobile targets.

### Per process — `sr_runtime_init`

SurrealDB's blocking pool is built **once per process**, on first use. A
per-context field would be honoured for the first context and silently ignored
for every later one, so it lives on its own call:

```c
sr_runtime_options_t rt = {0};
rt.kvs_threadpool_size = 4;     // minimum 4, enforced by SurrealDB
sr_runtime_init(&err, &rt);     // BEFORE any sr_connect* or sr_surreal_rpc_new
```

Call it first or not at all — afterwards the pool exists and there is no way
for it to report that it had no effect.

Two reasons this is worth setting even if the count seems fine:

- **The default pins one worker per core**, whenever the resolved size equals
  the core count and that count is at least 16 — the default path on any modern
  desktop. Inside a game engine, which manages its own affinity, that is
  actively unhelpful. **Any** explicit value that differs from the core count
  drops pinning, so setting this is worth doing for that alone.
- **The floor is 16, not the core count.** A host with fewer than 16 cores still
  takes 16 threads here, so the defaults get relatively worse as the machine
  gets smaller.

`SURREAL_KVS_THREADPOOL_SIZE` does the same thing if you would rather configure
it from the environment.

## Live queries

`sr_select_live` returns a stream. Read it with `sr_stream_next_timeout` and
retire it with `sr_stream_kill`:

```c
sr_notification_t note;
while (running) {
    int got = sr_stream_next_timeout(stream, &note, 250);
    if (got > 0)            { handle(&note); sr_notification_free(note); }
    else if (got == SR_AGAIN) continue;   // nothing yet
    else                     break;      // SR_CLOSED, or an error
}
sr_stream_kill(stream);
```

`sr_stream_next` blocks without a bound. It is the natural call for a dedicated
reader thread and it is **not recommended on the version currently pinned**: a
killed live query does not report its end, so a reader parked there can wait for
a notification that cannot arrive, and nothing can release it — `sr_stream_kill`
frees the stream that reader is borrowing. `REMOVE TABLE` puts a stream in the
same state without anyone asking for it.

That is an upstream defect on the embedded engines, fixed by
[surrealdb/surrealdb#7520](https://github.com/surrealdb/surrealdb/pull/7520). Two
tripwire tests in `c_test/src/tests/stream_tests.c` pin the current behaviour and
will fail once the fix is released and the dependency bumped — that failure is
the signal to switch this recommendation back.

A `ws://` endpoint has the same symptom from an unrelated cause: the WebSocket
client rejects `KILLED` when decoding it and drops the frame silently, fixed by
[#7521](https://github.com/surrealdb/surrealdb/pull/7521). So the advice holds on
both transports for now, and neither fix subsumes the other.

### Tearing one down takes both calls

```c
sr_kill(db, &err, query_id);   // retires the subscription in the datastore
sr_stream_kill(stream);        // frees the local reader
```

Neither does the other's job, and the order between them does not matter.
`sr_stream_kill` does spawn a kill through the SDK, but fire-and-forget with the
result discarded, and it observably does not land: measured with `INFO FOR
TABLE`, the table's `lives` map still holds the subscription afterwards, and
only `sr_kill` empties it. A test pins that.

Getting `query_id` is the awkward part. `sr_select_live` hands back a stream and
not the id, and the SDK keeps its own copy private, so the only way to learn it
is from `sr_notification_t.query_id` on the first notification — a live query
that never fires cannot be retired by id, and goes when the connection does.
Running `LIVE SELECT ...` through `sr_query` answers with the id instead, but
then there is no stream to read.

## Sessions

An RPC context (`sr_surreal_rpc_new`) carries a session map. Since SurrealDB 3.1
every request names a session explicitly, so the context mints one for itself
and `sr_surreal_rpc_execute` runs against it.

```c
sr_uuid_t id = {{0}};                       // all-zero: generate an id
sr_rpc_session_attach(rpc, &err, &id);
sr_rpc_execute_on(rpc, &err, &res, &id, request, request_len);
sr_rpc_session_detach(rpc, &err, &id);      // also cancels its live queries
```

`sr_rpc_session_list` reports the active ids (free with `sr_uuid_arr_free`) and
`sr_rpc_session_default` gives the one the context created. Setting
`opts.session_dir` persists sessions to disk, so an attached session outlives
the context that created it.

### Sessions on the typed path

`sr_session_fork` gives a second `sr_surreal_t` that talks to the same engine
over the same runtime, with its own `USE` namespace and database, its own `LET`
variables, and its own authentication:

```c
sr_surreal_t *player = NULL;
sr_session_fork(db, &err, &player);      // shares the engine, not the state
sr_use_db(player, &err, "tenant_b");     // does not touch `db`
...
sr_surreal_disconnect(player);           // freed like any other handle
```

A fork is a session, not a connection: no engine is started, no runtime is
built, and no threads are added. `sr_session_new` does the same and then clears
the inherited authentication.

Poisoning is shared. If any handle reports `SR_FATAL` the engine is gone, so
every handle derived from it is poisoned too — the alternative would leave
siblings looking healthy while talking to a dead engine.

### Two clients, and which to reach for

`sr_connect` gives the typed calls — `sr_query`, `sr_select`, `sr_create` and
the rest — over `sr_value_t`, with sessions via `sr_session_fork`.
`sr_surreal_rpc_new` speaks SurrealDB's RPC protocol in CBOR, with sessions
addressed by uuid.

The RPC context is the **escape hatch**: it reaches the whole protocol surface,
including methods the typed API does not wrap, which is what you want for proxy
work, protocol tooling, or anything the typed calls cannot express. It is fully
supported, not deprecated.

For application code that wants sessions without hand-encoding CBOR,
`sr_rpc_query_on` runs a query on a chosen session and returns the same
`sr_arr_res_t` array `sr_query` does. Session state — `USE`, auth, variables —
applies, so two sessions can sit on different namespaces and not see each
other's writes.

## Building

```sh
cargo build --release
```

Produces `target/release/libsurrealdb_c.a` and writes `include/surrealdb.h`;

Include the header and link the archive.

```sh
cc app.c -Iinclude target/release/libsurrealdb_c.a -lm -ldl -lpthread     # Linux
```

On macOS, replace the trailing libraries with `-framework Security
-framework SystemConfiguration -framework CoreFoundation -framework IOKit
-lobjc`. Link the archive by path rather than with `-lsurrealdb_c`, which would
otherwise find the shared library built alongside it.

### Testing

```sh
cargo test
```

Runs the whole C test corpus and every example in `c_test/examples`, with no
CMake step. The examples are complete programs compiled and run as tests, so
one that stops building or stops working fails the suite.

### CMake

Not required to build or run anything -- `cargo build` produces the library and
`cargo test` runs the suite. CMake is optional, for running the same corpus
under `ctest` and for installing the header and archive so consumers can
`find_package` them.

```sh
cmake -S . -B build && cmake --build build
cmake --install build --prefix /usr/local
ctest --test-dir build
```

Consumers then get the include path and the platform libraries for free:

```cmake
find_package(surrealdb_c REQUIRED)
target_link_libraries(myapp PRIVATE surrealdb::surrealdb_c)
```

Needs CMake 3.21 or newer and a C23 compiler.
