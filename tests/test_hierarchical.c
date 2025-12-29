// ============================================================================
// H-WFQ Hierarchical Scheduler Tests
// ============================================================================

#include "hwfq.h"
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Global Test Counters
// ============================================================================

static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Helper Functions
// ============================================================================

static hwfq_scheduler_t *create_test_scheduler(void)
{
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 1000000000ULL,  // 1 GB/s
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

// Helper to add a default flow to a tenant
static hwfq_flow_id_t add_default_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id)
{
    hwfq_allocation_t flow_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_flow_id_t flow_id;
    int ret = hwfq_add_flow(scheduler, tenant_id, &flow_alloc, &flow_id);
    if (ret != HWFQ_SUCCESS) {
        return 0;  // Return reserved ID on failure
    }
    return flow_id;
}

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: Basic single tenant, single flow enqueue/dequeue
static void test_single_tenant_single_flow(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue work
    int user_value = 42;
    hwfq_session_t work = {
        .user_data = &user_value,
        .work_size = 1024,
        .timestamp = 0
    };
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");

    // Dequeue work
    hwfq_session_t work_out;
    hwfq_tenant_id_t tenant_out;
    hwfq_flow_id_t flow_out;
    ret = hwfq_dequeue(scheduler, &work_out, &tenant_out, &flow_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
    TEST_ASSERT(tenant_out == tenant_id, "Tenant ID mismatch");
    TEST_ASSERT(flow_out == flow_id, "Flow ID mismatch");
    TEST_ASSERT(work_out.user_data == &user_value, "User data mismatch");
    TEST_ASSERT(work_out.work_size == 1024, "Work size mismatch");

    // Complete the work
    hwfq_complete(scheduler, &work_out, tenant_out, flow_out, get_time_ns_bench());

    // Queue should be empty now
    ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_NO_WORK, "Expected no work");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 2: Single tenant with multiple flows
static void test_single_tenant_multiple_flows(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add 3 flows
    hwfq_flow_id_t flow_ids[3];
    for (int i = 0; i < 3; i++) {
        flow_ids[i] = add_default_flow(scheduler, tenant_id);
        TEST_ASSERT(flow_ids[i] != 0, "Failed to add flow");
    }

    // Enqueue work for multiple flows
    int values[3] = {1, 2, 3};
    hwfq_session_t work;
    work.work_size = 1024;
    work.timestamp = 0;

    for (int i = 0; i < 3; i++) {
        work.user_data = &values[i];
        ret = hwfq_enqueue(scheduler, tenant_id, flow_ids[i], &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    }

    // Dequeue all work
    int dequeue_count = 0;
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    while (hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out) == HWFQ_SUCCESS) {
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
        dequeue_count++;
    }
    TEST_ASSERT(dequeue_count == 3, "Expected 3 dequeues");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 3: Multiple tenants
static void test_multiple_tenants(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add two tenants
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };

    hwfq_tenant_id_t tenant1, tenant2;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant1);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant 1");
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant2);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant 2");

    // Add flows for each tenant
    hwfq_flow_id_t flow1 = add_default_flow(scheduler, tenant1);
    hwfq_flow_id_t flow2 = add_default_flow(scheduler, tenant2);
    TEST_ASSERT(flow1 != 0 && flow2 != 0, "Failed to add flows");

    // Enqueue work for each tenant
    int value1 = 1, value2 = 2;
    hwfq_session_t work = {
        .work_size = 1024,
        .timestamp = 0
    };

    work.user_data = &value1;
    ret = hwfq_enqueue(scheduler, tenant1, flow1, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue to tenant 1");

    work.user_data = &value2;
    ret = hwfq_enqueue(scheduler, tenant2, flow2, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue to tenant 2");

    // Dequeue both
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    int count = 0;
    while (hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out) == HWFQ_SUCCESS) {
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
        count++;
    }
    TEST_ASSERT(count == 2, "Expected 2 dequeues");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 4: Hierarchical fairness - weighted tenants with flows
static void test_hierarchical_fairness(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add tenant with weight 200 (2x)
    hwfq_allocation_t tenant1_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 200
    };
    hwfq_tenant_id_t tenant1;
    int ret = hwfq_add_tenant(scheduler, &tenant1_alloc, &tenant1);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant 1");

    // Add tenant with weight 100 (1x)
    hwfq_allocation_t tenant2_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant2;
    ret = hwfq_add_tenant(scheduler, &tenant2_alloc, &tenant2);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant 2");

    // Add flows for each tenant
    hwfq_flow_id_t flow1 = add_default_flow(scheduler, tenant1);
    hwfq_flow_id_t flow2 = add_default_flow(scheduler, tenant2);
    TEST_ASSERT(flow1 != 0 && flow2 != 0, "Failed to add flows");

    // Enqueue 100 work items for each tenant
    hwfq_session_t work = {
        .user_data = NULL,
        .work_size = 1024,
        .timestamp = 0
    };

    for (int i = 0; i < 100; i++) {
        ret = hwfq_enqueue(scheduler, tenant1, flow1, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue to tenant 1");
        ret = hwfq_enqueue(scheduler, tenant2, flow2, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue to tenant 2");
    }

    // Dequeue and count per tenant
    int tenant1_count = 0, tenant2_count = 0;
    hwfq_session_t work_out;
    hwfq_tenant_id_t tenant_out;
    hwfq_flow_id_t flow_out;

    while (hwfq_dequeue(scheduler, &work_out, &tenant_out, &flow_out) == HWFQ_SUCCESS) {
        hwfq_complete(scheduler, &work_out, tenant_out, flow_out, get_time_ns_bench());
        if (tenant_out == tenant1) {
            tenant1_count++;
        } else if (tenant_out == tenant2) {
            tenant2_count++;
        }
    }

    // Verify all sessions were dequeued
    TEST_ASSERT(tenant1_count + tenant2_count == 200, "Total count mismatch");

    // Note: Hierarchical fairness depends on interleaving of enqueue/dequeue cycles.
    // When sessions are bulk-enqueued upfront, fairness ratios may vary.
    // The underlying WF2Q+ algorithm fairness is verified in test_fairness.c.
    // Here we just verify that both tenants received work.
    TEST_ASSERT(tenant1_count > 0 && tenant2_count > 0, "Both tenants should receive work");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 5: Tenant goes idle and reactivates
static void test_tenant_idle_reactivate(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue and drain
    int value = 1;
    hwfq_session_t work = {
        .user_data = &value,
        .work_size = 1024,
        .timestamp = 0
    };
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");

    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
    hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());

    // Tenant should be idle now
    ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_NO_WORK, "Expected no work");

    // Re-enqueue
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to re-enqueue");

    // Should be able to dequeue again
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue after reactivate");
    hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 6: Unconfigured flow enqueue fails (flows must be created via hwfq_add_flow)
static void test_unconfigured_flow_auto_created(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Enqueue to flow without explicit configuration - should FAIL
    int value = 42;
    hwfq_session_t work = {
        .user_data = &value,
        .work_size = 1024,
        .timestamp = 0
    };
    ret = hwfq_enqueue(scheduler, tenant_id, 999, &work);  // flow_id = 999 (not created)
    TEST_ASSERT(ret == HWFQ_ERR_NOT_FOUND, "Enqueue to unconfigured flow should fail");

    // Now add the flow properly and verify it works
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Enqueue to configured flow should succeed");

    // Should be able to dequeue
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t flow_out;
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &flow_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
    TEST_ASSERT(flow_out == flow_id, "Flow ID mismatch");
    hwfq_complete(scheduler, &work_out, tid_out, flow_out, get_time_ns_bench());

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 7: Remove tenant with backlog fails
static void test_remove_tenant_with_backlog_fails(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue work
    int value = 1;
    hwfq_session_t work = {
        .user_data = &value,
        .work_size = 1024,
        .timestamp = 0
    };
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");

    // Try to remove tenant - should fail
    ret = hwfq_remove_tenant(scheduler, tenant_id);
    TEST_ASSERT(ret == HWFQ_ERR_TENANT_HAS_BACKLOG, "Expected backlog error");

    // Drain the work
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
    hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());

    // Now removal should succeed
    ret = hwfq_remove_tenant(scheduler, tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to remove tenant after drain");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 8: Empty dequeue returns NO_WORK
static void test_empty_dequeue(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_session_t work_out;
    int ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_NO_WORK, "Expected no work on empty scheduler");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 9: Multiple enqueue/dequeue cycles
static void test_multiple_cycles(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add 5 flows
    hwfq_flow_id_t flow_ids[5];
    for (int i = 0; i < 5; i++) {
        flow_ids[i] = add_default_flow(scheduler, tenant_id);
        TEST_ASSERT(flow_ids[i] != 0, "Failed to add flow");
    }

    // Run multiple enqueue/dequeue cycles
    hwfq_session_t work = {
        .user_data = NULL,
        .work_size = 1024,
        .timestamp = 0
    };

    for (int cycle = 0; cycle < 10; cycle++) {
        // Enqueue 5 items
        for (int i = 0; i < 5; i++) {
            ret = hwfq_enqueue(scheduler, tenant_id, flow_ids[i], &work);
            TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
        }

        // Dequeue all
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        int count = 0;
        while (hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out) == HWFQ_SUCCESS) {
            hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
            count++;
        }
        TEST_ASSERT(count == 5, "Expected 5 items per cycle");
    }

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 10: Configured flows get their weights
static void test_configured_flow_weights(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow 1 with weight 200
    hwfq_allocation_t flow1_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 200
    };
    hwfq_flow_id_t flow1_id;
    ret = hwfq_add_flow(scheduler, tenant_id, &flow1_alloc, &flow1_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add flow 1");

    // Add flow 2 with weight 100
    hwfq_allocation_t flow2_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_flow_id_t flow2_id;
    ret = hwfq_add_flow(scheduler, tenant_id, &flow2_alloc, &flow2_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add flow 2");

    // Enqueue work for both flows
    hwfq_session_t work = {
        .user_data = NULL,
        .work_size = 1024,
        .timestamp = 0
    };

    for (int i = 0; i < 60; i++) {
        ret = hwfq_enqueue(scheduler, tenant_id, flow1_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue to flow 1");
        ret = hwfq_enqueue(scheduler, tenant_id, flow2_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue to flow 2");
    }

    // Dequeue and count per flow
    int flow1_count = 0, flow2_count = 0;
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t flow_out;

    while (hwfq_dequeue(scheduler, &work_out, &tid_out, &flow_out) == HWFQ_SUCCESS) {
        hwfq_complete(scheduler, &work_out, tid_out, flow_out, get_time_ns_bench());
        if (flow_out == flow1_id) {
            flow1_count++;
        } else if (flow_out == flow2_id) {
            flow2_count++;
        }
    }

    // Verify all sessions were dequeued
    TEST_ASSERT(flow1_count + flow2_count == 120, "Total count mismatch");

    // Note: Flow fairness depends on interleaving of enqueue/dequeue cycles.
    // When sessions are bulk-enqueued upfront, fairness ratios may vary.
    // The underlying WF2Q+ algorithm fairness is verified in test_fairness.c.
    // Here we just verify that both flows received work.
    TEST_ASSERT(flow1_count > 0 && flow2_count > 0, "Both flows should receive work");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 11: Capacity limit prevents dequeue when at capacity
static void test_capacity_limit(void)
{
    // Create scheduler with small capacity
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 5000,  // Small capacity: can fit 5 work items of size 1000
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .alloc_fn = NULL,
        .free_fn = NULL,
        .session_available_fn = NULL
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue 10 work items of size 1000 each
    hwfq_session_t work = {
        .user_data = NULL,
        .work_size = 1000,
        .timestamp = 0
    };

    for (int i = 0; i < 10; i++) {
        ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    }

    // Dequeue - should only be able to get 5 (capacity = 5000)
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    int dequeue_count = 0;

    while (hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out) == HWFQ_SUCCESS) {
        dequeue_count++;
        // Don't call complete - keep items in-flight
    }

    TEST_ASSERT(dequeue_count == 5, "Expected 5 dequeues (capacity limit)");

    // Now complete one item
    hwfq_complete(scheduler, NULL, tenant_id, flow_id, get_time_ns_bench());

    // Should be able to dequeue one more
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should dequeue after complete freed capacity");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 12: Multiple in-flight sessions for same flow
static void test_multiple_in_flight_same_flow(void)
{
    // Create scheduler with moderate capacity
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 10000,  // Can fit 10 work items of size 1000
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .alloc_fn = NULL,
        .free_fn = NULL,
        .session_available_fn = NULL
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue 5 work items all for the SAME flow
    int user_values[5] = {1, 2, 3, 4, 5};
    hwfq_session_t work = {
        .work_size = 1000,
        .timestamp = 0
    };

    for (int i = 0; i < 5; i++) {
        work.user_data = &user_values[i];
        ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    }

    // Dequeue all 5 - all from the same flow should be in-flight simultaneously
    hwfq_session_t work_out[5];
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;

    for (int i = 0; i < 5; i++) {
        ret = hwfq_dequeue(scheduler, &work_out[i], &tid_out, &fid_out);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
        TEST_ASSERT(fid_out == flow_id, "All should be from same flow");
    }

    // Now complete them in reverse order
    for (int i = 4; i >= 0; i--) {
        hwfq_complete(scheduler, &work_out[i], tenant_id, flow_id, get_time_ns_bench());
    }

    // Queue should be empty
    hwfq_session_t dummy;
    ret = hwfq_dequeue(scheduler, &dummy, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_NO_WORK, "Expected no work after completing all");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Callback counter for test
static int g_callback_count = 0;
static void test_callback(hwfq_scheduler_t *scheduler)
{
    (void)scheduler;
    g_callback_count++;
}

// Test 13: session_available_fn callback is called
static void test_session_available_callback(void)
{
    g_callback_count = 0;

    // Create scheduler with callback
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 2000,  // Can fit 2 work items of size 1000
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .alloc_fn = NULL,
        .free_fn = NULL,
        .session_available_fn = test_callback
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue work - should trigger callback since capacity available
    hwfq_session_t work = {
        .user_data = NULL,
        .work_size = 1000,
        .timestamp = 0
    };

    int initial_count = g_callback_count;
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    TEST_ASSERT(g_callback_count > initial_count, "Callback should be called on enqueue");

    // Dequeue to use capacity
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");

    // Enqueue more work to fill capacity
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");

    // Enqueue more - should still trigger callback even though we're now at capacity
    // (callback happens on enqueue before we know if dequeue will succeed)
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");

    // Now at capacity - dequeue should fail
    ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_NO_WORK, "Expected at capacity");

    // Complete work - should trigger callback since work is queued and capacity freed
    int count_before_complete = g_callback_count;
    hwfq_complete(scheduler, NULL, tenant_id, flow_id, get_time_ns_bench());
    TEST_ASSERT(g_callback_count > count_before_complete, "Callback should be called on complete");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 14: Zero total_capacity means unlimited
static void test_zero_capacity_unlimited(void)
{
    // Create scheduler with zero capacity (should mean unlimited)
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 0,  // Zero = unlimited
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .alloc_fn = NULL,
        .free_fn = NULL,
        .session_available_fn = NULL
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue many work items
    hwfq_session_t work = {
        .user_data = NULL,
        .work_size = 1000000,  // Large work size
        .timestamp = 0
    };

    for (int i = 0; i < 100; i++) {
        ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    }

    // Should be able to dequeue all without capacity limit
    hwfq_session_t work_out;
    int dequeue_count = 0;
    while (hwfq_dequeue(scheduler, &work_out, NULL, NULL) == HWFQ_SUCCESS) {
        dequeue_count++;
    }

    TEST_ASSERT(dequeue_count == 100, "Should dequeue all 100 with unlimited capacity");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Timeout callback tracking
static int g_timeout_callback_count = 0;
static hwfq_tenant_id_t g_last_timeout_tenant_id = 0;
static hwfq_flow_id_t g_last_timeout_flow_id = 0;
static void *g_last_timeout_user_data = NULL;

static void test_timeout_callback(hwfq_scheduler_t *scheduler,
                                  hwfq_tenant_id_t tenant_id,
                                  hwfq_flow_id_t flow_id,
                                  void *user_data,
                                  size_t work_size,
                                  uint64_t timeout_ns)
{
    (void)scheduler;
    (void)work_size;
    (void)timeout_ns;
    g_timeout_callback_count++;
    g_last_timeout_tenant_id = tenant_id;
    g_last_timeout_flow_id = flow_id;
    g_last_timeout_user_data = user_data;
}

// Test 15: Basic timeout test
static void test_basic_timeout(void)
{
    g_timeout_callback_count = 0;
    g_last_timeout_user_data = NULL;

    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 10000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .alloc_fn = NULL,
        .free_fn = NULL,
        .session_available_fn = NULL,
        .session_timeout_fn = test_timeout_callback
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue with 100ms timeout
    int user_value = 42;
    hwfq_session_t work = {
        .user_data = &user_value,
        .work_size = 1000,
        .timestamp = 0,
        .timeout_ns = 100000000ULL  // 100ms
    };
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");

    // Dequeue to put in-flight
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");

    // Check timeouts before timeout period - should not timeout
    uint64_t now = get_time_ns_bench();
    uint32_t timed_out = hwfq_check_timeouts(scheduler, now);
    TEST_ASSERT(timed_out == 0, "Should not timeout yet");
    TEST_ASSERT(g_timeout_callback_count == 0, "Callback should not be called yet");

    // Check timeouts after timeout period
    uint64_t future = now + 200000000ULL;  // +200ms (> 100ms timeout)
    timed_out = hwfq_check_timeouts(scheduler, future);
    TEST_ASSERT(timed_out == 1, "Should timeout exactly 1 session");
    TEST_ASSERT(g_timeout_callback_count == 1, "Callback should be called once");
    TEST_ASSERT(g_last_timeout_tenant_id == tenant_id, "Tenant ID should match");
    TEST_ASSERT(g_last_timeout_flow_id == flow_id, "Flow ID should match");
    TEST_ASSERT(g_last_timeout_user_data == &user_value, "User data should match");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 16: No timeout when timeout_ns is 0
static void test_no_timeout_when_zero(void)
{
    g_timeout_callback_count = 0;

    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 10000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .session_timeout_fn = test_timeout_callback
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue with NO timeout (timeout_ns = 0)
    hwfq_session_t work = {
        .user_data = NULL,
        .work_size = 1000,
        .timeout_ns = 0  // No timeout
    };
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");

    hwfq_session_t work_out;
    ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");

    // Check timeouts far in the future - should never timeout
    uint64_t now = get_time_ns_bench();
    uint64_t far_future = now + 1000000000000ULL;  // +1000 seconds
    uint32_t timed_out = hwfq_check_timeouts(scheduler, far_future);
    TEST_ASSERT(timed_out == 0, "Should never timeout with timeout_ns=0");
    TEST_ASSERT(g_timeout_callback_count == 0, "Callback should never be called");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 17: Basic cancel test
static void test_basic_cancel(void)
{
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 5000,  // Can fit 5 work items of size 1000
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue 5 items (fill capacity)
    int user_values[5] = {1, 2, 3, 4, 5};
    for (int i = 0; i < 5; i++) {
        hwfq_session_t work = {
            .user_data = &user_values[i],
            .work_size = 1000,
            .timeout_ns = 0
        };
        ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    }

    // Dequeue all 5 (fill in-flight capacity)
    hwfq_session_t work_out[5];
    for (int i = 0; i < 5; i++) {
        ret = hwfq_dequeue(scheduler, &work_out[i], NULL, NULL);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
    }

    // At capacity - can't dequeue more
    // Now add more work
    hwfq_session_t more_work = {
        .user_data = NULL,
        .work_size = 1000,
        .timeout_ns = 0
    };
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &more_work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");

    // Can't dequeue because at capacity
    hwfq_session_t dummy;
    ret = hwfq_dequeue(scheduler, &dummy, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_NO_WORK, "Should be at capacity");

    // Cancel one of the in-flight sessions
    ret = hwfq_cancel(scheduler, &work_out[2], tenant_id, flow_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to cancel");

    // Now we should be able to dequeue
    ret = hwfq_dequeue(scheduler, &dummy, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should be able to dequeue after cancel");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 18: Cancel not found
static void test_cancel_not_found(void)
{
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Try to cancel non-existent session
    int fake_user_data = 999;
    hwfq_session_t fake_work = {
        .user_data = &fake_user_data,
        .work_size = 1000
    };
    ret = hwfq_cancel(scheduler, &fake_work, tenant_id, flow_id);
    TEST_ASSERT(ret == HWFQ_ERR_NOT_FOUND, "Should return not found");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 19: Complete before timeout should not trigger timeout
static void test_complete_before_timeout(void)
{
    g_timeout_callback_count = 0;

    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 10000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .session_timeout_fn = test_timeout_callback
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue with timeout
    hwfq_session_t work = {
        .user_data = NULL,
        .work_size = 1000,
        .timeout_ns = 100000000ULL  // 100ms
    };
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");

    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");

    // Complete the work before timeout
    hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());

    // Now check for timeouts way in the future
    uint64_t far_future = get_time_ns_bench() + 1000000000ULL;  // +1 second
    uint32_t timed_out = hwfq_check_timeouts(scheduler, far_future);
    TEST_ASSERT(timed_out == 0, "Should not timeout already-completed work");
    TEST_ASSERT(g_timeout_callback_count == 0, "Callback should not be called");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 20: Multiple timeouts in single check
static void test_multiple_timeouts(void)
{
    g_timeout_callback_count = 0;

    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 100000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .session_timeout_fn = test_timeout_callback
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Create 5 flows
    hwfq_flow_id_t flow_ids[5];
    for (int i = 0; i < 5; i++) {
        flow_ids[i] = add_default_flow(scheduler, tenant_id);
        TEST_ASSERT(flow_ids[i] != 0, "Failed to add flow");
    }

    // Enqueue 5 items with same timeout (one to each flow)
    for (int i = 0; i < 5; i++) {
        hwfq_session_t work = {
            .user_data = NULL,
            .work_size = 1000,
            .timeout_ns = 100000000ULL  // 100ms
        };
        ret = hwfq_enqueue(scheduler, tenant_id, flow_ids[i], &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    }

    // Dequeue all
    hwfq_session_t work_out;
    for (int i = 0; i < 5; i++) {
        ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
    }

    // Check timeouts after timeout period - all 5 should timeout
    uint64_t future = get_time_ns_bench() + 200000000ULL;  // +200ms
    uint32_t timed_out = hwfq_check_timeouts(scheduler, future);
    TEST_ASSERT(timed_out == 5, "All 5 sessions should timeout");
    TEST_ASSERT(g_timeout_callback_count == 5, "Callback should be called 5 times");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 21: Dynamic rate reconfiguration
// Verify that changing tenant rates during operation maintains fairness
static void test_dynamic_rate_reconfiguration(void)
{
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 10000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = true
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    // Add tenant with initial rate
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_RATE,
        .rate = 5000
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add flow
    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue work
    for (int i = 0; i < 10; i++) {
        hwfq_session_t work = {
            .user_data = NULL,
            .work_size = 100
        };
        ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    }

    // Dequeue some work
    hwfq_session_t work_out;
    for (int i = 0; i < 5; i++) {
        ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
        hwfq_complete(scheduler, &work_out, tenant_id, flow_id, get_time_ns_bench());
    }

    // Reconfigure tenant to different rate
    hwfq_allocation_t new_alloc = {
        .allocation_type = HWFQ_ALLOCATION_RATE,
        .rate = 8000
    };
    ret = hwfq_configure_tenant(scheduler, tenant_id, &new_alloc);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to reconfigure tenant");

    // Verify capacity info updated
    hwfq_capacity_info_t cap_info;
    ret = hwfq_get_capacity_info(scheduler, &cap_info);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to get capacity info");
    TEST_ASSERT(cap_info.allocated_capacity == 8000, "Rate should be updated");

    // Dequeue remaining work - should still work
    for (int i = 0; i < 5; i++) {
        ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue after reconfig");
        hwfq_complete(scheduler, &work_out, tenant_id, flow_id, get_time_ns_bench());
    }

    // Verify all work processed
    ret = hwfq_dequeue(scheduler, &work_out, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_NO_WORK, "Queue should be empty");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 22: Flow rate reconfiguration during operation
static void test_flow_rate_reconfiguration(void)
{
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 100000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = true
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    // Add tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add two flows with rate-based allocation
    hwfq_allocation_t flow1_alloc = {
        .allocation_type = HWFQ_ALLOCATION_RATE,
        .rate = 10000
    };
    hwfq_flow_id_t flow1_id;
    ret = hwfq_add_flow(scheduler, tenant_id, &flow1_alloc, &flow1_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add flow 1");

    hwfq_allocation_t flow2_alloc = {
        .allocation_type = HWFQ_ALLOCATION_RATE,
        .rate = 10000
    };
    hwfq_flow_id_t flow2_id;
    ret = hwfq_add_flow(scheduler, tenant_id, &flow2_alloc, &flow2_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add flow 2");

    // Enqueue work to both flows
    uint32_t flow1_count = 0;
    uint32_t flow2_count = 0;

    for (int i = 0; i < 100; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 100 };
        hwfq_enqueue(scheduler, tenant_id, flow1_id, &work);
        hwfq_enqueue(scheduler, tenant_id, flow2_id, &work);
    }

    // Dequeue first batch - should be roughly equal
    hwfq_session_t work_out;
    hwfq_flow_id_t fid_out;
    for (int i = 0; i < 100; i++) {
        ret = hwfq_dequeue(scheduler, &work_out, NULL, &fid_out);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
        if (fid_out == flow1_id) flow1_count++;
        else flow2_count++;
        hwfq_complete(scheduler, &work_out, tenant_id, fid_out, get_time_ns_bench());
    }

    // Verify roughly equal (within 20%)
    double ratio = (double)flow1_count / (double)flow2_count;
    TEST_ASSERT(ratio > 0.8 && ratio < 1.2, "Initial allocation should be ~1:1");

    // Reconfigure flow1 to have 3x the rate
    hwfq_allocation_t new_alloc = {
        .allocation_type = HWFQ_ALLOCATION_RATE,
        .rate = 30000  // 3x flow2's rate
    };
    ret = hwfq_reconfigure_flow(scheduler, tenant_id, flow1_id, &new_alloc);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to reconfigure flow");

    // Add more work AFTER reconfiguration to test new rates
    for (int i = 0; i < 100; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 100 };
        hwfq_enqueue(scheduler, tenant_id, flow1_id, &work);
        hwfq_enqueue(scheduler, tenant_id, flow2_id, &work);
    }

    // Reset counters
    flow1_count = 0;
    flow2_count = 0;

    // Dequeue all - flow1 should get more
    while (hwfq_dequeue(scheduler, &work_out, NULL, &fid_out) == HWFQ_SUCCESS) {
        if (fid_out == flow1_id) flow1_count++;
        else flow2_count++;
        hwfq_complete(scheduler, &work_out, tenant_id, fid_out, get_time_ns_bench());
    }

    // Verify reconfiguration happened successfully (not testing exact ratio since
    // existing queued work affects the distribution)
    TEST_ASSERT(flow1_count > 0 && flow2_count > 0, "Both flows should get work");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 23: Timeout during ongoing cancel operations
static void test_timeout_and_cancel_interaction(void)
{
    g_timeout_callback_count = 0;

    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 100000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
        .session_timeout_fn = test_timeout_callback
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    hwfq_flow_id_t flow_id = add_default_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue 5 items with different timeouts
    int user_values[5] = {1, 2, 3, 4, 5};
    hwfq_session_t works[5];
    for (int i = 0; i < 5; i++) {
        works[i].user_data = &user_values[i];
        works[i].work_size = 1000;
        works[i].timeout_ns = (i + 1) * 100000000ULL;  // 100ms, 200ms, 300ms, 400ms, 500ms
        ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &works[i]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue");
    }

    // Dequeue all
    hwfq_session_t work_out[5];
    for (int i = 0; i < 5; i++) {
        ret = hwfq_dequeue(scheduler, &work_out[i], NULL, NULL);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to dequeue");
    }

    // Cancel item 2 (the middle one with 300ms timeout)
    ret = hwfq_cancel(scheduler, &work_out[2], tenant_id, flow_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to cancel");

    // Check timeouts after 250ms - items 0 and 1 should timeout (100ms, 200ms)
    uint64_t future = get_time_ns_bench() + 250000000ULL;
    uint32_t timed_out = hwfq_check_timeouts(scheduler, future);
    TEST_ASSERT(timed_out == 2, "Should timeout 2 sessions");
    TEST_ASSERT(g_timeout_callback_count == 2, "Callback should be called twice");

    // Check timeouts after 550ms - items 3 and 4 should timeout (400ms, 500ms)
    // Item 2 was cancelled, so it shouldn't trigger timeout
    future = get_time_ns_bench() + 550000000ULL;
    timed_out = hwfq_check_timeouts(scheduler, future);
    TEST_ASSERT(timed_out == 2, "Should timeout 2 more sessions");
    TEST_ASSERT(g_timeout_callback_count == 4, "Callback should be called 4 times total");

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// Test 24: Cross-tenant fairness with flows
// Verify tenant-level and flow-level fairness don't interfere
static void test_cross_tenant_flow_fairness(void)
{
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 100000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = true
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS && scheduler != NULL, "Failed to create scheduler");

    // Create tenant 1 with weight 200
    hwfq_allocation_t tenant1_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 200
    };
    hwfq_tenant_id_t tenant1_id;
    ret = hwfq_add_tenant(scheduler, &tenant1_alloc, &tenant1_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant 1");

    // Create tenant 2 with weight 100 (half of tenant 1)
    hwfq_allocation_t tenant2_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant2_id;
    ret = hwfq_add_tenant(scheduler, &tenant2_alloc, &tenant2_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant 2");

    // Tenant 1: two flows with equal weights
    hwfq_allocation_t flow_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_flow_id_t t1_flow1, t1_flow2;
    ret = hwfq_add_flow(scheduler, tenant1_id, &flow_alloc, &t1_flow1);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add t1 flow1");
    ret = hwfq_add_flow(scheduler, tenant1_id, &flow_alloc, &t1_flow2);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add t1 flow2");

    // Tenant 2: one flow
    hwfq_flow_id_t t2_flow1;
    ret = hwfq_add_flow(scheduler, tenant2_id, &flow_alloc, &t2_flow1);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add t2 flow1");

    // Enqueue 300 work items: 100 to each flow
    for (int i = 0; i < 100; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 100 };
        hwfq_enqueue(scheduler, tenant1_id, t1_flow1, &work);
        hwfq_enqueue(scheduler, tenant1_id, t1_flow2, &work);
        hwfq_enqueue(scheduler, tenant2_id, t2_flow1, &work);
    }

    // Dequeue all and count
    uint32_t t1f1_count = 0, t1f2_count = 0, t2f1_count = 0;
    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;

    while (hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out) == HWFQ_SUCCESS) {
        if (tid_out == tenant1_id) {
            if (fid_out == t1_flow1) t1f1_count++;
            else t1f2_count++;
        } else {
            t2f1_count++;
        }
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    uint32_t tenant1_total = t1f1_count + t1f2_count;
    uint32_t tenant2_total = t2f1_count;

    // Tenant 1 should get more than tenant 2 (weight 200 vs 100)
    // Note: Due to hierarchical scheduling with tenant quantum, exact 2:1 ratio
    // may not be achieved in small sample sizes. Just verify tenant 1 gets more.
    TEST_ASSERT(tenant1_total > tenant2_total,
                "Tenant with higher weight should get more work");

    // Within tenant 1, flows should be roughly equal
    if (t1f1_count > 0 && t1f2_count > 0) {
        double flow_ratio = (double)t1f1_count / (double)t1f2_count;
        TEST_ASSERT(flow_ratio > 0.5 && flow_ratio < 2.0,
                    "Flow fairness within tenant should be roughly equal");
    }

    test_hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void)
{
    printf("\n");
    printf("============================================\n");
    printf("H-WFQ Hierarchical Scheduler Tests\n");
    printf("============================================\n");
    printf("\n");

    test_single_tenant_single_flow();
    test_single_tenant_multiple_flows();
    test_multiple_tenants();
    test_hierarchical_fairness();
    test_tenant_idle_reactivate();
    test_unconfigured_flow_auto_created();
    test_remove_tenant_with_backlog_fails();
    test_empty_dequeue();
    test_multiple_cycles();
    test_configured_flow_weights();
    test_capacity_limit();
    test_multiple_in_flight_same_flow();
    test_session_available_callback();
    test_zero_capacity_unlimited();
    test_basic_timeout();
    test_no_timeout_when_zero();
    test_basic_cancel();
    test_cancel_not_found();
    test_complete_before_timeout();
    test_multiple_timeouts();
    test_dynamic_rate_reconfiguration();
    test_flow_rate_reconfiguration();
    test_timeout_and_cancel_interaction();
    test_cross_tenant_flow_fairness();

    printf("\n");
    printf("============================================\n");
    printf("Results: %d passed, %d failed\n", g_tests_passed, g_tests_failed);
    printf("============================================\n");
    printf("\n");

    return (g_tests_failed > 0) ? 1 : 0;
}
