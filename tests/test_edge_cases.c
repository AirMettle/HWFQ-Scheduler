// ============================================================================
// Edge Case Tests for H-WFQ Scheduler
// ============================================================================
//
// Story 5: Comprehensive Test Suite - Edge Cases
// - Max tenants limit
// - Max flows per tenant limit
// - Zero/max work sizes
// - Rapid empty/full transitions
// - Single session fairness (ordering)
//
// ============================================================================

#include "test_common.h"
#include "../include/hwfq.h"
#include "../src/hwfq_internal.h"
#include "../src/hwfq_group_scheduler_internal.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

// Global test counters
static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Test: Max Tenants Limit
// ============================================================================

void test_max_tenants(void) {
    #define TEST_MAX_TENANTS 100

    hwfq_config_t config = {
        .max_tenants = TEST_MAX_TENANTS,
        .max_flows_per_tenant = 100,
        .max_total_flows = 10000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    // Add max_tenants tenants
    hwfq_tenant_id_t tenant_ids[TEST_MAX_TENANTS];
    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };

    for (int i = 0; i < TEST_MAX_TENANTS; i++) {
        ret = hwfq_add_tenant(scheduler, &alloc, &tenant_ids[i]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant within limit");
    }

    // Try to add one more - should fail
    hwfq_tenant_id_t extra_tenant;
    ret = hwfq_add_tenant(scheduler, &alloc, &extra_tenant);
    TEST_ASSERT(ret == HWFQ_ERR_NO_MEMORY, "Should fail when exceeding max tenants");

    printf("    Successfully added %d tenants, correctly rejected tenant %d\n",
           TEST_MAX_TENANTS, TEST_MAX_TENANTS + 1);

    // Remove one and try again - should succeed
    ret = hwfq_remove_tenant(scheduler, tenant_ids[0]);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to remove tenant");

    ret = hwfq_add_tenant(scheduler, &alloc, &extra_tenant);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should succeed after removing a tenant");

    printf("    After removal, successfully added replacement tenant\n");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Max Flows Per Tenant Limit
// ============================================================================

void test_max_flows_per_tenant(void) {
    // Note: Flow ID 0 is reserved, so max_flows_per_tenant should be
    // at least 1 larger than the number of flows you want to configure.
    // With max_flows_per_tenant=50, valid flow IDs are 1-49 (49 flows).
    #define TEST_MAX_FLOWS 50

    hwfq_config_t config = {
        .max_tenants = 10,
        .max_flows_per_tenant = TEST_MAX_FLOWS,
        .max_total_flows = 1000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flows - valid flow IDs are 1 to (max_flows_per_tenant - 1) since
    // flow ID 0 is reserved and flow ID >= max_flows_per_tenant exceeds array bounds
    int num_flows = TEST_MAX_FLOWS - 1;  // Valid flow IDs: 1 to 49
    for (int f = 1; f <= num_flows; f++) {
        ret = hwfq_configure_flow(scheduler, tenant_id, f, &alloc);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to configure flow");
    }

    printf("    Successfully configured %d flows (IDs 1-%d)\n", num_flows, num_flows);

    // Verify they work
    for (int f = 1; f <= num_flows; f++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        ret = hwfq_enqueue(scheduler, tenant_id, f, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue to configured flow");
    }

    printf("    Successfully enqueued to all %d flows\n", num_flows);

    // Verify flow ID at max_flows_per_tenant is rejected
    ret = hwfq_configure_flow(scheduler, tenant_id, TEST_MAX_FLOWS, &alloc);
    TEST_ASSERT(ret != HWFQ_SUCCESS, "Flow ID at max limit should be rejected");
    printf("    Flow ID %d correctly rejected (at limit)\n", TEST_MAX_FLOWS);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Max Total Flows Limit
// ============================================================================

void test_max_total_flows(void) {
    // This test verifies the behavior when approaching total flow limits
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 500,  // Lower limit for testing
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    // Add multiple tenants each with some flows
    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };

    int total_flows_configured = 0;
    for (int t = 0; t < 10; t++) {
        hwfq_tenant_id_t tenant_id;
        ret = hwfq_add_tenant(scheduler, &alloc, &tenant_id);
        if (ret != HWFQ_SUCCESS) break;

        for (int f = 1; f <= 50; f++) {
            ret = hwfq_configure_flow(scheduler, tenant_id, f, &alloc);
            if (ret == HWFQ_SUCCESS) {
                total_flows_configured++;
            }
        }
    }

    printf("    Configured %d total flows (limit: %d)\n", total_flows_configured, 500);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Zero Work Size
// ============================================================================

void test_zero_work_size(void) {
    hwfq_config_t config = {
        .max_tenants = 10,
        .max_flows_per_tenant = 100,
        .max_total_flows = 1000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tenant_id;
    hwfq_add_tenant(scheduler, &alloc, &tenant_id);

    // Try to enqueue with work_size = 0
    hwfq_session_t work = { .user_data = NULL, .work_size = 0, .timestamp = 0 };
    ret = hwfq_enqueue(scheduler, tenant_id, 1, &work);

    // Should be rejected
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Zero work size should be rejected");

    printf("    Zero work size correctly rejected\n");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Max Work Size
// ============================================================================

void test_max_work_size(void) {
    hwfq_config_t config = {
        .max_tenants = 10,
        .max_flows_per_tenant = 100,
        .max_total_flows = 1000,
        .total_capacity = UINT64_MAX,  // Max capacity to allow large work sizes
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tenant_id;
    hwfq_add_tenant(scheduler, &alloc, &tenant_id);

    // Enqueue with very large work size (not UINT64_MAX to avoid overflow)
    hwfq_session_t work = { .user_data = NULL, .work_size = UINT64_MAX / 2, .timestamp = 0 };
    ret = hwfq_enqueue(scheduler, tenant_id, 1, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Large work size should be accepted");

    // Dequeue
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Dequeue should succeed");
    TEST_ASSERT(work_out.work_size == UINT64_MAX / 2, "Work size should match");
    hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());

    printf("    Large work size (UINT64_MAX/2) handled correctly\n");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Rapid Empty/Full Transitions
// ============================================================================

void test_rapid_empty_full(void) {
    hwfq_config_t config = {
        .max_tenants = 10,
        .max_flows_per_tenant = 100,
        .max_total_flows = 1000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tenant_id;
    hwfq_add_tenant(scheduler, &alloc, &tenant_id);

    #define RAPID_CYCLES 1000

    for (int i = 0; i < RAPID_CYCLES; i++) {
        // Fill with 10 items
        for (int j = 0; j < 10; j++) {
            hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
            ret = hwfq_enqueue(scheduler, tenant_id, 1, &work);
            TEST_ASSERT(ret == HWFQ_SUCCESS, "Enqueue should succeed");
        }

        // Empty completely
        for (int j = 0; j < 10; j++) {
            hwfq_session_t work_out;
            hwfq_tenant_id_t tid_out;
            hwfq_flow_id_t fid_out;
            ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
            TEST_ASSERT(ret == HWFQ_SUCCESS, "Dequeue should succeed");
            hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
        }

        // Verify empty
        hwfq_session_t work_out;
        ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
        TEST_ASSERT(ret == HWFQ_ERR_NO_WORK, "Should be empty");
    }

    printf("    Completed %d fill/drain cycles successfully\n", RAPID_CYCLES);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Single Session Fairness (Ordering)
// ============================================================================

void test_single_session_fairness(void) {
    hwfq_config_t config = {
        .max_tenants = 10,
        .max_flows_per_tenant = 100,
        .max_total_flows = 1000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    // Create group scheduler directly for testing
    group_scheduler_t *gs = scheduler->system_scheduler;

    // Configure entries with different rates (higher rate = lower finish time)
    group_entry_config_t config0 = {
        .entry_id = 0,
        .allocation = { .allocation_type = HWFQ_ALLOCATION_RATE, .rate = 100000000ULL }  // 100 MB/s
    };
    group_entry_config_t config1 = {
        .entry_id = 1,
        .allocation = { .allocation_type = HWFQ_ALLOCATION_RATE, .rate = 200000000ULL }  // 200 MB/s
    };
    group_entry_config_t config2 = {
        .entry_id = 2,
        .allocation = { .allocation_type = HWFQ_ALLOCATION_RATE, .rate = 300000000ULL }  // 300 MB/s
    };

    group_scheduler_configure_entry(gs, &config0);
    group_scheduler_configure_entry(gs, &config1);
    group_scheduler_configure_entry(gs, &config2);

    // Enqueue one session to each entry (same size)
    group_scheduler_enqueue(gs, 0, 1024, NULL, NULL);
    group_scheduler_enqueue(gs, 1, 1024, NULL, NULL);
    group_scheduler_enqueue(gs, 2, 1024, NULL, NULL);

    // Dequeue order should be: highest rate first (entry 2, then 1, then 0)
    session_state_t *s1 = group_scheduler_dequeue(gs);
    session_state_t *s2 = group_scheduler_dequeue(gs);
    session_state_t *s3 = group_scheduler_dequeue(gs);

    TEST_ASSERT(s1 != NULL && s2 != NULL && s3 != NULL, "All sessions should dequeue");

    printf("    Dequeue order: %u, %u, %u (expected: 2, 1, 0 for rate order)\n",
           s1->entry_id, s2->entry_id, s3->entry_id);

    // Higher rate should finish first
    TEST_ASSERT(s1->entry_id == 2, "Highest rate entry should dequeue first");
    TEST_ASSERT(s2->entry_id == 1, "Middle rate entry should dequeue second");
    TEST_ASSERT(s3->entry_id == 0, "Lowest rate entry should dequeue last");

    free(s1);
    free(s2);
    free(s3);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Invalid Parameters
// ============================================================================

void test_invalid_parameters(void) {
    hwfq_config_t config = {
        .max_tenants = 10,
        .max_flows_per_tenant = 100,
        .max_total_flows = 1000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tenant_id;
    hwfq_add_tenant(scheduler, &alloc, &tenant_id);

    // NULL scheduler
    hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
    ret = hwfq_enqueue(NULL, tenant_id, 1, &work);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "NULL scheduler should fail");

    // NULL work
    ret = hwfq_enqueue(scheduler, tenant_id, 1, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "NULL work should fail");

    // Invalid tenant ID
    ret = hwfq_enqueue(scheduler, 99999, 1, &work);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Invalid tenant ID should fail");

    // Reserved flow ID
    ret = hwfq_enqueue(scheduler, tenant_id, HWFQ_FLOW_ID_RESERVED, &work);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Reserved flow ID should fail");

    // Non-existent tenant
    ret = hwfq_enqueue(scheduler, tenant_id + 1, 1, &work);
    TEST_ASSERT(ret == HWFQ_ERR_NOT_FOUND, "Non-existent tenant should fail");

    // NULL dequeue output
    ret = hwfq_dequeue(scheduler, NULL, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "NULL dequeue output should fail");

    printf("    All invalid parameter cases handled correctly\n");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Edge Case Tests ===\n\n");

    printf("Running test_max_tenants...\n");
    test_max_tenants();

    printf("\nRunning test_max_flows_per_tenant...\n");
    test_max_flows_per_tenant();

    printf("\nRunning test_max_total_flows...\n");
    test_max_total_flows();

    printf("\nRunning test_zero_work_size...\n");
    test_zero_work_size();

    printf("\nRunning test_max_work_size...\n");
    test_max_work_size();

    printf("\nRunning test_rapid_empty_full...\n");
    test_rapid_empty_full();

    printf("\nRunning test_single_session_fairness...\n");
    test_single_session_fairness();

    printf("\nRunning test_invalid_parameters...\n");
    test_invalid_parameters();

    printf("\n=== Edge Case Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
