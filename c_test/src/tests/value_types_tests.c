/*
 * Coverage for the value variants added for SurrealDB 3.x: table, file, range,
 * regex and set.
 *
 * Before these existed, surrealdb-types values of those kinds fell through a
 * catch-all in the conversion and reached C as SR_VALUE_NONE, so a caller could
 * not tell a file reference from an absent field. The round-trip tests below
 * exist to keep that from regressing silently.
 */

#include "unity_fixture.h"
#include "surrealdb.h"
#include <string.h>

TEST_GROUP(ValueTypes);

static sr_surreal_t *db;
static sr_string_t err;

TEST_SETUP(ValueTypes) {
    db = NULL;
    sr_connect(&err, &db, "mem://");
    if (db) {
        sr_use_ns(db, &err, "test_ns");
        sr_use_db(db, &err, "test_db");
    }
}

TEST_TEAR_DOWN(ValueTypes) {
    if (db != NULL) {
        sr_surreal_disconnect(db);
        db = NULL;
    }
}

/* Run a statement and return the single resulting value, or NULL. */
static sr_value_t *query_one(const char *statement) {
    sr_value_t *out = NULL;
    sr_arr_res_t *results = NULL;

    int len = sr_query(db, &err, &results, statement, NULL);
    if (len < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        return NULL;
    }
    if (len > 0 && results[0].ok.len > 0) {
        out = &results[0].ok.arr[0];
    }
    return out ? out : NULL;
}

/* ------------------------------------------------------------ constructors */

TEST(ValueTypes, TableConstructor) {
    sr_value_t *val = sr_value_table("users");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_VALUE_TABLE, val->tag, "tag should be TABLE");
    sr_value_free(val);
}

TEST(ValueTypes, FileConstructor) {
    sr_value_t *val = sr_value_file("avatars", "user-1.png");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_VALUE_FILE, val->tag, "tag should be FILE");
    TEST_ASSERT_EQUAL_STRING("avatars", val->sr_value_file.bucket);
    TEST_ASSERT_EQUAL_STRING("user-1.png", val->sr_value_file.key);
    sr_value_free(val);
}

TEST(ValueTypes, RegexConstructor) {
    sr_value_t *val = sr_value_regex("^a.*z$");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_VALUE_REGEX, val->tag, "tag should be REGEX");
    sr_value_free(val);
}

TEST(ValueTypes, SetConstructor) {
    sr_value_t *val = sr_value_set();
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_VALUE_SET, val->tag, "tag should be SET");
    sr_value_free(val);
}

TEST(ValueTypes, RangeConstructor) {
    sr_value_t *lo = sr_value_int(1);
    sr_value_t *hi = sr_value_int(10);

    sr_value_t *val = sr_value_range(sr_bound_included(lo), sr_bound_excluded(hi));
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_VALUE_RANGE, val->tag, "tag should be RANGE");
    TEST_ASSERT_EQUAL_INT(SR_BOUND_INCLUDED, val->sr_value_range->start.tag);
    TEST_ASSERT_EQUAL_INT(SR_BOUND_EXCLUDED, val->sr_value_range->end.tag);

    /* lo and hi were consumed by the bounds; freeing the range frees them. */
    sr_value_free(val);
}

TEST(ValueTypes, UnboundedRange) {
    sr_value_t *val = sr_value_range(sr_bound_unbounded(), sr_bound_unbounded());
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(SR_BOUND_UNBOUNDED, val->sr_value_range->start.tag);
    TEST_ASSERT_EQUAL_INT(SR_BOUND_UNBOUNDED, val->sr_value_range->end.tag);
    sr_value_free(val);
}

TEST(ValueTypes, NullConstructorArgsAreRejected) {
    /* A null argument must not crash; it degrades to NONE. */
    sr_value_t *a = sr_value_table(NULL);
    sr_value_t *b = sr_value_file(NULL, NULL);
    sr_value_t *c = sr_value_regex(NULL);
    TEST_ASSERT_EQUAL_INT(SR_VALUE_NONE, a->tag);
    TEST_ASSERT_EQUAL_INT(SR_VALUE_NONE, b->tag);
    TEST_ASSERT_EQUAL_INT(SR_VALUE_NONE, c->tag);
    sr_value_free(a);
    sr_value_free(b);
    sr_value_free(c);
}

/* ------------------------------------------------------------- round trips */

TEST(ValueTypes, RangeSurvivesTheDatabase) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_value_t *val = query_one("RETURN 1..10");
    if (val == NULL) {
        TEST_IGNORE_MESSAGE("server did not return a value for a range literal");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_VALUE_RANGE, val->tag,
                                  "a range must not arrive as SR_VALUE_NONE");
}

TEST(ValueTypes, SetSurvivesTheDatabase) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_value_t *val = query_one("RETURN <set>[1, 2, 2, 3]");
    if (val == NULL) {
        TEST_IGNORE_MESSAGE("server did not return a value for a set literal");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_VALUE_SET, val->tag,
                                  "a set must not arrive as SR_VALUE_NONE");
}

TEST(ValueTypes, RegexSurvivesTheDatabase) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_value_t *val = query_one("RETURN /^abc$/");
    if (val == NULL) {
        TEST_IGNORE_MESSAGE("server did not return a value for a regex literal");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(SR_VALUE_REGEX, val->tag,
                                  "a regex must not arrive as SR_VALUE_NONE");
}

TEST_GROUP_RUNNER(ValueTypes) {
    RUN_TEST_CASE(ValueTypes, TableConstructor);
    RUN_TEST_CASE(ValueTypes, FileConstructor);
    RUN_TEST_CASE(ValueTypes, RegexConstructor);
    RUN_TEST_CASE(ValueTypes, SetConstructor);
    RUN_TEST_CASE(ValueTypes, RangeConstructor);
    RUN_TEST_CASE(ValueTypes, UnboundedRange);
    RUN_TEST_CASE(ValueTypes, NullConstructorArgsAreRejected);
    RUN_TEST_CASE(ValueTypes, RangeSurvivesTheDatabase);
    RUN_TEST_CASE(ValueTypes, SetSurvivesTheDatabase);
    RUN_TEST_CASE(ValueTypes, RegexSurvivesTheDatabase);
}
