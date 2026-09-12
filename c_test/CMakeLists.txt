# ============================================================================
# SurrealDB C Library - Test Suite
# ============================================================================

# Make sure CMake can find <solution-root>/cmake/FindUnity.cmake
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

find_package(Unity REQUIRED)

# Put test outputs in the build tree (cleaner than writing into source/)
set(_test_bin_dir "${CMAKE_CURRENT_BINARY_DIR}/bin")

# ============================================================================
# Test Source Files (Unity Fixture based)
# ============================================================================

# Discovered rather than listed: c-tests/build.rs globs the same directory, and
# a hand-maintained list here would drift from it.
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/*.c")

# API Tests - individual test implementations
set(API_TEST_SOURCES
    src/api_tests/api_tests.c
)

# ============================================================================
# Test Runner Executable
# ============================================================================

add_executable(test_runner
    src/bin/main.c
    src/bin/runner.c
    ${TEST_SOURCES}
    ${API_TEST_SOURCES}
)

target_include_directories(test_runner PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/tests"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/api_tests"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/ThirdParty/Unity/extras/fixture/src"
)

target_link_libraries(test_runner PRIVATE
    Unity::unity
    surrealdb_c
)

set_target_properties(test_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_test_bin_dir}"
)

# Fix LNK4098: Rust library uses release runtime (/MD), suppress conflict with debug runtime
if(MSVC)
    target_link_options(test_runner PRIVATE /NODEFAULTLIB:MSVCRT)
endif()

# ============================================================================
# Scratch Test Executable (for ad-hoc testing)
# ============================================================================

add_executable(test_scratch
    src/test_scratch.c
)

target_include_directories(test_scratch PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(test_scratch PRIVATE
    surrealdb_c
)

set_target_properties(test_scratch PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_test_bin_dir}"
)

# Fix LNK4098: Rust library uses release runtime (/MD), suppress conflict with debug runtime
if(MSVC)
    target_link_options(test_scratch PRIVATE /NODEFAULTLIB:MSVCRT)
endif()

# ============================================================================
# CTest Integration
# ============================================================================

enable_testing()

# One test per Unity group rather than a single all-or-nothing entry, so CI
# reports which group failed. Group names are read back out of the corpus.
set(_sdb_test_groups "")
foreach(_src IN LISTS TEST_SOURCES)
    file(STRINGS "${_src}" _runners REGEX "^TEST_GROUP_RUNNER\\(")
    foreach(_line IN LISTS _runners)
        string(REGEX REPLACE "^TEST_GROUP_RUNNER\\(([A-Za-z0-9_]+)\\).*" "\\1" _group "${_line}")
        list(APPEND _sdb_test_groups "${_group}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES _sdb_test_groups)
list(SORT _sdb_test_groups)

foreach(_group IN LISTS _sdb_test_groups)
    add_test(NAME "surrealdb_c.${_group}" COMMAND test_runner -g "${_group}" -v)
    set_tests_properties("surrealdb_c.${_group}" PROPERTIES
        WORKING_DIRECTORY "${_test_bin_dir}"
    )
endforeach()

list(LENGTH _sdb_test_groups _sdb_group_count)
message(STATUS "Registered ${_sdb_group_count} test groups with CTest")

message(STATUS "Test binaries will be output to: ${_test_bin_dir}")
message(STATUS "Unity Fixture extension enabled for test suites")

# ============================================================================
# Examples
# ============================================================================

add_subdirectory(examples)
