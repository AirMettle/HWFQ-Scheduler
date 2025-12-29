#include "hwfq_group_scheduler.h"
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// ============================================================================
// Test Helper Functions
// ============================================================================

static int g_tests_passed = 0;
static int g_tests_failed = 0;

static hwfq_scheduler_t *create_test_parent(void)
{
    hwfq_config_t config = {
        .max_tenants = 1000,
        .max_flows_per_tenant = 1000,
        .total_capacity = 1000000000ULL,
        .alloc_fn = NULL,
        .free_fn = NULL
    };

    hwfq_scheduler_t *parent = NULL;
    int ret = hwfq_init(&config, &parent);
    if (ret != HWFQ_SUCCESS || parent == NULL) {
        return NULL;
    }

    return parent;
}

// ============================================================================
// Basic Functionality Tests
// ============================================================================

// Test 1: Create and destroy group scheduler
void test_init_destroy(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    TEST_ASSERT(parent != NULL, "Failed to create parent scheduler");

    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);
    TEST_ASSERT(gs != NULL, "Failed to initialize group scheduler");
    TEST_ASSERT(group_scheduler_is_empty(gs), "New scheduler should be empty");
    TEST_ASSERT(group_scheduler_get_session_count(gs) == 0, "Session count should be 0");

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 2: Single entry, single session
void test_single_entry_single_session(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    // Allocate entry first
    uint32_t entry_id;
    int ret = test_alloc_entry(gs->entries, &entry_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to allocate entry");

    group_entry_config_t entry_cfg = {.entry_id = entry_id,
                                      .allocation = {
                                          .allocation_type = HWFQ_ALLOCATION_RATE,
                                          .rate = 100000000ULL
                                      }};

    ret = group_scheduler_configure_entry(gs, &entry_cfg);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to configure entry");

    session_state_t *session = NULL;
    ret = group_scheduler_enqueue(gs, entry_id, 4096, (void *)0x1234, NULL, &session);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue session");
    TEST_ASSERT(session != NULL, "Session should not be NULL");
    TEST_ASSERT(group_scheduler_get_session_count(gs) == 1, "Should have 1 session");

    session_state_t *dequeued = group_scheduler_dequeue(gs);
    TEST_ASSERT(dequeued != NULL, "Dequeue should return session");
    TEST_ASSERT(dequeued == session, "Should dequeue the same session");
    TEST_ASSERT(session_get_user_data(dequeued) == (void *)0x1234, "User data should match");
    TEST_ASSERT(group_scheduler_get_session_count(gs) == 0, "Should have 0 sessions");

    free(dequeued);

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 3: Multiple sessions, same entry
void test_multiple_sessions_same_entry(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    // Allocate entry first
    uint32_t entry_id;
    test_alloc_entry(gs->entries, &entry_id);

    group_entry_config_t entry_cfg = {
        .entry_id = entry_id,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 100000000ULL}};
    group_scheduler_configure_entry(gs, &entry_cfg);

    // Enqueue 3 sessions
    session_state_t *s1, *s2, *s3;
    group_scheduler_enqueue(gs, entry_id, 4096, (void *)1, NULL, &s1);
    group_scheduler_enqueue(gs, entry_id, 8192, (void *)2, NULL, &s2);
    group_scheduler_enqueue(gs, entry_id, 2048, (void *)3, NULL, &s3);

    TEST_ASSERT(group_scheduler_get_session_count(gs) == 3, "Should have 3 sessions");

    session_state_t *d1 = group_scheduler_dequeue(gs);
    session_state_t *d2 = group_scheduler_dequeue(gs);
    session_state_t *d3 = group_scheduler_dequeue(gs);

    TEST_ASSERT(d1 != NULL && d2 != NULL && d3 != NULL, "All sessions should dequeue");

    uint64_t f1 = session_get_finish_time(d1);
    uint64_t f2 = session_get_finish_time(d2);
    uint64_t f3 = session_get_finish_time(d3);

    if (f1 > f2 || f2 > f3) {
        printf("DEBUG: Finish times: d1=%llu, d2=%llu, d3=%llu\n", (unsigned long long)f1,
               (unsigned long long)f2, (unsigned long long)f3);
    }

    TEST_ASSERT(f1 <= f2, "Finish times should be in order");
    TEST_ASSERT(f2 <= f3, "Finish times should be in order");

    free(d1);
    free(d2);
    free(d3);

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 4: Multiple entries with different rates
void test_multiple_entries_different_rates(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    uint32_t id0, id1, id2;
    test_alloc_entry(gs->entries, &id0);
    test_alloc_entry(gs->entries, &id1);
    test_alloc_entry(gs->entries, &id2);

    group_entry_config_t entry0 = {
        .entry_id = id0,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 100000000ULL}};
    group_entry_config_t entry1 = {
        .entry_id = id1,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 200000000ULL}};
    group_entry_config_t entry2 = {
        .entry_id = id2,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 300000000ULL}};

    group_scheduler_configure_entry(gs, &entry0);
    group_scheduler_configure_entry(gs, &entry1);
    group_scheduler_configure_entry(gs, &entry2);

    uint64_t work_size = 10000;
    group_scheduler_enqueue(gs, id0, work_size, (void *)0, NULL, NULL);
    group_scheduler_enqueue(gs, id1, work_size, (void *)1, NULL, NULL);
    group_scheduler_enqueue(gs, id2, work_size, (void *)2, NULL, NULL);

    session_state_t *s1 = group_scheduler_dequeue(gs);
    session_state_t *s2 = group_scheduler_dequeue(gs);
    session_state_t *s3 = group_scheduler_dequeue(gs);

    TEST_ASSERT(session_get_entry_id(s1) == id2, "Entry 2 (highest rate) should finish first");
    TEST_ASSERT(session_get_entry_id(s2) == id1, "Entry 1 should finish second");
    TEST_ASSERT(session_get_entry_id(s3) == id0, "Entry 0 (lowest rate) should finish last");

    free(s1);
    free(s2);
    free(s3);

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 5: Weight-based allocation
void test_weight_based_allocation(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    uint32_t id0, id1;
    test_alloc_entry(gs->entries, &id0);
    test_alloc_entry(gs->entries, &id1);

    group_entry_config_t entry0 = {
        .entry_id = id0, .allocation = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 70}};
    group_entry_config_t entry1 = {
        .entry_id = id1, .allocation = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 30}};

    group_scheduler_configure_entry(gs, &entry0);
    group_scheduler_configure_entry(gs, &entry1);

    uint64_t rate0, rate1;
    group_scheduler_get_entry_rate(gs, id0, &rate0);
    group_scheduler_get_entry_rate(gs, id1, &rate1);

    TEST_ASSERT(rate0 > rate1, "Entry 0 should have higher rate");

    double ratio = (double)rate0 / (double)rate1;
    TEST_ASSERT(ratio > 2.0 && ratio < 2.5, "Rate ratio should be ~2.33 (70/30)");

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 6: Mixed rate and weight allocation
void test_mixed_rate_and_weight(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    uint32_t id0, id1;
    test_alloc_entry(gs->entries, &id0);
    test_alloc_entry(gs->entries, &id1);

    group_entry_config_t entry0 = {
        .entry_id = id0,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 500000000ULL}};

    group_entry_config_t entry1 = {
        .entry_id = id1, .allocation = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 1}};

    group_scheduler_configure_entry(gs, &entry0);
    group_scheduler_configure_entry(gs, &entry1);

    uint64_t rate0, rate1;
    group_scheduler_get_entry_rate(gs, id0, &rate0);
    group_scheduler_get_entry_rate(gs, id1, &rate1);

    TEST_ASSERT(rate0 == 500000000ULL, "Rate-based entry should have exact rate");
    TEST_ASSERT(rate1 <= 500000000ULL, "Weight-based entry should get remaining capacity");

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 7: Virtual time advancement
void test_virtual_time_advancement(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    // Allocate entry first
    uint32_t entry_id;
    test_alloc_entry(gs->entries, &entry_id);

    group_entry_config_t entry_cfg = {
        .entry_id = entry_id,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 100000000ULL}};
    group_scheduler_configure_entry(gs, &entry_cfg);

    uint64_t initial_vtime = group_scheduler_get_virtual_time(gs);
    TEST_ASSERT(initial_vtime == 0, "Initial virtual time should be 0");

    group_scheduler_enqueue(gs, entry_id, 4096, NULL, NULL, NULL);
    session_state_t *s = group_scheduler_dequeue(gs);
    TEST_ASSERT(s != NULL, "Should dequeue session");

    group_scheduler_update_virtual_time(gs, 4096);

    uint64_t new_vtime = group_scheduler_get_virtual_time(gs);
    TEST_ASSERT(new_vtime > initial_vtime, "Virtual time should advance");

    free(s);
    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 8: Empty queue dequeue
void test_empty_queue_dequeue(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    session_state_t *s = group_scheduler_dequeue(gs);
    TEST_ASSERT(s == NULL, "Dequeue from empty queue should return NULL");
    TEST_ASSERT(group_scheduler_is_empty(gs), "Queue should be empty");

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 9: Entry configuration validation
void test_entry_config_validation(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    uint32_t id0, id1;
    test_alloc_entry(gs->entries, &id0);
    test_alloc_entry(gs->entries, &id1);

    group_entry_config_t invalid_rate = {
        .entry_id = id0, .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 0}};
    int ret = group_scheduler_configure_entry(gs, &invalid_rate);
    TEST_ASSERT(ret != HWFQ_SUCCESS, "Should reject zero rate");

    group_entry_config_t invalid_weight = {
        .entry_id = id1, .allocation = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 0}};
    ret = group_scheduler_configure_entry(gs, &invalid_weight);
    TEST_ASSERT(ret != HWFQ_SUCCESS, "Should reject zero weight");

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 10: Overbooking prevention
void test_overbooking_prevention(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    uint32_t id0, id1;
    test_alloc_entry(gs->entries, &id0);
    test_alloc_entry(gs->entries, &id1);

    group_entry_config_t entry0 = {
        .entry_id = id0,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 800000000ULL}};
    int ret = group_scheduler_configure_entry(gs, &entry0);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should accept 800MB rate");

    group_entry_config_t entry1 = {
        .entry_id = id1,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 300000000ULL}};
    ret = group_scheduler_configure_entry(gs, &entry1);
    TEST_ASSERT(ret == HWFQ_ERR_OVERBOOKED, "Should reject overbooking");

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 11: Entry reconfiguration
void test_entry_reconfiguration(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    uint32_t entry_id;
    test_alloc_entry(gs->entries, &entry_id);

    group_entry_config_t entry_cfg = {
        .entry_id = entry_id,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 100000000ULL}};
    group_scheduler_configure_entry(gs, &entry_cfg);

    uint64_t rate1;
    group_scheduler_get_entry_rate(gs, entry_id, &rate1);
    TEST_ASSERT(rate1 == 100000000ULL, "Initial rate should be 100MB");

    entry_cfg.allocation.rate = 200000000ULL;
    int ret = group_scheduler_configure_entry(gs, &entry_cfg);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Reconfiguration should succeed");

    uint64_t rate2;
    group_scheduler_get_entry_rate(gs, entry_id, &rate2);
    TEST_ASSERT(rate2 == 200000000ULL, "Updated rate should be 200MB");

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// Test 12: Remove entry
void test_remove_entry(void)
{
    hwfq_scheduler_t *parent = create_test_parent();
    group_scheduler_t *gs = test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);

    // Allocate entry first
    uint32_t entry_id;
    test_alloc_entry(gs->entries, &entry_id);

    group_entry_config_t entry_cfg = {
        .entry_id = entry_id,
        .allocation = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 100000000ULL}};
    group_scheduler_configure_entry(gs, &entry_cfg);

    int ret = group_scheduler_remove_entry(gs, entry_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should remove entry");

    ret = group_scheduler_enqueue(gs, entry_id, 4096, NULL, NULL, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_NOT_FOUND, "Should reject enqueue to removed entry");

    test_destroy_group_scheduler(parent, gs);
    hwfq_destroy(parent);
    TEST_PASS();
}

// ============================================================================
// Test Runner
// ============================================================================

int main(void)
{
    printf("=== H-WFQ Group Scheduler Tests (Single-Level WFQ Component) ===\n\n");

    test_init_destroy();
    test_single_entry_single_session();
    test_multiple_sessions_same_entry();
    test_multiple_entries_different_rates();
    test_weight_based_allocation();
    test_mixed_rate_and_weight();
    test_virtual_time_advancement();
    test_empty_queue_dequeue();
    test_entry_config_validation();
    test_overbooking_prevention();
    test_entry_reconfiguration();
    test_remove_entry();

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    if (g_tests_failed == 0) {
        printf("\nAll tests passed!\n");
        return 0;
    } else {
        printf("\nSome tests failed.\n");
        return 1;
    }
}
