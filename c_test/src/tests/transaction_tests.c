#include "unity_fixture.h"
#include "surrealdb.h"
#include <stdio.h>
#include <string.h>

TEST_GROUP(Transaction);

static sr_surreal_t *db;
static sr_string_t err;

TEST_SETUP(Transaction) {
    db = NULL;
    sr_connect(&err, &db, "memory");
    if (db) {
        sr_use_ns(db, &err, "test_ns");
        sr_use_db(db, &err, "test_db");
        sr_arr_res_t *r = NULL;
        int n = sr_query(db, &err, &r, "DEFINE TABLE acct SCHEMALESS;", NULL);
        if (n > 0) sr_arr_res_arr_free(r, n);
        if (err) { sr_string_free(err); err = NULL; }
    }
}

TEST_TEAR_DOWN(Transaction) {
    if (err) { sr_string_free(err); err = NULL; }
    if (db != NULL) {
        sr_surreal_disconnect(db);
        db = NULL;
    }
}

/* Rows in `acct`, read outside any transaction. */
static int rows(void) {
    sr_arr_res_t *r = NULL;
    int n = sr_query(db, &err, &r, "SELECT * FROM acct;", NULL);
    int c = -1;
    if (n > 0) c = sr_array_len(&r[0].ok);
    if (n > 0) sr_arr_res_arr_free(r, n);
    if (err) { sr_string_free(err); err = NULL; }
    return c;
}

/*
 * Cancelling rolls back.
 *
 * Asserted against the database rather than against the return code: a
 * transaction API that reports success while scoping nothing is the failure
 * worth guarding, and only the row count can tell the two apart.
 */
TEST(Transaction, CancelRollsBack) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");
    TEST_ASSERT_EQUAL_INT(0, rows());

    sr_transaction_t *tx = NULL;
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, sr_begin(db, &err, &tx),
        "begin should open a transaction");
    TEST_ASSERT_NOT_NULL(tx);

    sr_arr_res_t *res = NULL;
    int n = sr_tx_query(tx, &err, &res, "CREATE acct:1 SET bal = 10;", NULL);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, n, "a write inside the transaction should run");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, res[0].err.code, "and should not error");
    sr_arr_res_arr_free(res, n);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, sr_cancel(tx, &err), "cancel should succeed");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rows(),
        "a cancelled transaction must leave nothing behind");
}

/* And committing really persists. */
TEST(Transaction, CommitPersists) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_transaction_t *tx = NULL;
    TEST_ASSERT_GREATER_THAN_INT(0, sr_begin(db, &err, &tx));

    sr_arr_res_t *res = NULL;
    int n = sr_tx_query(tx, &err, &res, "CREATE acct:2 SET bal = 20;", NULL);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    sr_arr_res_arr_free(res, n);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, sr_commit(tx, &err), "commit should succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, rows(), "a committed write must survive");
}

/*
 * The whole point of a handle over `BEGIN; ...; COMMIT;` as one query: the
 * caller gets to run its own code between statements and still scope them
 * together. Two writes, a decision in between, one atomic outcome.
 */
TEST(Transaction, SpansSeparateCallsAtomically) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_transaction_t *tx = NULL;
    TEST_ASSERT_GREATER_THAN_INT(0, sr_begin(db, &err, &tx));

    sr_arr_res_t *res = NULL;
    int n = sr_tx_query(tx, &err, &res, "CREATE acct:a SET bal = 100;", NULL);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    sr_arr_res_arr_free(res, n);

    /* Neither write is visible outside while the transaction is open. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rows(),
        "an uncommitted write must not be visible outside the transaction");

    res = NULL;
    n = sr_tx_query(tx, &err, &res, "CREATE acct:b SET bal = 200;", NULL);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    sr_arr_res_arr_free(res, n);

    TEST_ASSERT_GREATER_THAN_INT(0, sr_commit(tx, &err));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, rows(),
        "both writes should land together on commit");
}

/* A consumed handle is gone; using it again is an error, not a crash. */
TEST(Transaction, UseAfterCompletionIsRejected) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_transaction_t *tx = NULL;
    TEST_ASSERT_GREATER_THAN_INT(0, sr_begin(db, &err, &tx));
    TEST_ASSERT_GREATER_THAN_INT(0, sr_cancel(tx, &err));

    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_ERROR, sr_commit(NULL, &err),
        "a null transaction should be rejected");
    if (err) { sr_string_free(err); err = NULL; }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_ERROR, sr_begin(db, &err, NULL),
        "a null out-pointer should be rejected");
    if (err) { sr_string_free(err); err = NULL; }
}

/* A failing statement reports in its own slot and leaves the choice to the
   caller, rather than tearing the transaction down. */
TEST(Transaction, FailedStatementLeavesTheChoiceToTheCaller) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_transaction_t *tx = NULL;
    TEST_ASSERT_GREATER_THAN_INT(0, sr_begin(db, &err, &tx));

    sr_arr_res_t *res = NULL;
    int n = sr_tx_query(tx, &err, &res, "CREATE acct:ok SET bal = 1;", NULL);
    if (n > 0) sr_arr_res_arr_free(res, n);

    /* Something the parser or executor will refuse. */
    res = NULL;
    n = sr_tx_query(tx, &err, &res, "SELECT * FROM $$$nope;", NULL);
    if (n > 0) sr_arr_res_arr_free(res, n);
    if (err) { sr_string_free(err); err = NULL; }

    /* The caller decides: discard the lot. */
    TEST_ASSERT_GREATER_THAN_INT(0, sr_cancel(tx, &err));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rows(),
        "cancelling after a failed statement should discard the good write too");
}

TEST_GROUP_RUNNER(Transaction) {
    RUN_TEST_CASE(Transaction, CancelRollsBack);
    RUN_TEST_CASE(Transaction, CommitPersists);
    RUN_TEST_CASE(Transaction, SpansSeparateCallsAtomically);
    RUN_TEST_CASE(Transaction, UseAfterCompletionIsRejected);
    RUN_TEST_CASE(Transaction, FailedStatementLeavesTheChoiceToTheCaller);
}
