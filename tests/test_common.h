#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <time.h>
#include <stdint.h>

// ============================================================================
// Benchmark Timing Macros
// ============================================================================

#define BENCH_START() \
    struct timespec _bench_start, _bench_end; \
    clock_gettime(CLOCK_MONOTONIC, &_bench_start)

#define BENCH_END() \
    clock_gettime(CLOCK_MONOTONIC, &_bench_end)

#define BENCH_ELAPSED_NS() \
    ((uint64_t)(_bench_end.tv_sec - _bench_start.tv_sec) * 1000000000ULL + \
     (uint64_t)(_bench_end.tv_nsec - _bench_start.tv_nsec))

#define BENCH_OPS_PER_SEC(ops) \
    ((double)(ops) * 1000000000.0 / (double)BENCH_ELAPSED_NS())

// Get current time in nanoseconds
static inline uint64_t get_time_ns_bench(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

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
