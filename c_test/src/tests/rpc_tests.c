#include "unity_fixture.h"
#include "surrealdb.h"
#include "test_support.h"
#include <stdio.h>
#include <string.h>

TEST_GROUP(RPC);

TEST_SETUP(RPC) {
}

TEST_TEAR_DOWN(RPC) {
}

TEST(RPC, New) {
    sr_surreal_rpc_t *rpc;
    sr_string_t err;
    sr_option_t opts = {0};
    
    int result = sr_surreal_rpc_new(&err, &rpc, "memory", opts);
    if (result < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "rpc_new should succeed: %s", err);
        sr_string_free(err);
        TEST_FAIL_MESSAGE(msg);
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, result, "rpc_new should succeed");
    TEST_ASSERT_NOT_NULL_MESSAGE(rpc, "RPC handle should not be NULL");
    
    sr_surreal_rpc_disconnect(rpc);
}

TEST(RPC, Execute) {
    sr_surreal_rpc_t *rpc;
    sr_string_t err;
    sr_option_t opts = {0};
    
    int result = sr_surreal_rpc_new(&err, &rpc, "memory", opts);
    if (result < 0) {
        if (err) sr_string_free(err);
        TEST_FAIL_MESSAGE("Failed to create RPC connection");
    }
    
    // RPC execute requires CBOR-encoded request bytes
    // For now, test with empty/invalid input to verify it handles errors gracefully
    uint8_t *response = NULL;
    uint8_t invalid_cbor[] = {0x00};  // Invalid CBOR
    
    result = sr_surreal_rpc_execute(rpc, &err, &response, invalid_cbor, 1);
    // This should fail since the CBOR is invalid, but shouldn't crash
    if (result < 0) {
        if (err) sr_string_free(err);
    }
    if (response) {
        sr_byte_arr_free(response, result);
    }
    
    sr_surreal_rpc_disconnect(rpc);
    // Test passes if we get here without crashing
}

TEST(RPC, Notifications) {
    sr_surreal_rpc_t *rpc;
    sr_string_t err = NULL;
    sr_option_t opts = {0};
    
    int result = sr_surreal_rpc_new(&err, &rpc, "memory", opts);
    if (result < 0) {
        if (err) sr_string_free(err);
        TEST_FAIL_MESSAGE("Failed to create RPC connection");
    }
    
    sr_RpcStream *stream = NULL;
    result = sr_surreal_rpc_notifications(rpc, &err, &stream);
    if (result < 0) {
        if (err) sr_string_free(err);
        sr_surreal_rpc_disconnect(rpc);
        TEST_FAIL_MESSAGE("Failed to get notifications stream");
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(stream, "Notifications stream should not be NULL");
    
    sr_rpc_stream_free(stream);
    sr_surreal_rpc_disconnect(rpc);
}

/* Context for the blocked reader below. */
typedef struct {
    sr_RpcStream *stream;
    int rc;
} rpc_reader_ctx;

static void rpc_reader(void *arg) {
    rpc_reader_ctx *ctx = (rpc_reader_ctx *)arg;
    uint8_t *payload = NULL;

    /* Blocks: nothing is pending, and there is no timeout variant. */
    ctx->rc = sr_rpc_stream_next(ctx->stream, &payload);
    if (ctx->rc > 0 && payload != NULL) {
        sr_byte_arr_free(payload, ctx->rc);
    }
}

TEST(RPC, StreamNextUnblocksOnShutdown) {
    sr_surreal_rpc_t *rpc;
    sr_string_t err = NULL;
    sr_option_t opts = {0};

    if (sr_surreal_rpc_new(&err, &rpc, "memory", opts) < 0) {
        if (err) sr_string_free(err);
        TEST_FAIL_MESSAGE("Failed to create RPC connection");
    }

    sr_RpcStream *stream = NULL;
    if (sr_surreal_rpc_notifications(rpc, &err, &stream) < 0) {
        if (err) sr_string_free(err);
        sr_surreal_rpc_disconnect(rpc);
        TEST_FAIL_MESSAGE("Failed to get notifications stream");
    }

    /*
     * The contract under test is how a consumer shuts down: a worker parked in
     * sr_rpc_stream_next is released by freeing the connection, which drops the
     * sending half of the notification channel. Without that there would be no
     * way to retire the thread short of ending the process.
     */
    rpc_reader_ctx ctx = {stream, 0};
    test_thread *reader = test_thread_start(rpc_reader, &ctx);
    TEST_ASSERT_NOT_NULL_MESSAGE(reader, "could not start the reader thread");

    test_sleep_ms(300);
    sr_surreal_rpc_disconnect(rpc);
    test_thread_join(reader);

    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_CLOSED, ctx.rc,
                                  "closing the connection must release the reader");

    /* The stream outlives its connection and is still ours to free. */
    sr_rpc_stream_free(stream);
}

/* A hand-rolled CBOR map: {"method": <name>, "params": [<arg>]}. Encoding this
   by hand keeps the test suite free of a CBOR dependency; the request shape is
   small enough that doing so stays readable. */
static int rpc_request_1(uint8_t *buf, int cap, const char *method, const char *arg) {
    const int mlen = (int)strlen(method), alen = (int)strlen(arg);
    if (mlen > 23 || alen > 255 || cap < 32 + mlen + alen) return -1;
    int i = 0;
    buf[i++] = 0xA2;                                   /* map(2) */
    buf[i++] = 0x66; memcpy(buf + i, "method", 6); i += 6;
    buf[i++] = (uint8_t)(0x60 | mlen); memcpy(buf + i, method, (size_t)mlen); i += mlen;
    buf[i++] = 0x66; memcpy(buf + i, "params", 6); i += 6;
    buf[i++] = 0x81;                                   /* array(1) */
    if (alen < 24) { buf[i++] = (uint8_t)(0x60 | alen); }
    else { buf[i++] = 0x78; buf[i++] = (uint8_t)alen; }
    memcpy(buf + i, arg, (size_t)alen); i += alen;
    return i;
}

/*
 * `query` is the method the whole RPC surface exists for, and it was
 * unreachable: run_rpc matched only DbResult::Other, so `query` and `gql` --
 * the two methods that return DbResult::Query -- came back as
 * "RPC::execute had unimplemented response". Nothing caught it because the
 * other tests only drive methods that return Other (ping, info, version, use).
 */
TEST(RPC, QueryIsReachable) {
    sr_surreal_rpc_t *rpc;
    sr_string_t err = NULL;
    sr_option_t opts = {0};

    if (sr_surreal_rpc_new(&err, &rpc, "memory", opts) < 0) {
        if (err) sr_string_free(err);
        TEST_FAIL_MESSAGE("Failed to create RPC connection");
    }

    uint8_t req[128];
    int n = rpc_request_1(req, (int)sizeof(req), "query", "RETURN 1;");
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    uint8_t *res = NULL;
    int rc = sr_surreal_rpc_execute(rpc, &err, &res, req, n);

    if (rc < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "query over RPC should succeed: %s",
                 err ? (const char *)err : "(no error)");
        if (err) { sr_string_free(err); err = NULL; }
        sr_surreal_rpc_disconnect(rpc);
        TEST_FAIL_MESSAGE(msg);
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, rc, "a query response should carry bytes");
    TEST_ASSERT_NOT_NULL_MESSAGE(res, "a query response should be written");

    sr_byte_arr_free(res, rc);
    if (err) sr_string_free(err);
    sr_surreal_rpc_disconnect(rpc);
}

TEST(RPC, Free) {
    sr_surreal_rpc_t *rpc;
    sr_string_t err;
    sr_option_t opts = {0};
    
    sr_surreal_rpc_new(&err, &rpc, "memory", opts);
    sr_surreal_rpc_disconnect(rpc);
    // If we get here without crashing, test passes
}

TEST_GROUP_RUNNER(RPC) {
    RUN_TEST_CASE(RPC, New);
    RUN_TEST_CASE(RPC, Execute);
    RUN_TEST_CASE(RPC, Notifications);
    RUN_TEST_CASE(RPC, StreamNextUnblocksOnShutdown);
    RUN_TEST_CASE(RPC, QueryIsReachable);
    RUN_TEST_CASE(RPC, Free);
}
