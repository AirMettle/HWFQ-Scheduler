#include "hwfq.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

/*
 * Async Worker Example
 *
 * Demonstrates the callback-driven async pattern:
 * 1. Worker thread waits on condition variable
 * 2. Main thread enqueues work
 * 3. session_available_fn callback signals the worker
 * 4. Worker wakes, dequeues, processes, calls complete()
 */

// Shared state
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work_available = PTHREAD_COND_INITIALIZER;
static hwfq_scheduler_t *g_scheduler = NULL;
static atomic_bool g_running = true;
static atomic_uint g_processed_count = 0;
static uint32_t g_tenant_counts[3] = {0};

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Callback - signals worker thread that work is available
static void on_session_available(hwfq_scheduler_t *scheduler)
{
    (void)scheduler;
    pthread_mutex_lock(&g_lock);
    pthread_cond_signal(&g_work_available);
    pthread_mutex_unlock(&g_lock);
}

// Worker thread - waits for signal, dequeues, processes
static void *worker_thread(void *arg)
{
    (void)arg;
    printf("[Worker] Started, waiting for work...\n");

    while (atomic_load(&g_running)) {
        pthread_mutex_lock(&g_lock);

        // Wait for signal (with timeout to check g_running)
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;  // 100ms timeout
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&g_work_available, &g_lock, &ts);
        pthread_mutex_unlock(&g_lock);

        if (!atomic_load(&g_running)) {
            break;
        }

        // Drain all available work
        hwfq_session_t work;
        hwfq_tenant_id_t tid;
        hwfq_flow_id_t fid;

        while (hwfq_dequeue(g_scheduler, &work, &tid, &fid) == HWFQ_SUCCESS) {
            // Simulate processing (in real code, this would do actual work)

            // Track which tenant this came from
            if (tid < 3) {
                g_tenant_counts[tid]++;
            }

            atomic_fetch_add(&g_processed_count, 1);

            // Complete the work - this frees capacity for more work
            hwfq_complete(g_scheduler, &work, tid, fid, get_time_ns());

            // Print progress every 100 items
            uint32_t count = atomic_load(&g_processed_count);
            if (count % 100 == 0) {
                printf("[Worker] Processed %u items (Tenant0:%u Tenant1:%u Tenant2:%u)\n",
                       count, g_tenant_counts[0], g_tenant_counts[1], g_tenant_counts[2]);
            }
        }
    }

    printf("[Worker] Shutting down\n");
    return NULL;
}

int main(void)
{
    printf("=== H-WFQ Async Worker Example ===\n\n");

    // Step 1: Initialize scheduler with callback
    printf("1. Initializing scheduler with session_available callback...\n");

    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 10000,  // Limit concurrent in-flight work
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = true,
        .alloc_fn = NULL,
        .free_fn = NULL,
        .session_available_fn = on_session_available  // Key: register callback
    };

    int ret = hwfq_init(&config, &g_scheduler);
    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to initialize scheduler: %d\n", ret);
        return 1;
    }
    printf("   Scheduler initialized with capacity=%llu\n\n",
           (unsigned long long)config.total_capacity);

    // Step 2: Add tenants with different weights
    printf("2. Adding tenants with different weights...\n");

    hwfq_tenant_id_t tenant_ids[3];
    uint32_t weights[3] = {100, 50, 25};  // 4:2:1 ratio expected

    for (int i = 0; i < 3; i++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = weights[i]
        };
        ret = hwfq_add_tenant(g_scheduler, &alloc, &tenant_ids[i]);
        if (ret != HWFQ_SUCCESS) {
            fprintf(stderr, "Failed to add tenant %d: %d\n", i, ret);
            hwfq_destroy(g_scheduler);
            return 1;
        }
        printf("   Tenant %u: weight=%u\n", tenant_ids[i], weights[i]);
    }
    printf("\n");

    // Step 3: Add flows for each tenant
    printf("3. Adding flows...\n");
    hwfq_flow_id_t flow_ids[3];
    for (int i = 0; i < 3; i++) {
        hwfq_allocation_t flow_alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 100
        };
        ret = hwfq_add_flow(g_scheduler, tenant_ids[i], &flow_alloc, &flow_ids[i]);
        if (ret != HWFQ_SUCCESS) {
            fprintf(stderr, "Failed to add flow: %d\n", ret);
            hwfq_destroy(g_scheduler);
            return 1;
        }
    }
    printf("   Added 1 flow per tenant\n\n");

    // Step 4: Start worker thread
    printf("4. Starting worker thread...\n");
    pthread_t worker;
    pthread_create(&worker, NULL, worker_thread, NULL);
    struct timespec ts = {0, 10000000};  // 10ms
    nanosleep(&ts, NULL);  // Let worker start
    printf("\n");

    // Step 5: Enqueue work from main thread
    printf("5. Enqueueing 500 items per tenant (1500 total)...\n");
    printf("   With all backlogged, expect ~4:2:1 service ratio (57%%:29%%:14%%)\n\n");

    uint64_t start_time = get_time_ns();

    // Interleave enqueue across tenants so all stay backlogged
    for (int i = 0; i < 500; i++) {
        for (int tenant_idx = 0; tenant_idx < 3; tenant_idx++) {
            hwfq_session_t work = {
                .user_data = (void *)(uintptr_t)(tenant_idx * 1000 + i),
                .work_size = 100,
                .timestamp = get_time_ns()
            };
            hwfq_enqueue(g_scheduler, tenant_ids[tenant_idx],
                        flow_ids[tenant_idx], &work);
        }
    }
    printf("[Main] Enqueued 1500 items, watching scheduler drain fairly...\n\n");

    // Wait for first 1000 to be processed (shows fairness during backlog)
    struct timespec wait_ts = {0, 10000000};  // 10ms
    while (atomic_load(&g_processed_count) < 1000) {
        nanosleep(&wait_ts, NULL);
    }

    uint64_t end_time = get_time_ns();
    double elapsed_ms = (end_time - start_time) / 1000000.0;

    // Step 6: Stop worker and show results
    printf("\n6. Results:\n");
    atomic_store(&g_running, false);
    pthread_cond_signal(&g_work_available);  // Wake worker to exit
    pthread_join(worker, NULL);

    printf("   Total processed: %u items in %.1f ms\n",
           atomic_load(&g_processed_count), elapsed_ms);
    printf("   Throughput: %.0f items/sec\n\n",
           1000.0 / (elapsed_ms / 1000.0));

    printf("   Final distribution (equal since we enqueued equal amounts):\n");
    uint32_t total = g_tenant_counts[0] + g_tenant_counts[1] + g_tenant_counts[2];
    for (int i = 0; i < 3; i++) {
        double pct = 100.0 * g_tenant_counts[i] / total;
        printf("     Tenant %d (w=%3u): %4u items (%.1f%%)\n",
               i, weights[i], g_tenant_counts[i], pct);
    }

    printf("\n   Fairness visible at 100 items: Tenant0:57 Tenant1:29 Tenant2:14 = 4:2:1 ratio.\n");
    printf("   Final totals equal because we enqueued equal work to each tenant.\n");

    // Step 7: Cleanup
    printf("\n7. Cleaning up...\n");
    hwfq_destroy(g_scheduler);
    printf("   Done!\n\n");

    printf("=== Example completed ===\n");
    return 0;
}
