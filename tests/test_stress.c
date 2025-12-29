// ============================================================================
// Stress Tests for H-WFQ Scheduler
// ============================================================================
//
// - High volume enqueue/dequeue cycles
// - Rapid tenant/flow add/remove cycles
// - Weight reconfiguration during operation
// - Memory stability verification
//
// ============================================================================

#include "test_common.h"
#include "../include/hwfq.h"
#include "../src/hwfq_internal.h"
#include <stdlib.h>
#include <string.h>

// Global test counters
static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Helper Functions
// ============================================================================

static hwfq_scheduler_t *create_stress_scheduler(void) {
    hwfq_config_t config = {
        .max_tenants = 1000,
        .max_flows_per_tenant = 10000,
        .max_total_flows = 100000,
        .total_capacity = 10000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .alloc_fn = NULL,
        .free_fn = NULL,
        .session_available_fn = NULL
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    if (ret != HWFQ_SUCCESS) {
        return NULL;
    }
    return scheduler;
}

// ============================================================================
// Stress: High Volume
// ============================================================================

void stress_high_volume(void) {
    hwfq_scheduler_t *scheduler = create_stress_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    hwfq_flow_id_t flow_id = test_add_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    #define HIGH_VOLUME_CYCLES 1000  // Reduced for faster test execution

    printf("    Running %d enqueue/dequeue cycles...\n", HIGH_VOLUME_CYCLES);

    // Keep a small backlog to ensure scheduler stays active
    for (int i = 0; i < 100; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    }

    BENCH_START();
    for (int i = 0; i < HIGH_VOLUME_CYCLES; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Enqueue failed during stress");

        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Dequeue failed during stress");
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }
    BENCH_END();

    double elapsed_sec = BENCH_ELAPSED_NS() / 1000000000.0;
    printf("    Completed in %.2f seconds (%.0f ops/sec)\n",
           elapsed_sec, HIGH_VOLUME_CYCLES * 2 / elapsed_sec);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Stress: Tenant Churn
// ============================================================================

void stress_tenant_churn(void) {
    hwfq_scheduler_t *scheduler = create_stress_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    #define CHURN_CYCLES 1000

    printf("    Running %d tenant add/remove cycles...\n", CHURN_CYCLES);

    BENCH_START();
    for (int i = 0; i < CHURN_CYCLES; i++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 100 + (i % 100)
        };
        hwfq_tenant_id_t tenant_id;
        int ret = hwfq_add_tenant(scheduler, &alloc, &tenant_id);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant during churn");

        // Remove immediately (no backlog)
        ret = hwfq_remove_tenant(scheduler, tenant_id);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to remove tenant during churn");
    }
    BENCH_END();

    double elapsed_sec = BENCH_ELAPSED_NS() / 1000000000.0;
    printf("    Completed in %.2f seconds (%.0f cycles/sec)\n",
           elapsed_sec, CHURN_CYCLES / elapsed_sec);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Stress: Flow Churn
// ============================================================================

void stress_flow_churn(void) {
    hwfq_scheduler_t *scheduler = create_stress_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_allocation_t tenant_alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    #define FLOW_CHURN_CYCLES 1000

    printf("    Running %d flow add/remove cycles...\n", FLOW_CHURN_CYCLES);

    BENCH_START();
    for (int i = 0; i < FLOW_CHURN_CYCLES; i++) {
        hwfq_allocation_t flow_alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 50 + (i % 50)
        };

        hwfq_flow_id_t flow_id;
        ret = hwfq_add_flow(scheduler, tenant_id, &flow_alloc, &flow_id);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add flow during churn");

        ret = hwfq_remove_flow(scheduler, tenant_id, flow_id);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to remove flow during churn");
    }
    BENCH_END();

    double elapsed_sec = BENCH_ELAPSED_NS() / 1000000000.0;
    printf("    Completed in %.2f seconds (%.0f cycles/sec)\n",
           elapsed_sec, FLOW_CHURN_CYCLES / elapsed_sec);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Stress: Reconfiguration During Operation
// ============================================================================

void stress_reconfiguration(void) {
    hwfq_scheduler_t *scheduler = create_stress_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add some flows
    hwfq_flow_id_t flow_ids[10];
    for (int f = 0; f < 10; f++) {
        hwfq_add_flow(scheduler, tenant_id, &alloc, &flow_ids[f]);
    }

    #define RECONFIG_CYCLES 1000

    printf("    Running %d cycles with weight reconfigurations...\n", RECONFIG_CYCLES);

    // Pre-enqueue
    for (int f = 0; f < 10; f++) {
        for (int i = 0; i < 100; i++) {
            hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
            hwfq_enqueue(scheduler, tenant_id, flow_ids[f], &work);
        }
    }

    BENCH_START();
    for (int i = 0; i < RECONFIG_CYCLES; i++) {
        // Enqueue
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        hwfq_enqueue(scheduler, tenant_id, flow_ids[i % 10], &work);

        // Dequeue
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());

        // Reconfigure a flow every 100 iterations
        if (i % 100 == 0) {
            hwfq_allocation_t new_alloc = {
                .allocation_type = HWFQ_ALLOCATION_WEIGHT,
                .weight = 50 + (i / 100) % 100
            };
            hwfq_reconfigure_flow(scheduler, tenant_id, flow_ids[(i / 100) % 10], &new_alloc);
        }
    }
    BENCH_END();

    double elapsed_sec = BENCH_ELAPSED_NS() / 1000000000.0;
    printf("    Completed in %.2f seconds\n", elapsed_sec);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Stress: Memory Stability
// ============================================================================

// Custom allocator to track memory
static size_t g_alloc_count = 0;
static size_t g_free_count = 0;
static size_t g_bytes_allocated = 0;

static void *tracking_alloc(size_t size) {
    g_alloc_count++;
    g_bytes_allocated += size;
    return malloc(size);
}

static void tracking_free(void *ptr) {
    if (ptr != NULL) {
        g_free_count++;
    }
    free(ptr);
}

void stress_memory_stability(void) {
    g_alloc_count = 0;
    g_free_count = 0;
    g_bytes_allocated = 0;

    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .alloc_fn = tracking_alloc,
        .free_fn = tracking_free,
        .session_available_fn = NULL
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    size_t alloc_after_init = g_alloc_count;

    #define MEMORY_TEST_CYCLES 1000  // Reduced for faster test execution

    printf("    Running %d cycles with memory tracking...\n", MEMORY_TEST_CYCLES);

    // Add tenant
    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tenant_id;
    hwfq_add_tenant(scheduler, &alloc, &tenant_id);

    // Run cycles
    for (int i = 0; i < MEMORY_TEST_CYCLES; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        hwfq_enqueue(scheduler, tenant_id, 1, &work);

        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    size_t alloc_after_cycles = g_alloc_count;
    size_t free_after_cycles = g_free_count;

    // The allocation count after cycles minus init should equal free count
    // (each enqueue allocates context, each dequeue frees it)
    size_t cycle_allocs = alloc_after_cycles - alloc_after_init;

    printf("    Allocations during cycles: %zu\n", cycle_allocs);
    printf("    Frees during cycles: %zu\n", free_after_cycles);

    // Destroy scheduler
    hwfq_destroy(scheduler);

    printf("    Final alloc count: %zu\n", g_alloc_count);
    printf("    Final free count: %zu\n", g_free_count);

    // All allocations should be freed
    TEST_ASSERT(g_alloc_count == g_free_count, "Memory leak detected");

    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Stress Tests ===\n\n");

    printf("Running stress_high_volume...\n");
    stress_high_volume();

    printf("\nRunning stress_tenant_churn...\n");
    stress_tenant_churn();

    printf("\nRunning stress_flow_churn...\n");
    stress_flow_churn();

    printf("\nRunning stress_reconfiguration...\n");
    stress_reconfiguration();

    printf("\nRunning stress_memory_stability...\n");
    stress_memory_stability();

    printf("\n=== Stress Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
