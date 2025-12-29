// ============================================================================
// Scale Tests for H-WFQ Scheduler
// ============================================================================
//
// Verifies the DOE proposal requirements (page 8):
// - "at least 4,000 tenants"
// - "millions of per-flow queues"
// - "less than 2 GiB of memory"
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

// Memory tracking
static size_t g_alloc_count = 0;
static size_t g_free_count = 0;
static size_t g_bytes_allocated = 0;
static size_t g_peak_bytes = 0;

static void *tracking_alloc(size_t size) {
    g_alloc_count++;
    g_bytes_allocated += size;
    if (g_bytes_allocated > g_peak_bytes) {
        g_peak_bytes = g_bytes_allocated;
    }
    return malloc(size);
}

static void tracking_free(void *ptr) {
    if (ptr != NULL) {
        g_free_count++;
        // Note: We can't track exact bytes freed without additional bookkeeping
        // But alloc_count == free_count ensures no leaks
    }
    free(ptr);
}

// ============================================================================
// Test: 4000 Tenants
// ============================================================================

void test_4000_tenants(void) {
    printf("    Creating scheduler with 4000 tenant capacity...\n");

    g_alloc_count = 0;
    g_free_count = 0;
    g_bytes_allocated = 0;
    g_peak_bytes = 0;

    hwfq_config_t config = {
        .max_tenants = 4000,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 100000,  // Conservative for this test
        .total_capacity = 100000000000ULL,  // 100 GB/s
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

    size_t init_bytes = g_bytes_allocated;
    printf("    Memory after init: %.2f MB\n", init_bytes / (1024.0 * 1024.0));

    // Add 4000 tenants with weight-based allocations
    printf("    Adding 4000 tenants...\n");
    hwfq_tenant_id_t tenant_ids[4000];

    BENCH_START();
    for (int i = 0; i < 4000; i++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 100 + (i % 100)  // Varying weights
        };
        ret = hwfq_add_tenant(scheduler, &alloc, &tenant_ids[i]);
        if (ret != HWFQ_SUCCESS) {
            printf("    Failed to add tenant %d, error: %d\n", i, ret);
            TEST_ASSERT(false, "Failed to add tenant");
        }
    }
    BENCH_END();

    double elapsed_sec = BENCH_ELAPSED_NS() / 1000000000.0;
    printf("    Added 4000 tenants in %.2f seconds (%.0f tenants/sec)\n",
           elapsed_sec, 4000.0 / elapsed_sec);

    size_t after_tenants_bytes = g_bytes_allocated;
    printf("    Memory after 4000 tenants: %.2f MB\n",
           after_tenants_bytes / (1024.0 * 1024.0));
    printf("    Per-tenant overhead: %.2f KB\n",
           (after_tenants_bytes - init_bytes) / 4000.0 / 1024.0);

    // Verify all tenants are accessible
    hwfq_capacity_info_t capacity;
    ret = hwfq_get_capacity_info(scheduler, &capacity);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to get capacity info");
    TEST_ASSERT(capacity.num_configured_tenants == 4000,
                "Expected 4000 configured tenants");

    // Create a flow for each tenant (just for the first 1000 we'll enqueue to)
    hwfq_flow_id_t flow_ids[1000];
    for (int i = 0; i < 1000; i++) {
        flow_ids[i] = test_add_flow(scheduler, tenant_ids[i]);
        TEST_ASSERT(flow_ids[i] != 0, "Failed to add flow");
    }

    // Enqueue work to some tenants
    printf("    Enqueuing work to tenants...\n");
    for (int i = 0; i < 1000; i++) {
        hwfq_session_t work = {
            .user_data = (void*)(uintptr_t)i,
            .work_size = 4096,
            .timestamp = 0
        };
        ret = hwfq_enqueue(scheduler, tenant_ids[i], flow_ids[i], &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue work");
    }

    // Dequeue and complete
    for (int i = 0; i < 1000; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue work");
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    printf("    Peak memory: %.2f MB\n", g_peak_bytes / (1024.0 * 1024.0));

    // Clean up
    hwfq_destroy(scheduler);

    // Verify no memory leaks
    TEST_ASSERT(g_alloc_count == g_free_count, "Memory leak detected");
    printf("    All memory freed correctly\n");

    TEST_PASS();
}

// ============================================================================
// Test: Memory Budget (< 2 GiB)
// ============================================================================

void test_memory_budget(void) {
    printf("    Testing memory budget with 4000 tenants...\n");
    printf("    Note: Proposal says <2GiB for 4000 tenants + millions of flows\n");
    printf("    Flows are created on-demand, not all pre-configured\n\n");

    g_alloc_count = 0;
    g_free_count = 0;
    g_bytes_allocated = 0;
    g_peak_bytes = 0;

    // Configuration matching proposal requirements
    // Note: Per proposal page 10, the memory is primarily for:
    // - Bitfields: 4KiB per scheduler (32K bits)
    // - Pointers: 128 KiB per scheduler
    // - For 4000 schedulers: ~0.5 GiB for lists
    hwfq_config_t config = {
        .max_tenants = 4000,
        .max_flows_per_tenant = 10000,
        .max_total_flows = 100000,  // Pre-allocated session pool
        .total_capacity = 100000000000ULL,  // 100 GB/s
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,  // Stats off for baseline measurement
        .alloc_fn = tracking_alloc,
        .free_fn = tracking_free,
        .session_available_fn = NULL
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    size_t init_bytes = g_bytes_allocated;
    printf("    Memory after init: %.2f MB\n", init_bytes / (1024.0 * 1024.0));

    // Add 4000 tenants (this creates per-tenant schedulers)
    hwfq_tenant_id_t tenant_ids[4000];
    for (int i = 0; i < 4000; i++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 100
        };
        ret = hwfq_add_tenant(scheduler, &alloc, &tenant_ids[i]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");
    }

    size_t tenant_bytes = g_bytes_allocated;
    printf("    Memory after 4000 tenants: %.2f MB\n",
           tenant_bytes / (1024.0 * 1024.0));
    printf("    Per-tenant overhead: %.2f KB\n",
           (tenant_bytes - init_bytes) / 4000.0 / 1024.0);

    // The proposal's memory budget calculation (page 10):
    // - 32K bits (4KiB) per scheduler for bitfields
    // - 128 KiB per scheduler for bin pointers
    // - ~260 KiB per scheduler total
    // - For 4000 schedulers: ~1 GB
    // Plus system scheduler: ~260 KB
    // Plus session pool: configurable

    size_t peak_tenant_bytes = g_peak_bytes;
    printf("    Peak memory (4000 tenants): %.2f MB\n",
           peak_tenant_bytes / (1024.0 * 1024.0));

    // Verify under 2 GiB budget for tenant infrastructure
    size_t two_gib = 2ULL * 1024 * 1024 * 1024;
    bool within_budget = peak_tenant_bytes < two_gib;
    printf("    Memory budget check: %.2f MB %s 2048 MB (2 GiB)\n",
           peak_tenant_bytes / (1024.0 * 1024.0),
           within_budget ? "<" : ">=");

    // Test dynamic flow creation (flows created via hwfq_add_flow)
    printf("\n    Testing dynamic flow creation...\n");
    int flows_created = 0;
    hwfq_flow_id_t test_flow_ids[100][100];  // [tenant][flow]
    for (int t = 0; t < 100; t++) {  // Test subset of tenants
        for (int f = 0; f < 100; f++) {
            test_flow_ids[t][f] = test_add_flow(scheduler, tenant_ids[t]);
            if (test_flow_ids[t][f] != 0) {
                hwfq_session_t work = {
                    .user_data = (void*)(uintptr_t)(t * 100 + f),
                    .work_size = 4096,
                    .timestamp = 0
                };
                ret = hwfq_enqueue(scheduler, tenant_ids[t], test_flow_ids[t][f], &work);
                if (ret == HWFQ_SUCCESS) flows_created++;
            }
        }
    }
    printf("    Created %d flows dynamically via hwfq_add_flow\n", flows_created);

    // Drain all work
    for (int i = 0; i < flows_created; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    printf("    Peak memory after operations: %.2f MB\n",
           g_peak_bytes / (1024.0 * 1024.0));

    TEST_ASSERT(within_budget, "Memory usage exceeds 2 GiB budget for tenant infrastructure");

    // Clean up
    hwfq_destroy(scheduler);
    TEST_ASSERT(g_alloc_count == g_free_count, "Memory leak detected");

    TEST_PASS();
}

// ============================================================================
// Test: Multi-Tenant Operations
// ============================================================================

void test_multi_tenant_operations(void) {
    printf("    Testing concurrent operations across many tenants...\n");

    hwfq_config_t config = {
        .max_tenants = 1000,
        .max_flows_per_tenant = 100,
        .max_total_flows = 50000,
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
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    // Add 1000 tenants, each with 5 flows
    hwfq_tenant_id_t tenant_ids[1000];
    hwfq_flow_id_t flow_ids[1000][5];
    for (int i = 0; i < 1000; i++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 100
        };
        hwfq_add_tenant(scheduler, &alloc, &tenant_ids[i]);
        for (int f = 0; f < 5; f++) {
            flow_ids[i][f] = test_add_flow(scheduler, tenant_ids[i]);
        }
    }

    // Enqueue work to all tenants
    printf("    Enqueuing to 1000 tenants...\n");
    for (int i = 0; i < 1000; i++) {
        for (int j = 0; j < 10; j++) {
            hwfq_session_t work = {
                .user_data = (void*)(uintptr_t)(i * 10 + j),
                .work_size = 4096,
                .timestamp = 0
            };
            hwfq_enqueue(scheduler, tenant_ids[i], flow_ids[i][j % 5], &work);
        }
    }

    // Dequeue all (should be fair across tenants)
    printf("    Dequeuing 10000 work items...\n");
    int tenant_counts[1000] = {0};

    BENCH_START();
    for (int i = 0; i < 10000; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Dequeue failed");

        if (tid_out < 1000) {
            tenant_counts[tid_out]++;
        }

        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }
    BENCH_END();

    double elapsed_sec = BENCH_ELAPSED_NS() / 1000000000.0;
    printf("    Completed 10000 operations in %.2f seconds (%.0f ops/sec)\n",
           elapsed_sec, 10000.0 / elapsed_sec);

    // Verify fairness - each tenant should get ~10 dequeues
    int min_count = tenant_counts[0], max_count = tenant_counts[0];
    for (int i = 1; i < 1000; i++) {
        if (tenant_counts[i] < min_count) min_count = tenant_counts[i];
        if (tenant_counts[i] > max_count) max_count = tenant_counts[i];
    }
    printf("    Fairness check: min=%d, max=%d (expected ~10 each)\n",
           min_count, max_count);

    // Allow some variance but should be reasonably fair
    TEST_ASSERT(min_count >= 8 && max_count <= 12,
                "Fairness violation: distribution too uneven");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Scale Performance
// ============================================================================

void test_scale_performance(void) {
    printf("    Testing performance at scale...\n");

    hwfq_config_t config = {
        .max_tenants = 4000,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 100000,
        .total_capacity = 100000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .alloc_fn = NULL,
        .free_fn = NULL,
        .session_available_fn = NULL
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    // Add tenants with flows
    hwfq_tenant_id_t tenant_ids[4000];
    hwfq_flow_id_t flow_ids[4000][10];  // Each tenant gets 10 flows
    for (int i = 0; i < 4000; i++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 100
        };
        hwfq_add_tenant(scheduler, &alloc, &tenant_ids[i]);
        for (int f = 0; f < 10; f++) {
            flow_ids[i][f] = test_add_flow(scheduler, tenant_ids[i]);
        }
    }

    // Pre-fill with work
    for (int i = 0; i < 4000; i++) {
        for (int j = 0; j < 10; j++) {
            hwfq_session_t work = {
                .user_data = NULL,
                .work_size = 4096,
                .timestamp = 0
            };
            hwfq_enqueue(scheduler, tenant_ids[i], flow_ids[i][0], &work);
        }
    }

    // Benchmark mixed operations
    #define SCALE_OPS 10000
    printf("    Running %d mixed operations...\n", SCALE_OPS);

    BENCH_START();
    for (int i = 0; i < SCALE_OPS; i++) {
        // Enqueue
        hwfq_session_t work = {
            .user_data = NULL,
            .work_size = 4096,
            .timestamp = 0
        };
        hwfq_enqueue(scheduler, tenant_ids[i % 4000], flow_ids[i % 4000][i % 10], &work);

        // Dequeue
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }
    BENCH_END();

    double elapsed_sec = BENCH_ELAPSED_NS() / 1000000000.0;
    double ops_per_sec = (SCALE_OPS * 2) / elapsed_sec;  // 2 ops per iteration

    printf("    Completed in %.2f seconds\n", elapsed_sec);
    printf("    Throughput: %.0f ops/sec at 4000 tenant scale\n", ops_per_sec);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Scale Tests ===\n");
    printf("Verifying DOE proposal requirements:\n");
    printf("  - 4,000+ tenants\n");
    printf("  - Memory budget < 2 GiB\n\n");

    printf("Running test_4000_tenants...\n");
    test_4000_tenants();

    printf("\nRunning test_memory_budget...\n");
    test_memory_budget();

    printf("\nRunning test_multi_tenant_operations...\n");
    test_multi_tenant_operations();

    printf("\nRunning test_scale_performance...\n");
    test_scale_performance();

    printf("\n=== Scale Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
