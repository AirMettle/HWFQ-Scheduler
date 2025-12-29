// ============================================================================
// Statistics API Tests for H-WFQ Scheduler
// ============================================================================
//
// - Verifies counter updates during enqueue/dequeue
// - Tests wait time tracking
// - Tests effective rate calculation
// - Tests reset functionality with wildcards
// - Verifies zero overhead when statistics disabled
//
// ============================================================================

#include "test_common.h"
#include "../include/hwfq.h"
#include "../src/hwfq_internal.h"
#include <stdlib.h>
#include <string.h>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Helper Functions
// ============================================================================

static hwfq_scheduler_t *create_scheduler_with_stats(bool enable_stats) {
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = enable_stats,
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

static hwfq_tenant_id_t add_tenant(hwfq_scheduler_t *scheduler, uint32_t weight) {
    hwfq_allocation_t alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = weight
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &alloc, &tenant_id);
    if (ret != HWFQ_SUCCESS) {
        return (hwfq_tenant_id_t)-1;
    }
    return tenant_id;
}

// ============================================================================
// Test: Stats Disabled - No Overhead
// ============================================================================

void test_stats_disabled_no_overhead(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(false);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant_id = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant_id != (hwfq_tenant_id_t)-1, "Failed to add tenant");

    hwfq_flow_id_t flow_id = test_add_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    for (int i = 0; i < 100; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        int ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Enqueue failed");
    }

    for (int i = 0; i < 100; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        int ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Dequeue failed");
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    hwfq_tenant_stats_t stats;
    int ret = hwfq_get_tenant_stats(scheduler, tenant_id, &stats);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to get tenant stats");
    TEST_ASSERT(stats.work_units_processed == 0, "Stats should be 0 when disabled");
    TEST_ASSERT(stats.operations_completed == 0, "Ops should be 0 when disabled");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Tenant Stats Basic
// ============================================================================

void test_tenant_stats_basic(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(true);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant_id = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant_id != (hwfq_tenant_id_t)-1, "Failed to add tenant");

    hwfq_flow_id_t flow_id = test_add_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    for (int i = 0; i < 10; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        int ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Enqueue failed");
    }

    for (int i = 0; i < 10; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        int ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Dequeue failed");
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    hwfq_tenant_stats_t stats;
    int ret = hwfq_get_tenant_stats(scheduler, tenant_id, &stats);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to get tenant stats");
    TEST_ASSERT(stats.work_units_processed == 10 * 1024, "work_units_processed mismatch");
    TEST_ASSERT(stats.operations_completed == 10, "operations_completed mismatch");
    TEST_ASSERT(stats.current_backlog == 0, "current_backlog should be 0");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Flow Stats Basic
// ============================================================================

void test_flow_stats_basic(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(true);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant_id = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant_id != (hwfq_tenant_id_t)-1, "Failed to add tenant");

    hwfq_allocation_t flow_alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_flow_id_t flow_id;
    int ret = hwfq_add_flow(scheduler, tenant_id, &flow_alloc, &flow_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add flow");

    for (int i = 0; i < 5; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 2048, .timestamp = 0 };
        ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Enqueue failed");
    }

    for (int i = 0; i < 5; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Dequeue failed");
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    hwfq_flow_stats_t flow_stats;
    ret = hwfq_get_flow_stats(scheduler, tenant_id, flow_id, &flow_stats);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to get flow stats");
    TEST_ASSERT(flow_stats.work_units_processed == 5 * 2048, "flow work_units mismatch");
    TEST_ASSERT(flow_stats.operations_completed == 5, "flow ops mismatch");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Wait Time Tracking
// ============================================================================

void test_wait_time_tracking(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(true);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant_id = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant_id != (hwfq_tenant_id_t)-1, "Failed to add tenant");

    hwfq_flow_id_t flow_id = test_add_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
    int ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Enqueue failed");

    msleep(10);

    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out;
    hwfq_flow_id_t fid_out;
    ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Dequeue failed");
    hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());

    hwfq_tenant_stats_t stats;
    ret = hwfq_get_tenant_stats(scheduler, tenant_id, &stats);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to get tenant stats");
    TEST_ASSERT(stats.total_wait_time_ns > 0, "Wait time should be > 0");
    TEST_ASSERT(stats.total_wait_time_ns >= 10000000ULL, "Wait time should be >= 10ms");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Effective Rate Calculation
// ============================================================================

void test_effective_rate_calculation(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(true);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant_id = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant_id != (hwfq_tenant_id_t)-1, "Failed to add tenant");

    hwfq_flow_id_t flow_id = test_add_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    for (int i = 0; i < 100; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1000, .timestamp = 0 };
        int ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Enqueue failed");

        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Dequeue failed");
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    hwfq_tenant_stats_t stats;
    int ret = hwfq_get_tenant_stats(scheduler, tenant_id, &stats);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to get tenant stats");
    TEST_ASSERT(stats.effective_rate >= 0.0, "Effective rate should be >= 0");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Reset Specific Flow
// ============================================================================

void test_reset_specific_flow(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(true);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant_id = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant_id != (hwfq_tenant_id_t)-1, "Failed to add tenant");

    hwfq_allocation_t flow_alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_flow_id_t flow1_id, flow2_id;
    hwfq_add_flow(scheduler, tenant_id, &flow_alloc, &flow1_id);
    hwfq_add_flow(scheduler, tenant_id, &flow_alloc, &flow2_id);

    for (int i = 0; i < 5; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        hwfq_enqueue(scheduler, tenant_id, flow1_id, &work);
        hwfq_enqueue(scheduler, tenant_id, flow2_id, &work);
    }

    for (int i = 0; i < 10; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    hwfq_reset_stats(scheduler, tenant_id, flow1_id);

    hwfq_flow_stats_t flow1_stats, flow2_stats;
    hwfq_get_flow_stats(scheduler, tenant_id, flow1_id, &flow1_stats);
    hwfq_get_flow_stats(scheduler, tenant_id, flow2_id, &flow2_stats);

    TEST_ASSERT(flow1_stats.work_units_processed == 0, "Flow 1 should be reset");
    TEST_ASSERT(flow2_stats.work_units_processed > 0, "Flow 2 should retain stats");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Reset All Flows
// ============================================================================

void test_reset_all_flows(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(true);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant_id = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant_id != (hwfq_tenant_id_t)-1, "Failed to add tenant");

    hwfq_allocation_t flow_alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_flow_id_t flow1_id, flow2_id;
    hwfq_add_flow(scheduler, tenant_id, &flow_alloc, &flow1_id);
    hwfq_add_flow(scheduler, tenant_id, &flow_alloc, &flow2_id);

    for (int i = 0; i < 5; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        hwfq_enqueue(scheduler, tenant_id, flow1_id, &work);
        hwfq_enqueue(scheduler, tenant_id, flow2_id, &work);
    }
    for (int i = 0; i < 10; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    hwfq_reset_stats(scheduler, tenant_id, HWFQ_ALL_FLOWS);

    hwfq_flow_stats_t flow1_stats, flow2_stats;
    hwfq_get_flow_stats(scheduler, tenant_id, flow1_id, &flow1_stats);
    hwfq_get_flow_stats(scheduler, tenant_id, flow2_id, &flow2_stats);

    TEST_ASSERT(flow1_stats.work_units_processed == 0, "Flow 1 should be reset");
    TEST_ASSERT(flow2_stats.work_units_processed == 0, "Flow 2 should be reset");

    hwfq_tenant_stats_t tenant_stats;
    hwfq_get_tenant_stats(scheduler, tenant_id, &tenant_stats);
    TEST_ASSERT(tenant_stats.work_units_processed == 0, "Tenant should be reset");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Reset All Tenants
// ============================================================================

void test_reset_all_tenants(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(true);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant1 = add_tenant(scheduler, 100);
    hwfq_tenant_id_t tenant2 = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant1 != (hwfq_tenant_id_t)-1, "Failed to add tenant1");
    TEST_ASSERT(tenant2 != (hwfq_tenant_id_t)-1, "Failed to add tenant2");

    hwfq_flow_id_t flow1_id = test_add_flow(scheduler, tenant1);
    hwfq_flow_id_t flow2_id = test_add_flow(scheduler, tenant2);
    TEST_ASSERT(flow1_id != 0, "Failed to add flow1");
    TEST_ASSERT(flow2_id != 0, "Failed to add flow2");

    for (int i = 0; i < 5; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        hwfq_enqueue(scheduler, tenant1, flow1_id, &work);
        hwfq_enqueue(scheduler, tenant2, flow2_id, &work);
    }
    for (int i = 0; i < 10; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    hwfq_reset_stats(scheduler, HWFQ_ALL_TENANTS, 0);

    hwfq_tenant_stats_t stats1, stats2;
    hwfq_get_tenant_stats(scheduler, tenant1, &stats1);
    hwfq_get_tenant_stats(scheduler, tenant2, &stats2);

    TEST_ASSERT(stats1.work_units_processed == 0, "Tenant 1 should be reset");
    TEST_ASSERT(stats2.work_units_processed == 0, "Tenant 2 should be reset");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Backlog Tracking
// ============================================================================

void test_backlog_tracking(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(true);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant_id = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant_id != (hwfq_tenant_id_t)-1, "Failed to add tenant");

    hwfq_allocation_t flow_alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_flow_id_t flow_id;
    hwfq_add_flow(scheduler, tenant_id, &flow_alloc, &flow_id);

    for (int i = 0; i < 5; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
    }

    hwfq_tenant_stats_t stats;
    hwfq_get_tenant_stats(scheduler, tenant_id, &stats);
    TEST_ASSERT(stats.current_backlog == 5, "Backlog should be 5");

    hwfq_flow_stats_t flow_stats;
    hwfq_get_flow_stats(scheduler, tenant_id, flow_id, &flow_stats);
    TEST_ASSERT(flow_stats.current_backlog == 5, "Flow backlog should be 5");

    for (int i = 0; i < 3; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    hwfq_get_tenant_stats(scheduler, tenant_id, &stats);
    TEST_ASSERT(stats.current_backlog == 2, "Backlog should be 2");

    hwfq_get_flow_stats(scheduler, tenant_id, flow_id, &flow_stats);
    TEST_ASSERT(flow_stats.current_backlog == 2, "Flow backlog should be 2");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Stats with Multiple Tenants
// ============================================================================

void test_stats_with_multiple_tenants(void) {
    hwfq_scheduler_t *scheduler = create_scheduler_with_stats(true);
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    hwfq_tenant_id_t tenant1 = add_tenant(scheduler, 100);
    hwfq_tenant_id_t tenant2 = add_tenant(scheduler, 100);
    TEST_ASSERT(tenant1 != (hwfq_tenant_id_t)-1, "Failed to add tenant1");
    TEST_ASSERT(tenant2 != (hwfq_tenant_id_t)-1, "Failed to add tenant2");

    hwfq_flow_id_t flow1_id = test_add_flow(scheduler, tenant1);
    hwfq_flow_id_t flow2_id = test_add_flow(scheduler, tenant2);
    TEST_ASSERT(flow1_id != 0, "Failed to add flow1");
    TEST_ASSERT(flow2_id != 0, "Failed to add flow2");

    for (int i = 0; i < 10; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1000, .timestamp = 0 };
        hwfq_enqueue(scheduler, tenant1, flow1_id, &work);
    }
    for (int i = 0; i < 5; i++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 2000, .timestamp = 0 };
        hwfq_enqueue(scheduler, tenant2, flow2_id, &work);
    }

    for (int i = 0; i < 15; i++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    hwfq_tenant_stats_t stats1, stats2;
    hwfq_get_tenant_stats(scheduler, tenant1, &stats1);
    hwfq_get_tenant_stats(scheduler, tenant2, &stats2);

    TEST_ASSERT(stats1.work_units_processed == 10 * 1000, "Tenant 1 work units mismatch");
    TEST_ASSERT(stats2.work_units_processed == 5 * 2000, "Tenant 2 work units mismatch");
    TEST_ASSERT(stats1.operations_completed == 10, "Tenant 1 ops mismatch");
    TEST_ASSERT(stats2.operations_completed == 5, "Tenant 2 ops mismatch");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Statistics API Tests ===\n\n");

    test_stats_disabled_no_overhead();
    test_tenant_stats_basic();
    test_flow_stats_basic();
    test_wait_time_tracking();
    test_effective_rate_calculation();
    test_reset_specific_flow();
    test_reset_all_flows();
    test_reset_all_tenants();
    test_backlog_tracking();
    test_stats_with_multiple_tenants();

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
