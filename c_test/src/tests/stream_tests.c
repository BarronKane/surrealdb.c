#include "unity_fixture.h"
#include "surrealdb.h"
#include "test_support.h"
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
        SR_AGAIN, got, "a notification should arrive within two seconds");

    /* Only a positive result carries a notification. SR_AGAIN (0) means nothing
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

    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_AGAIN, got,
        "an empty queue polled with no timeout should report SR_AGAIN");

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
        TEST_ASSERT_EQUAL_INT_MESSAGE(SR_AGAIN, empty,
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
 * A killed live query ends its stream.
 *
 * These two were tripwires before the anchor carried the fix: the terminal Killed
 * notification carried no session id, so the SDK's local router dropped it and
 * the stream stayed open and silent forever -- indistinguishable from an idle
 * subscription. They asserted that broken behaviour and were written to fail the
 * day it was fixed, which is how they were found when the anchor picked it up
 * (surrealdb/surrealdb#7520).
 *
 * They now assert the real contract. Both drive sr_stream_next_timeout rather
 * than the blocking read: if the stream ever stops ending again, a bounded wait
 * fails in under a second where sr_stream_next would hang the suite.
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

TEST(Stream, KillEndsTheStream) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    int after = stream_after("killed_stream", "KILL u'%s'", 700);
    if (after == SR_ERROR) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }

    /* Not SR_AGAIN: the stream reports its end rather than merely going quiet,
       which is the difference between "nothing yet" and "stop asking". */
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_CLOSED, after,
        "a killed live query should end its stream with SR_CLOSED");
}

/*
 * The half nobody asks for: a schema change on an unrelated code path. Dropping
 * a table has to end the streams subscribed to it, since they can never produce
 * another row.
 */
TEST(Stream, RemoveTableEndsTheStream) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    int after = stream_after("removed_stream", "REMOVE TABLE removed_stream%.0s", 700);
    if (after == SR_ERROR) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_CLOSED, after,
        "REMOVE TABLE should end the streams subscribed to that table");
}

/* Subscriptions registered on `table`, via INFO FOR TABLE's `lives` map. */
static int live_count(const char *table) {
    char sql[96];
    snprintf(sql, sizeof(sql), "INFO FOR TABLE %s;", table);

    sr_arr_res_t *res = NULL;
    int n = sr_query(db, &err, &res, sql, NULL);
    if (n <= 0) { if (err) { sr_string_free(err); err = NULL; } return -1; }

    int count = -1;
    if (sr_array_len(&res[0].ok) > 0) {
        const sr_value_t *info = sr_array_get(&res[0].ok, 0);
        if (info && info->tag == SR_VALUE_OBJECT) {
            const sr_value_t *lives = sr_object_get(&info->sr_value_object, "lives");
            if (lives && lives->tag == SR_VALUE_OBJECT) {
                count = sr_object_len(&lives->sr_value_object);
            }
        }
    }
    sr_arr_res_arr_free(res, n);
    return count;
}

/*
 * Either teardown route retires the subscription.
 *
 * This was the opposite until the anchor picked up "Retire live queries on
 * every teardown path". `Stream::drop` built `KILL {id}` from a bare UUID,
 * which the parser rejects, and ran it under a blank session with no namespace
 * -- so the reader was freed and the subscription was left registered, on every
 * platform, for every live query. Both failures were silent: the kill is
 * spawned detached from `Drop` and its result discarded.
 *
 * `INFO FOR TABLE` lists a table's `lives`, which is the only view of the
 * subscription a C caller has.
 */
TEST(Stream, EitherTeardownRouteRetiresTheQuery) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    /* Route 1: hold the stream, free the stream. */
    sr_stream_t *stream = live_on("teardown");
    if (stream == NULL) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, live_count("teardown"),
        "the subscription should be registered while the stream is open");

    sr_stream_kill(stream);
    test_sleep_ms(500);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, live_count("teardown"),
        "sr_stream_kill should retire the subscription, not just free the reader");

    /* Route 2: hold an id, kill by id. The id is only reachable from a
       notification -- sr_select_live does not return it, and the SDK keeps its
       own copy private. */
    stream = live_on("teardown2");
    if (stream == NULL) {
        TEST_IGNORE_MESSAGE("live queries unavailable on this build");
    }

    sr_arr_res_t *res = NULL;
    int n = sr_query(db, &err, &res, "CREATE teardown2:1 SET v = 1", NULL);
    if (n > 0) sr_arr_res_arr_free(res, n);
    if (n < 0 && err) { sr_string_free(err); err = NULL; }

    sr_notification_t note;
    int got = sr_stream_next_timeout(stream, &note, 2000);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, got, "the live query should notify");

    const uint8_t *u = note.query_id._0;
    char qid[37];
    snprintf(qid, sizeof(qid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    sr_notification_free(note);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sr_kill(db, &err, qid));
    test_sleep_ms(500);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, live_count("teardown2"),
        "sr_kill should retire the subscription");

    /* And the reader is told, rather than being left to guess. */
    int after = sr_stream_next_timeout(stream, &note, 2000);
    if (after > 0) sr_notification_free(note);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_CLOSED, after,
        "a stream whose query was killed by id should report SR_CLOSED");

    sr_stream_kill(stream);
}

TEST_GROUP_RUNNER(Stream) {
    RUN_TEST_CASE(Stream, Next);
    RUN_TEST_CASE(Stream, NextTimeoutZeroDoesNotBlock);
    RUN_TEST_CASE(Stream, ExpiredWaitDoesNotDropNotifications);
    RUN_TEST_CASE(Stream, KillEndsTheStream);
    RUN_TEST_CASE(Stream, RemoveTableEndsTheStream);
    RUN_TEST_CASE(Stream, EitherTeardownRouteRetiresTheQuery);
    RUN_TEST_CASE(Stream, Kill);
}
