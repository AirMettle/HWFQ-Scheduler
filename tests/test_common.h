#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "../include/hwfq.h"
#include "../src/hwfq_group_scheduler.h"
#include "../src/hwfq_chunked_entries.h"
#include "../src/hwfq_group_scheduler_internal.h"

// ============================================================================
// Portable Barrier Implementation (for macOS compatibility)
// ============================================================================

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int target;
    int generation;
} portable_barrier_t;

int portable_barrier_init(portable_barrier_t *b, int count);
void portable_barrier_destroy(portable_barrier_t *b);
void portable_barrier_wait(portable_barrier_t *b);

// ============================================================================
// Sleep Helper (portable)
// ============================================================================

void msleep(int ms);

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
// Test Helper Functions
// ============================================================================

group_scheduler_t *test_create_group_scheduler(
    hwfq_scheduler_t *parent,
    uint32_t num_groups,
    uint32_t bins_per_group,
    uint64_t total_capacity,
    uint32_t max_entries);

void test_destroy_group_scheduler(hwfq_scheduler_t *parent, group_scheduler_t *gs);

void test_drain_all_sessions(hwfq_scheduler_t *parent, group_scheduler_t *gs);

void test_hwfq_destroy(hwfq_scheduler_t *scheduler);

int test_alloc_entry(hwfq_chunked_entries_t *entries, uint32_t *entry_id_out);

hwfq_flow_id_t test_add_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id);

hwfq_flow_id_t test_add_flow_with_weight(hwfq_scheduler_t *scheduler,
                                          hwfq_tenant_id_t tenant_id,
                                          uint32_t weight);

#endif // TEST_COMMON_H
