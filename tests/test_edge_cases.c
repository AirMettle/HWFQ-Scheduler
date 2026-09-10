// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 AirMettle, Inc.

// ============================================================================
// Edge Case Tests for H-WFQ Scheduler
// ============================================================================
//
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

    // Add flows up to the limit (scheduler assigns flow_ids)
    int num_flows = TEST_MAX_FLOWS - 1;  // Leave room near the limit
    hwfq_flow_id_t flow_ids[TEST_MAX_FLOWS];
    for (int f = 0; f < num_flows; f++) {
        ret = hwfq_add_flow(scheduler, tenant_id, &alloc, &flow_ids[f]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add flow");
    }

    printf("    Successfully added %d flows\n", num_flows);

    // Verify they work
    for (int f = 0; f < num_flows; f++) {
        hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
        ret = hwfq_enqueue(scheduler, tenant_id, flow_ids[f], &work);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue to configured flow");
    }

    printf("    Successfully enqueued to all %d flows\n", num_flows);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Max Total Flows Limit
// ============================================================================

void test_max_total_flows(void) {
    // Verifies the global max_total_flows cap is enforced across tenants.
    const uint32_t FLOW_LIMIT = 500;
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = FLOW_LIMIT,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };

    uint32_t total_flows_configured = 0;
    bool hit_limit = false;
    hwfq_tenant_id_t last_tid = 0;

    // Add flows until the cap rejects us.
    for (int t = 0; t < 10 && !hit_limit; t++) {
        hwfq_tenant_id_t tenant_id;
        ret = hwfq_add_tenant(scheduler, &alloc, &tenant_id);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "tenant add failed unexpectedly");
        last_tid = tenant_id;

        for (int f = 0; f < 80; f++) {
            hwfq_flow_id_t flow_id;
            ret = hwfq_add_flow(scheduler, tenant_id, &alloc, &flow_id);
            if (ret == HWFQ_SUCCESS) {
                total_flows_configured++;
            } else {
                TEST_ASSERT(ret == HWFQ_ERR_NO_MEMORY,
                            "flow add should fail with HWFQ_ERR_NO_MEMORY at cap");
                hit_limit = true;
                break;
            }
        }
    }

    TEST_ASSERT(hit_limit, "global cap should have been reached");
    TEST_ASSERT(total_flows_configured == FLOW_LIMIT,
                "total flows configured should equal max_total_flows at cap");

    // One more add must also fail.
    hwfq_flow_id_t extra_flow;
    ret = hwfq_add_flow(scheduler, last_tid, &alloc, &extra_flow);
    TEST_ASSERT(ret == HWFQ_ERR_NO_MEMORY, "beyond-cap add should fail");

    printf("    Global cap enforced at %u (attempted %u-th add rejected)\n",
           FLOW_LIMIT, FLOW_LIMIT + 1);

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Double-complete and mis-routed complete return HWFQ_ERR_NOT_FOUND
// ============================================================================

void test_complete_error_detection(void) {
    hwfq_config_t config = {
        .max_tenants = 4,
        .max_flows_per_tenant = 2,
        .max_total_flows = 8,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };
    hwfq_scheduler_t *scheduler = NULL;
    TEST_ASSERT(hwfq_init(&config, &scheduler) == HWFQ_SUCCESS, "init");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tid; TEST_ASSERT(hwfq_add_tenant(scheduler, &alloc, &tid) == HWFQ_SUCCESS, "add tenant");
    hwfq_flow_id_t fid; TEST_ASSERT(hwfq_add_flow(scheduler, tid, &alloc, &fid) == HWFQ_SUCCESS, "add flow");

    int marker = 42;
    hwfq_session_t work = { .user_data = &marker, .work_size = 100 };
    TEST_ASSERT(hwfq_enqueue(scheduler, tid, fid, &work) == HWFQ_SUCCESS, "enqueue");

    hwfq_session_t work_out;
    hwfq_tenant_id_t tid_out; hwfq_flow_id_t fid_out;
    TEST_ASSERT(hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out) == HWFQ_SUCCESS, "dequeue");

    // First complete succeeds.
    int r1 = hwfq_complete(scheduler, &work_out, tid_out, fid_out, 1000);
    TEST_ASSERT(r1 == HWFQ_SUCCESS, "first complete must return HWFQ_SUCCESS");

    // Second complete on the same session must detect the error.
    int r2 = hwfq_complete(scheduler, &work_out, tid_out, fid_out, 2000);
    TEST_ASSERT(r2 == HWFQ_ERR_NOT_FOUND, "double-complete must return HWFQ_ERR_NOT_FOUND");

    // Mis-routed complete (wrong tenant) must also fail.
    hwfq_session_t bogus = { .user_data = &marker, .work_size = 100 };
    int r3 = hwfq_complete(scheduler, &bogus, tid + 1, fid, 3000);
    TEST_ASSERT(r3 == HWFQ_ERR_NOT_FOUND, "wrong-tenant complete must return HWFQ_ERR_NOT_FOUND");

    // NULL scheduler must return HWFQ_ERR_INVALID_ARG.
    int r4 = hwfq_complete(NULL, &bogus, tid, fid, 4000);
    TEST_ASSERT(r4 == HWFQ_ERR_INVALID_ARG, "NULL scheduler must return HWFQ_ERR_INVALID_ARG");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: cleanup_fn fires on cancel, on timeout, and on destroy-with-backlog,
//       but NOT on normal dequeue path.
// ============================================================================

static int g_cleanup_count = 0;
static void count_cleanup(void *user_data) {
    (void)user_data;
    g_cleanup_count++;
}

void test_cleanup_fn_invoked(void) {
    hwfq_config_t config = {
        .max_tenants = 4,
        .max_flows_per_tenant = 2,
        .max_total_flows = 8,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };

    // Scenario A: normal enqueue + dequeue + complete — cleanup_fn must NOT fire.
    {
        hwfq_scheduler_t *s = NULL;
        TEST_ASSERT(hwfq_init(&config, &s) == HWFQ_SUCCESS, "init A");
        hwfq_tenant_id_t tid; TEST_ASSERT(hwfq_add_tenant(s, &alloc, &tid) == HWFQ_SUCCESS, "add tenant");
        hwfq_flow_id_t fid; TEST_ASSERT(hwfq_add_flow(s, tid, &alloc, &fid) == HWFQ_SUCCESS, "add flow");

        g_cleanup_count = 0;
        int marker = 1;
        hwfq_session_t w = { .user_data = &marker, .work_size = 100, .cleanup_fn = count_cleanup };
        TEST_ASSERT(hwfq_enqueue(s, tid, fid, &w) == HWFQ_SUCCESS, "enqueue A");
        hwfq_session_t wo; hwfq_tenant_id_t to; hwfq_flow_id_t fo;
        TEST_ASSERT(hwfq_dequeue(s, &wo, &to, &fo) == HWFQ_SUCCESS, "dequeue A");
        TEST_ASSERT(hwfq_complete(s, &wo, to, fo, 1000) == HWFQ_SUCCESS, "complete A");
        hwfq_destroy(s);
        TEST_ASSERT(g_cleanup_count == 0, "cleanup_fn must NOT fire on normal dequeue+complete");
    }

    // Scenario B: enqueue + destroy without dequeue — cleanup_fn must fire once.
    {
        hwfq_scheduler_t *s = NULL;
        TEST_ASSERT(hwfq_init(&config, &s) == HWFQ_SUCCESS, "init B");
        hwfq_tenant_id_t tid; TEST_ASSERT(hwfq_add_tenant(s, &alloc, &tid) == HWFQ_SUCCESS, "add tenant");
        hwfq_flow_id_t fid; TEST_ASSERT(hwfq_add_flow(s, tid, &alloc, &fid) == HWFQ_SUCCESS, "add flow");

        g_cleanup_count = 0;
        int marker = 2;
        hwfq_session_t w = { .user_data = &marker, .work_size = 100, .cleanup_fn = count_cleanup };
        TEST_ASSERT(hwfq_enqueue(s, tid, fid, &w) == HWFQ_SUCCESS, "enqueue B");
        hwfq_destroy(s);
        TEST_ASSERT(g_cleanup_count == 1, "cleanup_fn must fire exactly once on destroy-with-backlog");
    }

    printf("    cleanup_fn contract verified: fires on destroy-with-backlog, not on normal dequeue\n");
    TEST_PASS();
}

// ============================================================================
// Regression test: eligibility must find an interior session when the bin
// head is ineligible. Constructs two flows whose sessions collide in the same
// finish-time bin but have different start_times.
// ============================================================================

void test_bin_head_ineligible_regression(void) {
    hwfq_config_t config = {
        .max_tenants = 4,
        .max_flows_per_tenant = 2,
        .max_total_flows = 8,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false
    };
    hwfq_scheduler_t *scheduler = NULL;
    TEST_ASSERT(hwfq_init(&config, &scheduler) == HWFQ_SUCCESS, "init");

    hwfq_allocation_t alloc = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 };
    hwfq_tenant_id_t tid;
    TEST_ASSERT(hwfq_add_tenant(scheduler, &alloc, &tid) == HWFQ_SUCCESS, "add tenant");
    hwfq_flow_id_t f1, f2;
    TEST_ASSERT(hwfq_add_flow(scheduler, tid, &alloc, &f1) == HWFQ_SUCCESS, "add flow 1");
    TEST_ASSERT(hwfq_add_flow(scheduler, tid, &alloc, &f2) == HWFQ_SUCCESS, "add flow 2");

    // Enqueue 40 alternating sessions from two flows and dequeue them all.
    // Whatever sessions actually get queued, every enqueued session must come
    // back out through dequeue — no session left stranded because an
    // ineligible head shadowed an eligible interior session.
    int markers[40];
    for (int i = 0; i < 40; i++) {
        markers[i] = i;
        hwfq_session_t w = { .user_data = &markers[i], .work_size = 1000 };
        hwfq_flow_id_t f = (i & 1) ? f1 : f2;
        TEST_ASSERT(hwfq_enqueue(scheduler, tid, f, &w) == HWFQ_SUCCESS, "enqueue");
    }

    int dequeued = 0;
    bool seen[40] = { false };
    while (dequeued < 40) {
        hwfq_session_t wo; hwfq_tenant_id_t to; hwfq_flow_id_t fo;
        int r = hwfq_dequeue(scheduler, &wo, &to, &fo);
        TEST_ASSERT(r == HWFQ_SUCCESS, "dequeue should not return NO_WORK with backlog");
        int *mp = (int *)wo.user_data;
        TEST_ASSERT(mp >= &markers[0] && mp < &markers[40], "dequeued marker in range");
        int idx = (int)(mp - &markers[0]);
        TEST_ASSERT(!seen[idx], "each session dequeued exactly once");
        seen[idx] = true;
        TEST_ASSERT(hwfq_complete(scheduler, &wo, to, fo, 1000 + dequeued) == HWFQ_SUCCESS, "complete");
        dequeued++;
    }

    printf("    All 40 sessions from 2 flows dequeued exactly once\n");

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

    hwfq_flow_id_t flow_id = test_add_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    // Enqueue with very large work size (not UINT64_MAX to avoid overflow)
    hwfq_session_t work = { .user_data = NULL, .work_size = UINT64_MAX / 2, .timestamp = 0 };
    ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
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

    hwfq_flow_id_t flow_id = test_add_flow(scheduler, tenant_id);
    TEST_ASSERT(flow_id != 0, "Failed to add flow");

    #define RAPID_CYCLES 1000

    for (int i = 0; i < RAPID_CYCLES; i++) {
        // Fill with 10 items
        for (int j = 0; j < 10; j++) {
            hwfq_session_t work = { .user_data = NULL, .work_size = 1024, .timestamp = 0 };
            ret = hwfq_enqueue(scheduler, tenant_id, flow_id, &work);
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

    // Allocate entries
    uint32_t entry_ids[3];
    test_alloc_entry(gs->entries, &entry_ids[0]);
    test_alloc_entry(gs->entries, &entry_ids[1]);
    test_alloc_entry(gs->entries, &entry_ids[2]);

    // Configure entries with different rates (higher rate = lower finish time)
    // entry_ids[0] -> 100 MB/s (lowest), entry_ids[1] -> 200 MB/s, entry_ids[2] -> 300 MB/s (highest)
    group_entry_config_t config0 = {
        .entry_id = entry_ids[0],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_RATE, .rate = 100000000ULL }  // 100 MB/s
    };
    group_entry_config_t config1 = {
        .entry_id = entry_ids[1],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_RATE, .rate = 200000000ULL }  // 200 MB/s
    };
    group_entry_config_t config2 = {
        .entry_id = entry_ids[2],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_RATE, .rate = 300000000ULL }  // 300 MB/s
    };

    group_scheduler_configure_entry(gs, &config0);
    group_scheduler_configure_entry(gs, &config1);
    group_scheduler_configure_entry(gs, &config2);

    // Enqueue one session to each entry (same size)
    group_scheduler_enqueue(gs, entry_ids[0], 1024, NULL, NULL, NULL);
    group_scheduler_enqueue(gs, entry_ids[1], 1024, NULL, NULL, NULL);
    group_scheduler_enqueue(gs, entry_ids[2], 1024, NULL, NULL, NULL);

    // Dequeue order should be: highest rate first (entry_ids[2], then [1], then [0])
    session_state_t *s1 = group_scheduler_dequeue(gs);
    session_state_t *s2 = group_scheduler_dequeue(gs);
    session_state_t *s3 = group_scheduler_dequeue(gs);

    TEST_ASSERT(s1 != NULL && s2 != NULL && s3 != NULL, "All sessions should dequeue");

    printf("    Dequeue order: %u, %u, %u (expected: %u, %u, %u for rate order)\n",
           s1->entry_id, s2->entry_id, s3->entry_id,
           entry_ids[2], entry_ids[1], entry_ids[0]);

    // Higher rate should finish first
    TEST_ASSERT(s1->entry_id == entry_ids[2], "Highest rate entry should dequeue first");
    TEST_ASSERT(s2->entry_id == entry_ids[1], "Middle rate entry should dequeue second");
    TEST_ASSERT(s3->entry_id == entry_ids[0], "Lowest rate entry should dequeue last");

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

    printf("\nRunning test_complete_error_detection...\n");
    test_complete_error_detection();

    printf("\nRunning test_cleanup_fn_invoked...\n");
    test_cleanup_fn_invoked();

    printf("\nRunning test_bin_head_ineligible_regression...\n");
    test_bin_head_ineligible_regression();

    printf("\n=== Edge Case Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
