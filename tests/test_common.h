#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>

// ============================================================================
// Test Framework Macros
// ============================================================================

#define TEST_ASSERT(condition, message)                                                            \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            printf("  [FAIL] %s: %s\n", __func__, message);                                        \
            g_tests_failed++;                                                                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST_PASS()                                                                                \
    do {                                                                                           \
        printf("  [PASS] %s\n", __func__);                                                         \
        g_tests_passed++;                                                                          \
    } while (0)

// ============================================================================
// Global Test Counters (must be defined in each test file)
// ============================================================================

// extern int g_tests_passed;
// extern int g_tests_failed;

#endif // TEST_COMMON_H
