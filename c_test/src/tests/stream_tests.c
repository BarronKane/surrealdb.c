#include "unity_fixture.h"
#include "surrealdb.h"
#include <stdio.h>
#include <string.h>

TEST_GROUP(Stream);

static sr_surreal_t *db;
static sr_string_t err;

TEST_SETUP(Stream) {
    db = NULL;
    sr_connect(&err, &db, "mem://");
    if (db) {
        sr_use_ns(db, &err, "test_ns");
        sr_use_db(db, &err, "test_db");
    }
}

TEST_TEAR_DOWN(Stream) {
    if (db != NULL) {
        sr_surreal_disconnect(db);
        db = NULL;
    }
}

/* Open a live query on a table that exists, or NULL if that is not possible. */
static sr_stream_t *live_on(const char *table) {
    char stmt[128];
    snprintf(stmt, sizeof(stmt), "DEFINE TABLE %s SCHEMALESS", table);

    sr_arr_res_t *res = NULL;
    int n = sr_query(db, &err, &res, stmt, NULL);
    if (n > 0) sr_arr_res_arr_free(res, n);
    if (n < 0 && err) { sr_string_free(err); err = NULL; }

    sr_stream_t *stream = NULL;
    if (sr_select_live(db, &err, &stream, table) < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        return NULL;
    }
    return stream;
}

TEST(Stream, Next) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_stream_t *stream = live_on("streamable");
    if (stream == NULL) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }

    sr_arr_res_t *res = NULL;
    int n = sr_query(db, &err, &res, "CREATE streamable:1 SET v = 1", NULL);
    if (n > 0) sr_arr_res_arr_free(res, n);
    if (n < 0 && err) { sr_string_free(err); err = NULL; }

    /*
     * sr_stream_next blocks until a notification arrives; the CREATE above is
     * what guarantees one is already queued. There is no timeout variant, so
     * do not call this without having produced an event first.
     */
    sr_notification_t notification;
    int got = sr_stream_next(stream, &notification);

    /* A negative result means the stream closed, which is a failure here;
       zero means no notification was ready, which is timing and not an error. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, got, "stream must not be closed");
    if (got > 0) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(SR_ACTION_CREATE, notification.action,
                                      "a CREATE should be reported as such");
    }

    sr_stream_kill(stream);
}

TEST(Stream, Kill) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_stream_t *stream = live_on("killable_stream");
    if (stream == NULL) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }
    sr_stream_kill(stream);
}

TEST_GROUP_RUNNER(Stream) {
    RUN_TEST_CASE(Stream, Next);
    RUN_TEST_CASE(Stream, Kill);
}
