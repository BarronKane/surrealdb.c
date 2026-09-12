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
`sr_string_free`. Anything a `sr_*_new` or `sr_value_*` constructor hands back
is released by the matching `sr_*_free`.

Requires SurrealDB 3.2.

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

### CMake

Only needed to install the library or to build the tests.

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
