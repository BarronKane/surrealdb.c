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
     * Bounded rather than blocking. The CREATE above should already have queued
     * a notification, but asserting that with sr_stream_next means a build that
     * fails to deliver it hangs the suite instead of failing it -- which is
     * exactly the wrong behaviour to hand to CI.
     */
    sr_notification_t notification;
    int got = sr_stream_next_timeout(stream, &notification, 2000);

    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(
        SR_NONE, got, "a notification should arrive within two seconds");

    /* Only a positive result carries a notification. SR_NONE (0) means nothing
       arrived in time and SR_CLOSED means the stream ended without delivering the
       CREATE above; both are failures here, as is any other negative. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, got, "a notification should be received");

    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_ACTION_CREATE, notification.action,
                                  "a CREATE should be reported as such");
    /* The notification owns its data. sr_value_free must not be used on
       notification.data: that reclaims a Box, and this value lives in our
       own storage. */
    sr_notification_free(notification);

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

/*
 * The non-blocking form: zero waits for nothing and must return immediately
 * rather than parking. A live query with no traffic is the only reliable way
 * to be sure the queue is empty.
 */
TEST(Stream, NextTimeoutZeroDoesNotBlock) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_stream_t *stream = live_on("quiet_stream");
    if (stream == NULL) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }

    sr_notification_t notification;
    int got = sr_stream_next_timeout(stream, &notification, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_NONE, got,
        "an empty queue polled with no timeout should report SR_NONE");

    sr_stream_kill(stream);
}

/*
 * A timeout must not consume anything. Poll an empty stream until it expires,
 * then produce an event and confirm it is still delivered -- if an expired wait
 * took the notification off the channel, a polling reader would silently lose
 * events, which would be worse than the hang it replaces.
 */
TEST(Stream, ExpiredWaitDoesNotDropNotifications) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_stream_t *stream = live_on("patient_stream");
    if (stream == NULL) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }

    sr_notification_t notification;
    for (int i = 0; i < 3; i++) {
        int empty = sr_stream_next_timeout(stream, &notification, 10);
        TEST_ASSERT_EQUAL_INT_MESSAGE(SR_NONE, empty,
            "nothing has been written, so every poll should expire");
    }

    sr_arr_res_t *res = NULL;
    int n = sr_query(db, &err, &res, "CREATE patient_stream:1 SET v = 1", NULL);
    if (n > 0) sr_arr_res_arr_free(res, n);
    if (n < 0 && err) { sr_string_free(err); err = NULL; }

    int got = sr_stream_next_timeout(stream, &notification, 2000);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, got,
        "a notification after several expired waits should still arrive");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_ACTION_CREATE, notification.action,
        "and should be the CREATE that produced it");
    sr_notification_free(notification);

    sr_stream_kill(stream);
}

/*
 * ===========================================================================
 * TRIPWIRES -- these assert the BROKEN behaviour on surrealdb 3.2.4
 * ===========================================================================
 *
 * A killed live query does not end its stream on the embedded path: core emits
 * the terminal Killed notification with no session id, and the SDK's local
 * router drops any notification that names no session. See the module note in
 * src/types/stream.rs.
 *
 * surrealdb/surrealdb#7520 fixes it. WHEN THAT SHIPS AND WE BUMP THE
 * DEPENDENCY, THE TWO TESTS BELOW WILL START FAILING. That is the intended
 * signal, not a regression: invert them to assert SR_CLOSED, drop the TODO,
 * and make sr_stream_next the recommended read again in the README and in
 * sr_stream_next's own doc.
 *
 * They are written against sr_stream_next_timeout on purpose. The behaviour
 * under test is "the stream does not end", and asserting that with the
 * unbounded sr_stream_next would hang the suite forever rather than fail it --
 * the same defect Drew flagged in this file, one layer up.
 */

/* Shared body: open a live query on `table`, prove it is live, run `stmt`,
   then report what the stream does next within `budget_ms`. */
static int stream_after(const char *table, const char *stmt, int budget_ms) {
    sr_stream_t *stream = live_on(table);
    if (stream == NULL) return SR_ERROR;

    char create[128];
    snprintf(create, sizeof(create), "CREATE %s:1 SET v = 1", table);

    sr_arr_res_t *res = NULL;
    int n = sr_query(db, &err, &res, create, NULL);
    if (n > 0) sr_arr_res_arr_free(res, n);
    if (n < 0 && err) { sr_string_free(err); err = NULL; }

    sr_notification_t note;
    int got = sr_stream_next_timeout(stream, &note, 2000);
    if (got <= 0) { sr_stream_kill(stream); return SR_ERROR; }

    /* The live query id is only reachable from a notification. */
    const uint8_t *u = note.query_id._0;
    char qid[37];
    snprintf(qid, sizeof(qid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    sr_notification_free(note);

    char sql[256];
    snprintf(sql, sizeof(sql), stmt, qid);
    res = NULL;
    n = sr_query(db, &err, &res, sql, NULL);
    if (n > 0) sr_arr_res_arr_free(res, n);
    if (n < 0 && err) { sr_string_free(err); err = NULL; }

    /* Write again. If the subscription really is gone this produces nothing,
       which is what makes the assertion below mean something: without it a
       silent stream could just as well be a KILL that never ran. */
    snprintf(create, sizeof(create), "CREATE %s:2 SET v = 2", table);
    res = NULL;
    n = sr_query(db, &err, &res, create, NULL);
    if (n > 0) sr_arr_res_arr_free(res, n);
    if (n < 0 && err) { sr_string_free(err); err = NULL; }

    int after = sr_stream_next_timeout(stream, &note, budget_ms);
    if (after > 0) sr_notification_free(note);
    sr_stream_kill(stream);
    return after;
}

/* TODO(upstream #7520): flip to TEST_ASSERT_EQUAL_INT(SR_CLOSED, ...). */
TEST(Stream, KillDoesNotYetEndTheStream) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    int after = stream_after("killed_stream", "KILL u'%s'", 700);
    if (after == SR_ERROR) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }

    /* SR_NONE, not a notification: the kill took effect, so the later write
       was not delivered. And not SR_CLOSED: the stream will not admit it. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_NONE, after,
        "TRIPWIRE: KILL now ends the stream -- #7520 has landed, invert this test");
}

/*
 * The nastier half: nobody asked for this. A schema change on an unrelated code
 * path strands every stream subscribed to the table.
 *
 * TODO(upstream #7520): flip to TEST_ASSERT_EQUAL_INT(SR_CLOSED, ...).
 */
TEST(Stream, RemoveTableDoesNotYetEndTheStream) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    int after = stream_after("removed_stream", "REMOVE TABLE removed_stream%.0s", 700);
    if (after == SR_ERROR) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_NONE, after,
        "TRIPWIRE: REMOVE TABLE now ends the stream -- #7520 has landed, invert this test");
}

TEST_GROUP_RUNNER(Stream) {
    RUN_TEST_CASE(Stream, Next);
    RUN_TEST_CASE(Stream, NextTimeoutZeroDoesNotBlock);
    RUN_TEST_CASE(Stream, ExpiredWaitDoesNotDropNotifications);
    RUN_TEST_CASE(Stream, KillDoesNotYetEndTheStream);
    RUN_TEST_CASE(Stream, RemoveTableDoesNotYetEndTheStream);
    RUN_TEST_CASE(Stream, Kill);
}
