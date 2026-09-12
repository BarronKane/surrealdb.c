/*
 * C driver for the test corpus.
 *
 * The group list lives in runner.c so that the Rust driver in c-tests/ runs the
 * same corpus without duplicating it. Accepts the usual Unity Fixture options,
 * so `test_runner -g CRUD` runs a single group — which is how CTest registers
 * one test per group.
 */

#include <stdio.h>

#include "unity_fixture.h"

void sr_run_all_test_groups(void);

int main(int argc, const char *argv[])
{
    printf("=============================================================\n");
    printf("SurrealDB C API Test Suite (Unity Fixture Framework)\n");
    printf("=============================================================\n\n");

    return UnityMain(argc, argv, sr_run_all_test_groups);
}
