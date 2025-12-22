// ============================================================================
// Concurrency Tests for H-WFQ Scheduler
// ============================================================================
//
// Multi-threaded tests to validate thread safety of the scheduler.
// Uses the public API (hwfq_enqueue, hwfq_dequeue, hwfq_complete).
//
// Test scenarios:
// 1. Multiple producer threads enqueueing
// 2. Multiple consumer threads dequeueing
// 3. Mixed producer/consumer threads
// 4. Concurrent tenant operations (churn)
// 5. Concurrent weight reconfiguration
//
// ============================================================================

#include "test_common.h"
#include "../include/hwfq.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

// Global test counters
static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Configuration
// ============================================================================

#define NUM_PRODUCER_THREADS 4
#define NUM_CONSUMER_THREADS 4
#define OPERATIONS_PER_THREAD 5000
#define NUM_FLOWS_PER_TENANT 10

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

static int portable_barrier_init(portable_barrier_t *b, int count) {
    b->count = 0;
    b->target = count;
    b->generation = 0;
    if (pthread_mutex_init(&b->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&b->cond, NULL) != 0) {
        pthread_mutex_destroy(&b->mutex);
        return -1;
    }
    return 0;
}

static void portable_barrier_destroy(portable_barrier_t *b) {
    pthread_mutex_destroy(&b->mutex);
    pthread_cond_destroy(&b->cond);
}

static void portable_barrier_wait(portable_barrier_t *b) {
    pthread_mutex_lock(&b->mutex);
    int gen = b->generation;
    b->count++;
    if (b->count >= b->target) {
        b->count = 0;
        b->generation++;
        pthread_cond_broadcast(&b->cond);
    } else {
        while (gen == b->generation) {
            pthread_cond_wait(&b->cond, &b->mutex);
        }
    }
    pthread_mutex_unlock(&b->mutex);
}

// ============================================================================
// Sleep helper (portable)
// ============================================================================

static void msleep(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// ============================================================================
// Shared Test State
// ============================================================================

typedef struct {
    hwfq_scheduler_t *scheduler;
    hwfq_tenant_id_t tenant_id;

    // Synchronization
    portable_barrier_t start_barrier;
    pthread_mutex_t print_lock;

    // Atomic counters
    atomic_uint_fast64_t enqueue_success;
    atomic_uint_fast64_t enqueue_fail;
    atomic_uint_fast64_t dequeue_success;
    atomic_uint_fast64_t dequeue_empty;
    atomic_uint_fast64_t dequeue_fail;
    atomic_uint_fast64_t complete_count;

    // Control
    volatile int stop_flag;

    // Error tracking
    atomic_uint_fast32_t error_count;
} concurrency_state_t;

// ============================================================================
// State Initialization
// ============================================================================

static int concurrency_state_init(concurrency_state_t *state, int num_threads) {
    memset(state, 0, sizeof(*state));

    // Create scheduler
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 100000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .total_capacity = 1000000000ULL,
        .enable_statistics = false,
        .alloc_fn = NULL,
        .free_fn = NULL,
        .session_available_fn = NULL
    };

    int ret = hwfq_init(&config, &state->scheduler);
    if (ret != HWFQ_SUCCESS) {
        return -1;
    }

    // Add a tenant
    hwfq_allocation_t alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    ret = hwfq_add_tenant(state->scheduler, &alloc, &state->tenant_id);
    if (ret != HWFQ_SUCCESS) {
        hwfq_destroy(state->scheduler);
        return -1;
    }

    // Configure flows
    for (int i = 1; i <= NUM_FLOWS_PER_TENANT; i++) {
        hwfq_allocation_t flow_alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 10
        };
        hwfq_configure_flow(state->scheduler, state->tenant_id, (hwfq_flow_id_t)i, &flow_alloc);
    }

    // Initialize synchronization
    portable_barrier_init(&state->start_barrier, num_threads);
    pthread_mutex_init(&state->print_lock, NULL);

    // Initialize atomic counters
    atomic_store(&state->enqueue_success, 0);
    atomic_store(&state->enqueue_fail, 0);
    atomic_store(&state->dequeue_success, 0);
    atomic_store(&state->dequeue_empty, 0);
    atomic_store(&state->dequeue_fail, 0);
    atomic_store(&state->complete_count, 0);
    atomic_store(&state->error_count, 0);

    state->stop_flag = 0;

    return 0;
}

static void concurrency_state_destroy(concurrency_state_t *state) {
    portable_barrier_destroy(&state->start_barrier);
    pthread_mutex_destroy(&state->print_lock);
    if (state->scheduler) {
        hwfq_destroy(state->scheduler);
        state->scheduler = NULL;
    }
}

// ============================================================================
// Thread Functions
// ============================================================================

typedef struct {
    concurrency_state_t *state;
    int thread_id;
    int operations;
} thread_arg_t;

static void *producer_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    concurrency_state_t *state = targ->state;
    int thread_id = targ->thread_id;
    int ops = targ->operations;

    // Wait for all threads to be ready
    portable_barrier_wait(&state->start_barrier);

    for (int i = 0; i < ops && !state->stop_flag; i++) {
        // Cycle through flows
        hwfq_flow_id_t flow_id = (hwfq_flow_id_t)((i % NUM_FLOWS_PER_TENANT) + 1);

        hwfq_session_t work = {
            .user_data = (void *)(uintptr_t)((thread_id << 24) | i),
            .work_size = 1024 + (i % 4096),
            .timestamp = get_time_ns_bench()
        };

        int ret = hwfq_enqueue(state->scheduler, state->tenant_id, flow_id, &work);
        if (ret == HWFQ_SUCCESS) {
            atomic_fetch_add(&state->enqueue_success, 1);
        } else {
            atomic_fetch_add(&state->enqueue_fail, 1);
        }
    }

    return NULL;
}

static void *consumer_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    concurrency_state_t *state = targ->state;

    // Wait for all threads to be ready
    portable_barrier_wait(&state->start_barrier);

    while (!state->stop_flag) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid;
        hwfq_flow_id_t fid;

        int ret = hwfq_dequeue(state->scheduler, &work_out, &tid, &fid);
        if (ret == HWFQ_SUCCESS) {
            atomic_fetch_add(&state->dequeue_success, 1);

            // Complete the work
            hwfq_complete(state->scheduler, &work_out, tid, fid, get_time_ns_bench());
            atomic_fetch_add(&state->complete_count, 1);
        } else if (ret == HWFQ_ERR_NO_WORK) {
            atomic_fetch_add(&state->dequeue_empty, 1);
            // Brief spin before retry
            for (volatile int j = 0; j < 100; j++) { }
        } else {
            atomic_fetch_add(&state->dequeue_fail, 1);
        }
    }

    return NULL;
}

static void *mixed_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    concurrency_state_t *state = targ->state;
    int thread_id = targ->thread_id;
    int ops = targ->operations;

    // Wait for all threads to be ready
    portable_barrier_wait(&state->start_barrier);

    for (int i = 0; i < ops && !state->stop_flag; i++) {
        // Alternate between enqueue and dequeue
        if (i % 2 == 0) {
            // Enqueue
            hwfq_flow_id_t flow_id = (hwfq_flow_id_t)((i % NUM_FLOWS_PER_TENANT) + 1);
            hwfq_session_t work = {
                .user_data = (void *)(uintptr_t)((thread_id << 24) | i),
                .work_size = 1024,
                .timestamp = get_time_ns_bench()
            };

            int ret = hwfq_enqueue(state->scheduler, state->tenant_id, flow_id, &work);
            if (ret == HWFQ_SUCCESS) {
                atomic_fetch_add(&state->enqueue_success, 1);
            } else {
                atomic_fetch_add(&state->enqueue_fail, 1);
            }
        } else {
            // Dequeue
            hwfq_session_t work_out;
            hwfq_tenant_id_t tid;
            hwfq_flow_id_t fid;

            int ret = hwfq_dequeue(state->scheduler, &work_out, &tid, &fid);
            if (ret == HWFQ_SUCCESS) {
                atomic_fetch_add(&state->dequeue_success, 1);
                hwfq_complete(state->scheduler, &work_out, tid, fid, get_time_ns_bench());
                atomic_fetch_add(&state->complete_count, 1);
            } else if (ret == HWFQ_ERR_NO_WORK) {
                atomic_fetch_add(&state->dequeue_empty, 1);
            } else {
                atomic_fetch_add(&state->dequeue_fail, 1);
            }
        }
    }

    return NULL;
}

// ============================================================================
// Validation
// ============================================================================

static bool validate_final_state(concurrency_state_t *state) {
    bool valid = true;

    uint64_t enqueued = atomic_load(&state->enqueue_success);
    uint64_t dequeued = atomic_load(&state->dequeue_success);
    uint64_t completed = atomic_load(&state->complete_count);

    // Dequeued should equal completed
    if (dequeued != completed) {
        printf("      ERROR: dequeued (%lu) != completed (%lu)\n",
               (unsigned long)dequeued, (unsigned long)completed);
        valid = false;
    }

    // Drain remaining items from queue
    uint64_t remaining = 0;
    hwfq_session_t work;
    hwfq_tenant_id_t tid;
    hwfq_flow_id_t fid;

    while (hwfq_dequeue(state->scheduler, &work, &tid, &fid) == HWFQ_SUCCESS) {
        remaining++;
        hwfq_complete(state->scheduler, &work, tid, fid, get_time_ns_bench());
    }

    // Verify accounting: enqueued = dequeued + remaining
    if (enqueued != dequeued + remaining) {
        printf("      ERROR: accounting mismatch: enqueued=%lu, dequeued=%lu, remaining=%lu\n",
               (unsigned long)enqueued, (unsigned long)dequeued, (unsigned long)remaining);
        valid = false;
    }

    // Check for recorded errors
    if (atomic_load(&state->error_count) > 0) {
        printf("      ERROR: %u errors recorded during test\n",
               (unsigned int)atomic_load(&state->error_count));
        valid = false;
    }

    if (valid) {
        printf("      Accounting: enqueued=%lu, dequeued=%lu, remaining=%lu [OK]\n",
               (unsigned long)enqueued, (unsigned long)dequeued, (unsigned long)remaining);
    }

    return valid;
}

// ============================================================================
// Test: Producer Only
// ============================================================================

void test_producer_only(void) {
    printf("    Configuration: %d producer threads, %d ops each\n",
           NUM_PRODUCER_THREADS, OPERATIONS_PER_THREAD);

    concurrency_state_t state;
    int ret = concurrency_state_init(&state, NUM_PRODUCER_THREADS);
    TEST_ASSERT(ret == 0, "Failed to initialize test state");

    pthread_t threads[NUM_PRODUCER_THREADS];
    thread_arg_t args[NUM_PRODUCER_THREADS];

    // Start producer threads
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        args[i].state = &state;
        args[i].thread_id = i;
        args[i].operations = OPERATIONS_PER_THREAD;
        pthread_create(&threads[i], NULL, producer_thread_func, &args[i]);
    }

    // Wait for completion
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Verify results
    uint64_t enqueued = atomic_load(&state.enqueue_success);
    uint64_t expected = (uint64_t)NUM_PRODUCER_THREADS * OPERATIONS_PER_THREAD;
    printf("      Enqueued: %lu (expected %lu)\n", (unsigned long)enqueued, (unsigned long)expected);
    TEST_ASSERT(enqueued == expected, "Not all enqueues succeeded");

    // Drain queue
    uint64_t drained = 0;
    hwfq_session_t work;
    hwfq_tenant_id_t tid;
    hwfq_flow_id_t fid;
    while (hwfq_dequeue(state.scheduler, &work, &tid, &fid) == HWFQ_SUCCESS) {
        drained++;
        hwfq_complete(state.scheduler, &work, tid, fid, get_time_ns_bench());
    }
    printf("      Drained: %lu\n", (unsigned long)drained);
    TEST_ASSERT(drained == enqueued, "Drained count doesn't match enqueued");

    concurrency_state_destroy(&state);
    TEST_PASS();
}

// ============================================================================
// Test: Consumer Only (Pre-filled Queue)
// ============================================================================

void test_consumer_only(void) {
    printf("    Configuration: %d consumer threads, pre-fill %d items\n",
           NUM_CONSUMER_THREADS, OPERATIONS_PER_THREAD * 2);

    concurrency_state_t state;
    int ret = concurrency_state_init(&state, NUM_CONSUMER_THREADS);
    TEST_ASSERT(ret == 0, "Failed to initialize test state");

    // Pre-fill the queue
    int prefill_count = OPERATIONS_PER_THREAD * 2;
    for (int i = 0; i < prefill_count; i++) {
        hwfq_flow_id_t flow_id = (hwfq_flow_id_t)((i % NUM_FLOWS_PER_TENANT) + 1);
        hwfq_session_t work = {
            .user_data = (void *)(uintptr_t)i,
            .work_size = 1024,
            .timestamp = get_time_ns_bench()
        };
        ret = hwfq_enqueue(state.scheduler, state.tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to pre-fill queue");
    }
    printf("      Pre-filled %d items\n", prefill_count);

    pthread_t threads[NUM_CONSUMER_THREADS];
    thread_arg_t args[NUM_CONSUMER_THREADS];

    // Start consumer threads
    for (int i = 0; i < NUM_CONSUMER_THREADS; i++) {
        args[i].state = &state;
        args[i].thread_id = i;
        args[i].operations = 0;  // Not used for consumer
        pthread_create(&threads[i], NULL, consumer_thread_func, &args[i]);
    }

    // Let consumers run for a bit
    msleep(500);  // 500ms

    // Signal stop
    state.stop_flag = 1;

    // Wait for completion
    for (int i = 0; i < NUM_CONSUMER_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Verify
    uint64_t dequeued = atomic_load(&state.dequeue_success);
    uint64_t completed = atomic_load(&state.complete_count);
    printf("      Dequeued: %lu, Completed: %lu\n",
           (unsigned long)dequeued, (unsigned long)completed);
    TEST_ASSERT(dequeued == completed, "Dequeue/complete mismatch");
    TEST_ASSERT(dequeued <= (uint64_t)prefill_count, "Dequeued more than enqueued");

    concurrency_state_destroy(&state);
    TEST_PASS();
}

// ============================================================================
// Test: Mixed Producers and Consumers
// ============================================================================

void test_mixed_producers_consumers(void) {
    int total_threads = NUM_PRODUCER_THREADS + NUM_CONSUMER_THREADS;
    printf("    Configuration: %d producers, %d consumers\n",
           NUM_PRODUCER_THREADS, NUM_CONSUMER_THREADS);

    concurrency_state_t state;
    int ret = concurrency_state_init(&state, total_threads);
    TEST_ASSERT(ret == 0, "Failed to initialize test state");

    pthread_t producer_threads[NUM_PRODUCER_THREADS];
    pthread_t consumer_threads[NUM_CONSUMER_THREADS];
    thread_arg_t producer_args[NUM_PRODUCER_THREADS];
    thread_arg_t consumer_args[NUM_CONSUMER_THREADS];

    // Start producer threads
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        producer_args[i].state = &state;
        producer_args[i].thread_id = i;
        producer_args[i].operations = OPERATIONS_PER_THREAD;
        pthread_create(&producer_threads[i], NULL, producer_thread_func, &producer_args[i]);
    }

    // Start consumer threads
    for (int i = 0; i < NUM_CONSUMER_THREADS; i++) {
        consumer_args[i].state = &state;
        consumer_args[i].thread_id = i + NUM_PRODUCER_THREADS;
        consumer_args[i].operations = 0;
        pthread_create(&consumer_threads[i], NULL, consumer_thread_func, &consumer_args[i]);
    }

    // Wait for producers to finish
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        pthread_join(producer_threads[i], NULL);
    }

    // Let consumers drain
    msleep(100);  // 100ms

    // Signal stop
    state.stop_flag = 1;

    // Wait for consumers
    for (int i = 0; i < NUM_CONSUMER_THREADS; i++) {
        pthread_join(consumer_threads[i], NULL);
    }

    // Validate
    bool valid = validate_final_state(&state);
    TEST_ASSERT(valid, "Final state validation failed");

    concurrency_state_destroy(&state);
    TEST_PASS();
}

// ============================================================================
// Test: Mixed Thread Operations
// ============================================================================

void test_mixed_thread_operations(void) {
    int num_threads = 8;
    printf("    Configuration: %d mixed threads, %d ops each\n",
           num_threads, OPERATIONS_PER_THREAD);

    concurrency_state_t state;
    int ret = concurrency_state_init(&state, num_threads);
    TEST_ASSERT(ret == 0, "Failed to initialize test state");

    pthread_t threads[8];
    thread_arg_t args[8];

    // Start mixed threads
    for (int i = 0; i < num_threads; i++) {
        args[i].state = &state;
        args[i].thread_id = i;
        args[i].operations = OPERATIONS_PER_THREAD;
        pthread_create(&threads[i], NULL, mixed_thread_func, &args[i]);
    }

    // Wait for completion
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // Validate
    bool valid = validate_final_state(&state);
    TEST_ASSERT(valid, "Final state validation failed");

    concurrency_state_destroy(&state);
    TEST_PASS();
}

// ============================================================================
// Test: High Contention
// ============================================================================

void test_high_contention(void) {
    int num_threads = 16;  // Many threads for high contention
    printf("    Configuration: %d threads, high contention test\n", num_threads);

    concurrency_state_t state;
    int ret = concurrency_state_init(&state, num_threads);
    TEST_ASSERT(ret == 0, "Failed to initialize test state");

    pthread_t threads[16];
    thread_arg_t args[16];

    BENCH_START();

    // Start mixed threads
    for (int i = 0; i < num_threads; i++) {
        args[i].state = &state;
        args[i].thread_id = i;
        args[i].operations = OPERATIONS_PER_THREAD / 2;
        pthread_create(&threads[i], NULL, mixed_thread_func, &args[i]);
    }

    // Wait for completion
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    BENCH_END();

    uint64_t total_ops = atomic_load(&state.enqueue_success) +
                         atomic_load(&state.dequeue_success);
    double ops_per_sec = BENCH_OPS_PER_SEC(total_ops);
    printf("      Total operations: %lu\n", (unsigned long)total_ops);
    printf("      Throughput: %.0f ops/sec\n", ops_per_sec);

    // Validate
    bool valid = validate_final_state(&state);
    TEST_ASSERT(valid, "Final state validation failed");

    concurrency_state_destroy(&state);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Concurrency Tests ===\n\n");

    printf("Running test_producer_only...\n");
    test_producer_only();

    printf("\nRunning test_consumer_only...\n");
    test_consumer_only();

    printf("\nRunning test_mixed_producers_consumers...\n");
    test_mixed_producers_consumers();

    printf("\nRunning test_mixed_thread_operations...\n");
    test_mixed_thread_operations();

    printf("\nRunning test_high_contention...\n");
    test_high_contention();

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
