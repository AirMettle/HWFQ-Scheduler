// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 AirMettle, Inc.

// ============================================================================
// Group Scheduler Concurrency Tests
// ============================================================================
//
// These tests directly exercise the group_scheduler internal locks by calling
// group_scheduler_* functions from multiple threads, bypassing the higher-level
// hwfq_scheduler lock.
//
// The existing test_concurrency.c tests only the hwfq_* API which is serialized
// by hwfq_scheduler_t->lock, so it couldn't detect missing group_scheduler locks.
//
// ============================================================================

#include "hwfq_group_scheduler.h"
#include "test_common.h"
#include "../include/hwfq.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Configuration
// ============================================================================

#define NUM_THREADS 8
#define OPS_PER_THREAD 2000
#define NUM_ENTRIES 4

// ============================================================================
// Test State
// ============================================================================

typedef struct {
    hwfq_scheduler_t *parent;
    group_scheduler_t *gs;
    portable_barrier_t start_barrier;

    atomic_uint_fast64_t enqueue_count;
    atomic_uint_fast64_t dequeue_count;
    atomic_uint_fast64_t configure_count;
    atomic_uint_fast64_t error_count;

    atomic_int stop_flag;
} test_state_t;

typedef struct {
    test_state_t *state;
    int thread_id;
    int ops;
} thread_arg_t;

static uint32_t g_entry_ids[NUM_ENTRIES];

static void configure_test_entries(group_scheduler_t *gs) {
    for (int i = 0; i < NUM_ENTRIES; i++) {
        test_alloc_entry(gs->entries, &g_entry_ids[i]);
        group_entry_config_t cfg = {
            .entry_id = g_entry_ids[i],
            .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 }
        };
        group_scheduler_configure_entry(gs, &cfg);
    }
}

static hwfq_scheduler_t *create_test_parent(void) {
    hwfq_config_t config = {
        .max_tenants = 1000,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 100000,
        .total_capacity = 1000000000ULL,
        .alloc_fn = NULL,
        .free_fn = NULL
    };

    hwfq_scheduler_t *parent = NULL;
    int ret = hwfq_init(&config, &parent);
    return (ret == HWFQ_SUCCESS) ? parent : NULL;
}

// ============================================================================
// Test 1: Concurrent Enqueue
// ============================================================================

static void *enqueue_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    test_state_t *state = targ->state;
    int thread_id = targ->thread_id;
    int ops = targ->ops;

    portable_barrier_wait(&state->start_barrier);

    for (int i = 0; i < ops; i++) {
        group_entry_id_t entry_id = g_entry_ids[thread_id % NUM_ENTRIES];
        uint64_t work_size = 1024 + (i % 4096);
        void *user_data = (void *)(uintptr_t)((thread_id << 16) | i);
        session_state_t *session = NULL;

        int ret = group_scheduler_enqueue(state->gs, entry_id, work_size, user_data, NULL, &session);
        if (ret == HWFQ_SUCCESS) {
            atomic_fetch_add(&state->enqueue_count, 1);
        } else {
            atomic_fetch_add(&state->error_count, 1);
        }
    }

    return NULL;
}

void test_gs_concurrent_enqueue(void) {
    printf("    Configuration: %d threads, %d enqueues each\n", NUM_THREADS, OPS_PER_THREAD);

    test_state_t state = {0};
    state.parent = create_test_parent();
    TEST_ASSERT(state.parent != NULL, "Failed to create parent scheduler");

    state.gs = test_create_group_scheduler(state.parent, 16, 2048, 1000000000ULL, 1000);
    TEST_ASSERT(state.gs != NULL, "Failed to create group scheduler");

    // Configure entries
    configure_test_entries(state.gs);

    portable_barrier_init(&state.start_barrier, NUM_THREADS);
    atomic_store(&state.enqueue_count, 0);
    atomic_store(&state.error_count, 0);

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].state = &state;
        args[i].thread_id = i;
        args[i].ops = OPS_PER_THREAD;
        pthread_create(&threads[i], NULL, enqueue_thread_func, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t enqueued = atomic_load(&state.enqueue_count);
    uint64_t errors = atomic_load(&state.error_count);
    uint32_t session_count = group_scheduler_get_session_count(state.gs);

    printf("      Enqueued: %lu, Errors: %lu, Session count: %u\n",
           (unsigned long)enqueued, (unsigned long)errors, session_count);

    TEST_ASSERT(errors == 0, "Enqueue errors occurred");
    TEST_ASSERT(session_count == enqueued, "Session count mismatch");

    session_state_t *s;
    while ((s = group_scheduler_dequeue(state.gs)) != NULL) {
        hwfq_free(state.parent, s);
    }

    portable_barrier_destroy(&state.start_barrier);
    test_destroy_group_scheduler(state.parent, state.gs);
    hwfq_destroy(state.parent);
    TEST_PASS();
}

// ============================================================================
// Test 2: Concurrent Dequeue
// ============================================================================

static void *dequeue_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    test_state_t *state = targ->state;

    portable_barrier_wait(&state->start_barrier);

    while (!atomic_load(&state->stop_flag)) {
        session_state_t *session = group_scheduler_dequeue(state->gs);
        if (session != NULL) {
            atomic_fetch_add(&state->dequeue_count, 1);
            hwfq_free(state->parent, session);
        } else {
            for (volatile int j = 0; j < 100; j++) { }
        }
    }

    session_state_t *session;
    while ((session = group_scheduler_dequeue(state->gs)) != NULL) {
        atomic_fetch_add(&state->dequeue_count, 1);
        hwfq_free(state->parent, session);
    }

    return NULL;
}

void test_gs_concurrent_dequeue(void) {
    int prefill = OPS_PER_THREAD * 2;
    printf("    Configuration: %d threads, pre-fill %d items\n", NUM_THREADS, prefill);

    test_state_t state = {0};
    state.parent = create_test_parent();
    TEST_ASSERT(state.parent != NULL, "Failed to create parent scheduler");

    state.gs = test_create_group_scheduler(state.parent, 16, 2048, 1000000000ULL, 1000);
    TEST_ASSERT(state.gs != NULL, "Failed to create group scheduler");

    configure_test_entries(state.gs);

    for (int i = 0; i < prefill; i++) {
        session_state_t *session;
        int ret = group_scheduler_enqueue(state.gs, g_entry_ids[0], 1024, (void *)(uintptr_t)i, NULL, &session);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Pre-fill enqueue failed");
    }
    printf("      Pre-filled %d items\n", prefill);

    portable_barrier_init(&state.start_barrier, NUM_THREADS);
    atomic_store(&state.dequeue_count, 0);
    atomic_store(&state.stop_flag, 0);

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].state = &state;
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, dequeue_thread_func, &args[i]);
    }

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
    nanosleep(&ts, NULL);

    atomic_store(&state.stop_flag, 1);

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t dequeued = atomic_load(&state.dequeue_count);
    printf("      Dequeued: %lu (expected %d)\n", (unsigned long)dequeued, prefill);

    TEST_ASSERT(dequeued == (uint64_t)prefill, "Dequeue count mismatch");
    TEST_ASSERT(group_scheduler_is_empty(state.gs), "Scheduler should be empty");

    portable_barrier_destroy(&state.start_barrier);
    test_destroy_group_scheduler(state.parent, state.gs);
    hwfq_destroy(state.parent);
    TEST_PASS();
}

// ============================================================================
// Test 3: Mixed Enqueue/Dequeue
// ============================================================================

static void *mixed_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    test_state_t *state = targ->state;
    int thread_id = targ->thread_id;
    int ops = targ->ops;

    portable_barrier_wait(&state->start_barrier);

    for (int i = 0; i < ops; i++) {
        if (i % 2 == 0) {
            group_entry_id_t entry_id = g_entry_ids[thread_id % NUM_ENTRIES];
            session_state_t *session;
            int ret = group_scheduler_enqueue(state->gs, entry_id, 1024,
                                              (void *)(uintptr_t)((thread_id << 16) | i), NULL, &session);
            if (ret == HWFQ_SUCCESS) {
                atomic_fetch_add(&state->enqueue_count, 1);
            }
        } else {
            session_state_t *session = group_scheduler_dequeue(state->gs);
            if (session != NULL) {
                atomic_fetch_add(&state->dequeue_count, 1);
                hwfq_free(state->parent, session);
            }
        }
    }

    return NULL;
}

void test_gs_mixed_enqueue_dequeue(void) {
    printf("    Configuration: %d threads, %d mixed ops each\n", NUM_THREADS, OPS_PER_THREAD);

    test_state_t state = {0};
    state.parent = create_test_parent();
    TEST_ASSERT(state.parent != NULL, "Failed to create parent scheduler");

    state.gs = test_create_group_scheduler(state.parent, 16, 2048, 1000000000ULL, 1000);
    TEST_ASSERT(state.gs != NULL, "Failed to create group scheduler");

    // Configure entries
    configure_test_entries(state.gs);

    portable_barrier_init(&state.start_barrier, NUM_THREADS);
    atomic_store(&state.enqueue_count, 0);
    atomic_store(&state.dequeue_count, 0);

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].state = &state;
        args[i].thread_id = i;
        args[i].ops = OPS_PER_THREAD;
        pthread_create(&threads[i], NULL, mixed_thread_func, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t enqueued = atomic_load(&state.enqueue_count);
    uint64_t dequeued = atomic_load(&state.dequeue_count);

    uint64_t remaining = 0;
    session_state_t *s;
    while ((s = group_scheduler_dequeue(state.gs)) != NULL) {
        remaining++;
        hwfq_free(state.parent, s);
    }

    printf("      Enqueued: %lu, Dequeued: %lu, Remaining: %lu\n",
           (unsigned long)enqueued, (unsigned long)dequeued, (unsigned long)remaining);

    TEST_ASSERT(enqueued == dequeued + remaining, "Accounting mismatch");

    portable_barrier_destroy(&state.start_barrier);
    test_destroy_group_scheduler(state.parent, state.gs);
    hwfq_destroy(state.parent);
    TEST_PASS();
}

// ============================================================================
// Test 4: Concurrent Configure Entry
// ============================================================================

static void *configure_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    test_state_t *state = targ->state;
    int thread_id = targ->thread_id;
    int ops = targ->ops;

    portable_barrier_wait(&state->start_barrier);

    for (int i = 0; i < ops; i++) {
        group_entry_id_t entry_id = g_entry_ids[(thread_id + i) % NUM_ENTRIES];
        group_entry_config_t cfg = {
            .entry_id = entry_id,
            .allocation = {
                .allocation_type = HWFQ_ALLOCATION_WEIGHT,
                .weight = (uint32_t)(50 + (i % 100))
            }
        };

        int ret = group_scheduler_configure_entry(state->gs, &cfg);
        if (ret == HWFQ_SUCCESS) {
            atomic_fetch_add(&state->configure_count, 1);
        } else {
            atomic_fetch_add(&state->error_count, 1);
        }
    }

    return NULL;
}

void test_gs_concurrent_configure_entry(void) {
    printf("    Configuration: %d threads, %d reconfigurations each\n", NUM_THREADS, OPS_PER_THREAD);

    test_state_t state = {0};
    state.parent = create_test_parent();
    TEST_ASSERT(state.parent != NULL, "Failed to create parent scheduler");

    state.gs = test_create_group_scheduler(state.parent, 16, 2048, 1000000000ULL, 1000);
    TEST_ASSERT(state.gs != NULL, "Failed to create group scheduler");

    configure_test_entries(state.gs);

    portable_barrier_init(&state.start_barrier, NUM_THREADS);
    atomic_store(&state.configure_count, 0);
    atomic_store(&state.error_count, 0);

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].state = &state;
        args[i].thread_id = i;
        args[i].ops = OPS_PER_THREAD;
        pthread_create(&threads[i], NULL, configure_thread_func, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t configured = atomic_load(&state.configure_count);
    uint64_t errors = atomic_load(&state.error_count);

    printf("      Configurations: %lu, Errors: %lu\n",
           (unsigned long)configured, (unsigned long)errors);

    TEST_ASSERT(errors == 0, "Configuration errors occurred");

    portable_barrier_destroy(&state.start_barrier);
    test_destroy_group_scheduler(state.parent, state.gs);
    hwfq_destroy(state.parent);
    TEST_PASS();
}

// ============================================================================
// Test 5: Stress All Operations
// ============================================================================

static void *stress_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    test_state_t *state = targ->state;
    int thread_id = targ->thread_id;
    int ops = targ->ops;

    portable_barrier_wait(&state->start_barrier);

    for (int i = 0; i < ops; i++) {
        int op = (thread_id + i) % 5;

        switch (op) {
        case 0:
        case 1: {
            group_entry_id_t entry_id = g_entry_ids[i % NUM_ENTRIES];
            session_state_t *session;
            if (group_scheduler_enqueue(state->gs, entry_id, 1024,
                                        (void *)(uintptr_t)i, NULL, &session) == HWFQ_SUCCESS) {
                atomic_fetch_add(&state->enqueue_count, 1);
            }
            break;
        }
        case 2:
        case 3: {
            session_state_t *session = group_scheduler_dequeue(state->gs);
            if (session != NULL) {
                atomic_fetch_add(&state->dequeue_count, 1);
                hwfq_free(state->parent, session);
            }
            break;
        }
        case 4: {
            group_entry_config_t cfg = {
                .entry_id = g_entry_ids[i % NUM_ENTRIES],
                .allocation = {
                    .allocation_type = HWFQ_ALLOCATION_WEIGHT,
                    .weight = (uint32_t)(50 + (i % 100))
                }
            };
            if (group_scheduler_configure_entry(state->gs, &cfg) == HWFQ_SUCCESS) {
                atomic_fetch_add(&state->configure_count, 1);
            }
            break;
        }
        }

        if (i % 100 == 0) {
            group_scheduler_update_virtual_time(state->gs, 1000);
            (void)group_scheduler_get_virtual_time(state->gs);
            (void)group_scheduler_get_session_count(state->gs);
            (void)group_scheduler_is_empty(state->gs);
        }
    }

    return NULL;
}

void test_gs_stress_all_operations(void) {
    printf("    Configuration: %d threads, %d mixed operations each\n", NUM_THREADS, OPS_PER_THREAD);

    test_state_t state = {0};
    state.parent = create_test_parent();
    TEST_ASSERT(state.parent != NULL, "Failed to create parent scheduler");

    state.gs = test_create_group_scheduler(state.parent, 16, 2048, 1000000000ULL, 1000);
    TEST_ASSERT(state.gs != NULL, "Failed to create group scheduler");

    configure_test_entries(state.gs);

    portable_barrier_init(&state.start_barrier, NUM_THREADS);
    atomic_store(&state.enqueue_count, 0);
    atomic_store(&state.dequeue_count, 0);
    atomic_store(&state.configure_count, 0);

    BENCH_START();

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].state = &state;
        args[i].thread_id = i;
        args[i].ops = OPS_PER_THREAD;
        pthread_create(&threads[i], NULL, stress_thread_func, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    BENCH_END();

    uint64_t enqueued = atomic_load(&state.enqueue_count);
    uint64_t dequeued = atomic_load(&state.dequeue_count);
    uint64_t configured = atomic_load(&state.configure_count);

    uint64_t remaining = 0;
    session_state_t *s;
    while ((s = group_scheduler_dequeue(state.gs)) != NULL) {
        remaining++;
        hwfq_free(state.parent, s);
    }

    uint64_t total_ops = enqueued + dequeued + configured;
    double ops_per_sec = BENCH_OPS_PER_SEC(total_ops);

    printf("      Enqueued: %lu, Dequeued: %lu, Configured: %lu, Remaining: %lu\n",
           (unsigned long)enqueued, (unsigned long)dequeued,
           (unsigned long)configured, (unsigned long)remaining);
    printf("      Throughput: %.0f ops/sec\n", ops_per_sec);

    TEST_ASSERT(enqueued == dequeued + remaining, "Accounting mismatch");

    portable_barrier_destroy(&state.start_barrier);
    test_destroy_group_scheduler(state.parent, state.gs);
    hwfq_destroy(state.parent);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Group Scheduler Concurrency Tests ===\n");
    printf("(Tests group_scheduler internal locks directly)\n\n");

    printf("Running test_gs_concurrent_enqueue...\n");
    test_gs_concurrent_enqueue();

    printf("\nRunning test_gs_concurrent_dequeue...\n");
    test_gs_concurrent_dequeue();

    printf("\nRunning test_gs_mixed_enqueue_dequeue...\n");
    test_gs_mixed_enqueue_dequeue();

    printf("\nRunning test_gs_concurrent_configure_entry...\n");
    test_gs_concurrent_configure_entry();

    printf("\nRunning test_gs_stress_all_operations...\n");
    test_gs_stress_all_operations();

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
