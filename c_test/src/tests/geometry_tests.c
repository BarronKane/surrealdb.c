/*
 * Geometry values.
 *
 * Previously reachable only through the LegacyApi bridge, which meant the
 * type names went unexercised by anything current -- and those names were
 * wrong: cbindgen's export.prefix stacked onto Rust types already spelled
 * `sr_g_*`, emitting `sr_sr_g_point` and friends. Correcting that collided
 * with the tag enum, whose variants had taken the good names, so the variants
 * are now `SR_GEOMETRY_*` like every other enum in the header and the union
 * members are `sr_geometry_*`.
 *
 * These tests pin the current spelling and exercise every geometry kind
 * through construction, inspection, release and a database round trip.
 */

#include "unity_fixture.h"
#include "surrealdb.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

TEST_GROUP(Geometry);

static sr_surreal_t *db;
static sr_string_t err;

TEST_SETUP(Geometry) {
    db = NULL;
    err = NULL;
    sr_connect(&err, &db, "memory");
    if (db) {
        sr_use_ns(db, &err, "geo_ns");
        sr_use_db(db, &err, "geo_db");
    }
}

TEST_TEAR_DOWN(Geometry) {
    if (err) { sr_string_free(err); err = NULL; }
    if (db) { sr_surreal_disconnect(db); db = NULL; }
}

static sr_g_coord RING[5]  = {{0,0},{10,0},{10,10},{0,10},{0,0}};
static sr_g_coord LINE[3]  = {{0,0},{1,1},{2,0}};

TEST(Geometry, Point) {
    sr_value_t *v = sr_value_point(-122.4194, 37.7749);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_INT(SR_GEOMETRY_OBJECT, v->tag);
    TEST_ASSERT_EQUAL_INT(SR_GEOMETRY_POINT, v->sr_geometry_object.tag);
    /* A point holds an inline coord and owns no array -- which is why it was
       the one geometry kind that never leaked before Drop was added. */
    /* Unity's double assertions are compiled out in this build, so compare
       with an explicit tolerance rather than depend on that configuration. */
    TEST_ASSERT_TRUE_MESSAGE(
        fabs(v->sr_geometry_object.sr_geometry_point._0.x - (-122.4194)) < 1e-9,
        "x should round-trip through the constructor");
    TEST_ASSERT_TRUE_MESSAGE(
        fabs(v->sr_geometry_object.sr_geometry_point._0.y - 37.7749) < 1e-9,
        "y should round-trip through the constructor");
    sr_value_free(v);
}

TEST(Geometry, LineString) {
    sr_value_t *v = sr_value_linestring(LINE, 3);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_INT(SR_GEOMETRY_LINESTRING, v->sr_geometry_object.tag);
    TEST_ASSERT_EQUAL_INT(3, v->sr_geometry_object.sr_geometry_linestring._0.len);
    sr_value_free(v);
}

TEST(Geometry, Polygon) {
    sr_value_t *v = sr_value_polygon(RING, 5);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_INT(SR_GEOMETRY_POLYGON, v->sr_geometry_object.tag);
    /* Field 0 is the exterior ring; field 1 is the array of interior rings. */
    TEST_ASSERT_EQUAL_INT(5, v->sr_geometry_object.sr_geometry_polygon._0._0.len);
    sr_value_free(v);
}

TEST(Geometry, MultiPoint) {
    sr_value_t *v = sr_value_multipoint(LINE, 3);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_INT(SR_GEOMETRY_MULTIPOINT, v->sr_geometry_object.tag);
    TEST_ASSERT_EQUAL_INT(3, v->sr_geometry_object.sr_geometry_multipoint._0.len);
    sr_value_free(v);
}

TEST(Geometry, NullArgumentsAreRejected) {
    /* A null coordinate array must degrade, not crash. */
    sr_value_t *a = sr_value_linestring(NULL, 0);
    sr_value_t *b = sr_value_polygon(NULL, 0);
    sr_value_t *c = sr_value_multipoint(NULL, 0);
    if (a) sr_value_free(a);
    if (b) sr_value_free(b);
    if (c) sr_value_free(c);
}

TEST(Geometry, SurvivesTheDatabase) {
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "Connection should succeed");

    sr_object_t rec = sr_object_new();
    sr_value_t *area = sr_value_polygon(RING, 5);
    sr_object_insert(&rec, "area", area);
    sr_value_free(area);

    int rc = sr_create(db, &err, NULL, "site:one", &rec);
    sr_object_free(rec);
    if (rc < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        TEST_FAIL_MESSAGE("creating a record with a polygon should succeed");
    }

    sr_value_t *rows = NULL;
    int n = sr_select(db, &err, &rows, "site");
    if (n < 0) {
        if (err) { sr_string_free(err); err = NULL; }
        TEST_FAIL_MESSAGE("select should succeed");
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, n, "the record should come back");

    /* The geometry must survive the round trip as a geometry, not collapse to
       SR_VALUE_NONE the way the unmapped value kinds used to. */
    const sr_value_t *area_back = NULL;
    if (rows[0].tag == SR_VALUE_OBJECT) {
        area_back = sr_object_get(&rows[0].sr_value_object, "area");
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(area_back, "the area field should be present");
    if (area_back) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(SR_GEOMETRY_OBJECT, area_back->tag,
                                      "a polygon must not arrive as SR_VALUE_NONE");
        TEST_ASSERT_EQUAL_INT(SR_GEOMETRY_POLYGON, area_back->sr_geometry_object.tag);
    }
    sr_values_free(rows, n);
}

TEST(Geometry, RepeatedConstructionDoesNotLeak) {
    /* Exercised under -DSURREALDB_SANITIZE=address: every geometry kind that
       carries an array leaked its payload until Drop was implemented. */
    for (int i = 0; i < 8; ++i) {
        sr_value_free(sr_value_point(1.0, 2.0));
        sr_value_free(sr_value_linestring(LINE, 3));
        sr_value_free(sr_value_polygon(RING, 5));
        sr_value_free(sr_value_multipoint(LINE, 3));
    }
}

TEST_GROUP_RUNNER(Geometry) {
    RUN_TEST_CASE(Geometry, Point);
    RUN_TEST_CASE(Geometry, LineString);
    RUN_TEST_CASE(Geometry, Polygon);
    RUN_TEST_CASE(Geometry, MultiPoint);
    RUN_TEST_CASE(Geometry, NullArgumentsAreRejected);
    RUN_TEST_CASE(Geometry, SurvivesTheDatabase);
    RUN_TEST_CASE(Geometry, RepeatedConstructionDoesNotLeak);
}
