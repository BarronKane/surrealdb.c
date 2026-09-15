/*
 * Connection options: timeouts, the capability sandbox, and session
 * persistence.
 *
 * All three arrive together because they share one `sr_option_t`, and because
 * the guarantee worth testing is the same for each: a zero-initialised option
 * struct must change nothing, and anything the caller does set must actually
 * take effect rather than being quietly dropped.
 */

#include "unity_fixture.h"
#include "surrealdb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

TEST_GROUP(Options);

static sr_string_t err;

TEST_SETUP(Options) { err = NULL; }
TEST_TEAR_DOWN(Options) { if (err) { sr_string_free(err); err = NULL; } }

/* ------------------------------------------------------------ direct path */

TEST(Options, ZeroOptionsBehaveLikePlainConnect) {
    /* The whole design rests on this: every zero value means "SurrealDB's
       default", so {0} must be indistinguishable from sr_connect. */
    sr_option_t opts = {0};
    sr_surreal_t *db = NULL;
    int rc = sr_connect_with_options(&err, &db, "memory", opts);
    if (rc < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "connect_with_options{0} should succeed: %s",
                 err ? err : "unknown");
        if (err) { sr_string_free(err); err = NULL; }
        TEST_FAIL_MESSAGE(msg);
    }
    TEST_ASSERT_NOT_NULL(db);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sr_use_ns(db, &err, "o"));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sr_use_db(db, &err, "o"));

    sr_arr_res_t *res = NULL;
    int n = sr_query(db, &err, &res, "RETURN 1", NULL);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, n, "a trivial query should run");
    if (n > 0) sr_arr_res_arr_free(res, n);

    sr_surreal_disconnect(db);
}

TEST(Options, TimeoutsAreAccepted) {
    sr_option_t opts = {0};
    opts.query_timeout = 30;
    opts.transaction_timeout = 30;

    sr_surreal_t *db = NULL;
    int rc = sr_connect_with_options(&err, &db, "memory", opts);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, rc, "timeouts should be accepted");
    if (rc < 0) { if (err) { sr_string_free(err); err = NULL; } return; }

    sr_use_ns(db, &err, "o"); sr_use_db(db, &err, "o");
    sr_arr_res_t *res = NULL;
    int n = sr_query(db, &err, &res, "RETURN 1", NULL);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    if (n > 0) sr_arr_res_arr_free(res, n);
    sr_surreal_disconnect(db);
}

TEST(Options, ScriptingToggleIsHonoured) {
    /* Scripting is off in SurrealDB's defaults, so switching it on is an
       observable change in what the database will execute. */
    sr_option_t opts = {0};
    opts.capabilities.scripting = SR_TOGGLE_ON;

    sr_surreal_t *db = NULL;
    int rc = sr_connect_with_options(&err, &db, "memory", opts);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, rc, "scripting=ON should connect");
    if (rc < 0) { if (err) { sr_string_free(err); err = NULL; } return; }

    sr_use_ns(db, &err, "o"); sr_use_db(db, &err, "o");
    sr_arr_res_t *res = NULL;
    int n = sr_query(db, &err, &res, "RETURN function() { return 1 + 1; }", NULL);
    if (n < 0) {
        /* Scripting can be compiled out entirely; that is not this test's
           business, but a *capability* refusal would be. */
        TEST_ASSERT_NULL_MESSAGE(err ? strstr(err, "not allowed") : NULL,
                                 "scripting was enabled, so it must not be refused");
        if (err) { sr_string_free(err); err = NULL; }
    } else if (n > 0) {
        sr_arr_res_arr_free(res, n);
    }
    sr_surreal_disconnect(db);
}

TEST(Options, BadCapabilityNameIsReported) {
    /* A name that cannot be parsed must fail loudly. Silently dropping it
       would widen or narrow the sandbox without telling anyone -- the one
       outcome that must never happen in a security control. */
    static const char *const bogus[] = { "this-is-not-a-capability" };
    sr_option_t opts = {0};
    opts.capabilities.allow_experimental.mode = SR_TARGET_SOME;
    opts.capabilities.allow_experimental.items = bogus;
    opts.capabilities.allow_experimental.len = 1;

    sr_surreal_t *db = NULL;
    int rc = sr_connect_with_options(&err, &db, "memory", opts);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(0, rc, "an unparseable capability must be rejected");
    TEST_ASSERT_NOT_NULL_MESSAGE(err, "the rejection should say what failed");
    if (err) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "allow_experimental"),
                                     "the message should name the field");
        sr_string_free(err); err = NULL;
    }
    if (rc >= 0 && db) sr_surreal_disconnect(db);
}

TEST(Options, DenyOverridesAllowForRpcMethods) {
    /* Both sets are expressible, and SurrealDB's rule is that deny wins. */
    static const char *const allow[] = { "select", "query" };
    static const char *const deny[]  = { "query" };
    sr_option_t opts = {0};
    opts.capabilities.allow_rpc_methods.mode = SR_TARGET_SOME;
    opts.capabilities.allow_rpc_methods.items = allow;
    opts.capabilities.allow_rpc_methods.len = 2;
    opts.capabilities.deny_rpc_methods.mode = SR_TARGET_SOME;
    opts.capabilities.deny_rpc_methods.items = deny;
    opts.capabilities.deny_rpc_methods.len = 1;

    sr_surreal_t *db = NULL;
    int rc = sr_connect_with_options(&err, &db, "memory", opts);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, rc, "allow+deny should be accepted");
    if (rc < 0) { if (err) { sr_string_free(err); err = NULL; } return; }
    sr_surreal_disconnect(db);
}

TEST(Options, TargetModesAreAccepted) {
    /* NONE / ALL need no item list; SOME with an empty list degrades to NONE
       rather than erroring. */
    sr_option_t opts = {0};
    opts.capabilities.allow_functions.mode = SR_TARGET_ALL;
    opts.capabilities.allow_network.mode = SR_TARGET_NONE;
    opts.capabilities.allow_http_routes.mode = SR_TARGET_SOME;
    opts.capabilities.allow_http_routes.items = NULL;
    opts.capabilities.allow_http_routes.len = 0;

    sr_surreal_t *db = NULL;
    int rc = sr_connect_with_options(&err, &db, "memory", opts);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, rc, "all target modes should be accepted");
    if (rc < 0) { if (err) { sr_string_free(err); err = NULL; } return; }
    sr_surreal_disconnect(db);
}

static void rm_rf_sessions(const char *dir);

/* ------------------------------------------------- runtime footprint */

#if defined(__linux__)
#include <stdio.h>
#include <unistd.h>
static int thread_count(void) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}
#endif

/*
 * A context must not claim a worker per core.
 *
 * Until 0.3.2 the runtime was `Runtime::new()`, which is one tokio worker per
 * core -- 32 on the machine this was written on, for a database the host uses
 * intermittently. No Rust consumer meets that default because they bring their
 * own runtime; it existed only because the C boundary had to invent one and
 * inherited a server's shape.
 *
 * Thread residency is the whole point of the change, so it is asserted directly
 * rather than trusting that the builder was called.
 */
TEST(Options, DefaultRuntimeDoesNotScaleWithCores) {
#if defined(__linux__)
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 8) {
        TEST_IGNORE_MESSAGE("needs a host with enough cores for the old default to differ");
    }

    int before = thread_count();
    sr_option_t opts = {0};
    sr_surreal_rpc_t *rpc = NULL;
    if (sr_surreal_rpc_new(&err, &rpc, "memory", opts) < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        TEST_FAIL_MESSAGE("rpc context should be created");
    }
    int after = thread_count();
    sr_surreal_rpc_disconnect(rpc);

    int added = after - before;
    /* The KVS pool is process-global and sized elsewhere, so this bounds the
       total rather than the tokio pool alone: the assertion is simply that
       residency no longer tracks the core count. */
    char msg[160];
    snprintf(msg, sizeof(msg),
             "a context added %d threads on a %ld-core host; it should not scale with cores",
             added, cores);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE((int)cores * 2, added, msg);
#else
    TEST_IGNORE_MESSAGE("thread residency is checked on Linux only");
#endif
}

/* A single-threaded runtime is a mode, not a count, and it must still work. */
TEST(Options, CurrentThreadRuntimeStillServesQueries) {
    sr_option_t opts = {0};
    opts.current_thread = true;

    sr_surreal_rpc_t *rpc = NULL;
    if (sr_surreal_rpc_new(&err, &rpc, "memory", opts) < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        TEST_FAIL_MESSAGE("a current_thread context should be created");
    }

    sr_uuid_t id = {{0}};
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sr_rpc_session_attach(rpc, &err, &id));

    sr_arr_res_t *res = NULL;
    int n = sr_rpc_query_on(rpc, &err, &res, &id, "RETURN 1;", NULL);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, n,
        "a single-threaded runtime must still run queries");
    sr_arr_res_arr_free(res, n);

    if (err) { sr_string_free(err); err = NULL; }
    sr_surreal_rpc_disconnect(rpc);
}

/* An explicit worker count is honoured rather than ignored. */
TEST(Options, ExplicitWorkerCountConnects) {
    sr_option_t opts = {0};
    opts.worker_threads = 2;
    opts.thread_keep_alive_ms = 500;
    opts.max_blocking_threads = 8;

    sr_surreal_rpc_t *rpc = NULL;
    if (sr_surreal_rpc_new(&err, &rpc, "memory", opts) < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        TEST_FAIL_MESSAGE("an explicitly sized context should be created");
    }
    sr_surreal_rpc_disconnect(rpc);
}

/* A negative count is a caller error, not a silent clamp. */
TEST(Options, NegativeWorkerCountIsRejected) {
    sr_option_t opts = {0};
    opts.worker_threads = -1;

    sr_surreal_rpc_t *rpc = NULL;
    int rc = sr_surreal_rpc_new(&err, &rpc, "memory", opts);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(0, rc, "a negative worker count should fail");
    if (err) { sr_string_free(err); err = NULL; }
}

/*
 * sr_runtime_init is the process-global half. It is separate from sr_option_t
 * because the KVS pool is built once per process: a per-context field would be
 * honoured for the first context and silently ignored for every later one.
 */
TEST(Options, RuntimeInitValidatesItsInput) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_NONE, sr_runtime_init(&err, NULL),
        "a null options pointer is a no-op, not an error");

    sr_runtime_options_t ro = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_NONE, sr_runtime_init(&err, &ro),
        "an all-zero struct asks for nothing");

    ro.kvs_threadpool_size = -1;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_ERROR, sr_runtime_init(&err, &ro),
        "a negative pool size should be rejected");
    TEST_ASSERT_NOT_NULL(err);
    if (err) { sr_string_free(err); err = NULL; }

    ro.kvs_threadpool_size = 4;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sr_runtime_init(&err, &ro),
        "a valid pool size should be applied");
}

/* A caller-supplied temporary directory is accepted on the RPC path. */
TEST(Options, TemporaryDirectoryIsAccepted) {
    sr_option_t opts = {0};
    opts.temporary_directory = "surrealdb_tmp_test";

    sr_surreal_rpc_t *rpc = NULL;
    if (sr_surreal_rpc_new(&err, &rpc, "memory", opts) < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        TEST_FAIL_MESSAGE("a context with a temporary directory should be created");
    }
    sr_surreal_rpc_disconnect(rpc);
    rm_rf_sessions("surrealdb_tmp_test");
}

/* ------------------------------------------------- session persistence */

static void rm_rf_sessions(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    int rc = system(cmd);
    (void)rc;
}

TEST(Options, SessionsPersistAcrossContexts) {
    /* Upstream added session persistence in 3.2 for edge deployments, where an
       evicted isolate has to rehydrate a session on the next request. The same
       machinery lets a session outlive the RPC context that created it: attach
       here, tear the context down, reopen against the same directory, and the
       session is still resumable. */
    const char *dir = "sessions_test_dir";
    rm_rf_sessions(dir);

    sr_option_t opts = {0};
    opts.session_dir = dir;

    sr_surreal_rpc_t *rpc = NULL;
    if (sr_surreal_rpc_new(&err, &rpc, "memory", opts) < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        rm_rf_sessions(dir);
        TEST_FAIL_MESSAGE("rpc context with a session dir should be created");
    }

    sr_uuid_t id = {{0}};
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, sr_rpc_session_attach(rpc, &err, &id),
                                             "attach should succeed");

    /* Give the session something to persist, then tear the context down. */
    uint8_t req[64];
    int i = 0;
    req[i++] = 0xA2;
    req[i++] = 0x66; memcpy(req + i, "method", 6); i += 6;
    req[i++] = 0x63; memcpy(req + i, "use", 3); i += 3;
    req[i++] = 0x66; memcpy(req + i, "params", 6); i += 6;
    req[i++] = 0x82;
    req[i++] = 0x62; memcpy(req + i, "ns", 2); i += 2;
    req[i++] = 0x62; memcpy(req + i, "db", 2); i += 2;
    uint8_t *out = NULL;
    int n = sr_rpc_execute_on(rpc, &err, &out, &id, req, i);
    if (n > 0 && out) sr_byte_arr_free(out, n);
    if (err) { sr_string_free(err); err = NULL; }

    sr_surreal_rpc_disconnect(rpc);

    /* A file per session id should now exist. */
    char path[512];
    snprintf(path, sizeof(path), "%s", dir);
    FILE *probe = NULL;
    {
        char cmd[640];
        snprintf(cmd, sizeof(cmd), "ls '%s'/*.session.json >/dev/null 2>&1", path);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, system(cmd),
                                      "a session file should have been written");
    }
    (void)probe;

#ifndef _WIN32
    /*
     * A session file is the whole session serialised, which includes its
     * authentication token and record auth data. At a default umask
     * std::fs::write would leave it 0644 -- readable by every account on the
     * machine -- so the library sets 0600 on the file and 0700 on the
     * directory. Assert it, because nothing else would notice a regression.
     */
    {
        DIR *d = opendir(dir);
        TEST_ASSERT_NOT_NULL_MESSAGE(d, "the session directory should exist");

        int checked = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strstr(ent->d_name, ".session.json") == NULL) continue;

            char file[768];
            snprintf(file, sizeof(file), "%s/%s", dir, ent->d_name);

            struct stat st;
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, stat(file, &st), "stat should succeed");
            TEST_ASSERT_EQUAL_HEX_MESSAGE(0600, st.st_mode & 0777,
                "a persisted session holds credentials and must be owner-only");
            checked++;
        }
        closedir(d);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, checked,
            "at least one session file should have been inspected");

        struct stat ds;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, stat(dir, &ds), "stat on the dir should succeed");
        TEST_ASSERT_EQUAL_HEX_MESSAGE(0700, ds.st_mode & 0777,
            "the session directory should not be traversable by others");
    }
#endif

    /* Reopen against the same directory: the session rehydrates on demand. */
    sr_surreal_rpc_t *rpc2 = NULL;
    if (sr_surreal_rpc_new(&err, &rpc2, "memory", opts) < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        rm_rf_sessions(dir);
        TEST_FAIL_MESSAGE("second rpc context should be created");
    }

    uint8_t req2[64];
    int j = 0;
    req2[j++] = 0xA2;
    req2[j++] = 0x66; memcpy(req2 + j, "method", 6); j += 6;
    req2[j++] = 0x64; memcpy(req2 + j, "info", 4); j += 4;
    req2[j++] = 0x66; memcpy(req2 + j, "params", 6); j += 6;
    req2[j++] = 0x80;
    uint8_t *out2 = NULL;
    int n2 = sr_rpc_execute_on(rpc2, &err, &out2, &id, req2, j);

    /* `info` is stateful, so reaching it at all proves the session was
       rehydrated -- an unknown id is rejected by get_session(). */
    if (n2 < 0 && err) {
        TEST_ASSERT_NULL_MESSAGE(strstr(err, "not found"),
                                 "the persisted session should have been rehydrated");
        sr_string_free(err); err = NULL;
    }
    if (n2 > 0 && out2) sr_byte_arr_free(out2, n2);

    /* Detaching removes the durable copy too. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sr_rpc_session_detach(rpc2, &err, &id));
    {
        char cmd[640];
        snprintf(cmd, sizeof(cmd), "ls '%s'/*.session.json >/dev/null 2>&1", dir);
        TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(0, system(cmd),
                                          "detach should remove the durable copy");
    }

    sr_surreal_rpc_disconnect(rpc2);
    rm_rf_sessions(dir);
}

TEST(Options, PersistenceIsOffWithoutADirectory) {
    /* No directory means the durability branches stay inert -- nothing is
       written anywhere, and attach/detach behave exactly as before. */
    sr_option_t opts = {0};
    sr_surreal_rpc_t *rpc = NULL;
    if (sr_surreal_rpc_new(&err, &rpc, "memory", opts) < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        TEST_FAIL_MESSAGE("rpc context should be created");
    }
    sr_uuid_t id = {{0}};
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sr_rpc_session_attach(rpc, &err, &id));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sr_rpc_session_detach(rpc, &err, &id));
    sr_surreal_rpc_disconnect(rpc);
}

TEST_GROUP_RUNNER(Options) {
    RUN_TEST_CASE(Options, ZeroOptionsBehaveLikePlainConnect);
    RUN_TEST_CASE(Options, TimeoutsAreAccepted);
    RUN_TEST_CASE(Options, ScriptingToggleIsHonoured);
    RUN_TEST_CASE(Options, BadCapabilityNameIsReported);
    RUN_TEST_CASE(Options, DenyOverridesAllowForRpcMethods);
    RUN_TEST_CASE(Options, TargetModesAreAccepted);
    RUN_TEST_CASE(Options, DefaultRuntimeDoesNotScaleWithCores);
    RUN_TEST_CASE(Options, CurrentThreadRuntimeStillServesQueries);
    RUN_TEST_CASE(Options, ExplicitWorkerCountConnects);
    RUN_TEST_CASE(Options, NegativeWorkerCountIsRejected);
    RUN_TEST_CASE(Options, RuntimeInitValidatesItsInput);
    RUN_TEST_CASE(Options, TemporaryDirectoryIsAccepted);
    RUN_TEST_CASE(Options, SessionsPersistAcrossContexts);
    RUN_TEST_CASE(Options, PersistenceIsOffWithoutADirectory);
}
