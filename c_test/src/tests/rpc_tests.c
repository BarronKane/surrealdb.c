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
    RUN_TEST_CASE(RPC, Free);
}
