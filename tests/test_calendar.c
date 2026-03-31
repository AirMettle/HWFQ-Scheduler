// ============================================================================
// Calendar Queue Unit Tests
// ============================================================================
//
// Tests for the DTS-Calendar Queue used for WF2Q+ scheduling.
// The calendar queue maps sessions to bins based on their finish times
// and uses hierarchical bitfields for efficient lookups.
//
// ============================================================================

#include "test_common.h"
#include "../src/hwfq_group_scheduler_internal.h"
#include "../src/hwfq_internal.h"

// ============================================================================
// Test Helper Functions
// ============================================================================

static group_scheduler_t *create_test_gs(uint32_t num_groups, uint32_t bins_per_group) {
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 100,
        .max_total_flows = 10000,
        .num_groups = num_groups,
        .bins_per_group = bins_per_group,
        .total_capacity = 1000000000ULL,
        .alloc_fn = NULL,
        .free_fn = NULL
    };

    hwfq_scheduler_t *parent = NULL;
    int ret = hwfq_init(&config, &parent);
    if (ret != HWFQ_SUCCESS || parent == NULL) {
        return NULL;
    }

    return test_create_group_scheduler(parent, num_groups, bins_per_group, 1000000, 1000);
}

static void destroy_test_gs(group_scheduler_t *gs) {
    if (gs == NULL) return;

    hwfq_scheduler_t *parent = gs->parent;
    hwfq_chunked_entries_t *entries = gs->entries;

    group_scheduler_destroy(parent, gs);

    if (entries != NULL) {
        hwfq_chunked_entries_destroy(entries);
        hwfq_free(parent, entries);
    }

    hwfq_destroy(parent);
}

static session_state_t *create_test_session(uint64_t start_time, uint64_t finish_time,
                                             uint32_t group_index) {
    session_state_t *session = calloc(1, sizeof(session_state_t));
    if (session == NULL) return NULL;

    session->start_time = start_time;
    session->finish_time = finish_time;
    session->group_index = group_index;
    session->service_interval = finish_time - start_time;
    session->bin_next = NULL;
    session->bin_prev = NULL;

    return session;
}

// ============================================================================
// Test 1: Insert Single Session
// ============================================================================

static void test_calendar_insert_single(void) {
    printf("  test_calendar_insert_single...");

    group_scheduler_t *gs = create_test_gs(16, 64);
    assert(gs != NULL);

    session_state_t *session = create_test_session(0, 1000, 0);
    assert(session != NULL);

    int ret = calendar_insert_session(gs, session);
    (void)ret;
    assert(ret == HWFQ_SUCCESS);

    assert(gs->min_finish_time == 1000);
    assert(gs->min_start_time == 0);
    assert(gs->min_session == session);

    // Note: sessions in bins are freed by group_scheduler_destroy
    destroy_test_gs(gs);

    printf(" PASSED\n");
}

// ============================================================================
// Test 2: Insert Multiple Sessions
// ============================================================================

static void test_calendar_insert_multiple(void) {
    printf("  test_calendar_insert_multiple...");

    group_scheduler_t *gs = create_test_gs(16, 64);
    assert(gs != NULL);

    session_state_t *session1 = create_test_session(0, 3000, 0);
    session_state_t *session2 = create_test_session(0, 1000, 0);
    session_state_t *session3 = create_test_session(0, 2000, 0);
    assert(session1 != NULL && session2 != NULL && session3 != NULL);

    calendar_insert_session(gs, session1);
    calendar_insert_session(gs, session2);
    calendar_insert_session(gs, session3);

    assert(gs->min_finish_time == 1000);
    assert(gs->min_session == session2);

    // Note: sessions in bins are freed by group_scheduler_destroy
    destroy_test_gs(gs);

    printf(" PASSED\n");
}

// ============================================================================
// Test 3: Remove Session
// ============================================================================

static void test_calendar_remove(void) {
    printf("  test_calendar_remove...");

    group_scheduler_t *gs = create_test_gs(16, 64);
    assert(gs != NULL);

    session_state_t *session = create_test_session(0, 1000, 0);
    assert(session != NULL);

    calendar_insert_session(gs, session);
    gs->active_session_count = 1;

    int ret = calendar_remove_session(gs, session);
    (void)ret;
    assert(ret == HWFQ_SUCCESS);

    assert(gs->min_session == NULL);
    assert(gs->min_finish_time == UINT64_MAX);

    free(session);
    destroy_test_gs(gs);

    printf(" PASSED\n");
}

// ============================================================================
// Test 4: Find Min Session
// ============================================================================

static void test_calendar_find_min(void) {
    printf("  test_calendar_find_min...");

    group_scheduler_t *gs = create_test_gs(16, 64);
    assert(gs != NULL);

    session_state_t *session1 = create_test_session(0, 3000, 0);
    session_state_t *session2 = create_test_session(0, 1000, 0);
    session_state_t *session3 = create_test_session(0, 2000, 0);
    assert(session1 != NULL && session2 != NULL && session3 != NULL);

    calendar_insert_session(gs, session1);
    gs->active_session_count++;
    calendar_insert_session(gs, session2);
    gs->active_session_count++;
    calendar_insert_session(gs, session3);
    gs->active_session_count++;

    gs->virtual_time = 0;
    session_state_t *min = calendar_find_min_session(gs);
    (void)min;
    assert(min == session2);

    // Note: sessions in bins are freed by group_scheduler_destroy
    destroy_test_gs(gs);

    printf(" PASSED\n");
}

// ============================================================================
// Test 5: Bitfield Operations
// ============================================================================

static void test_calendar_bitfield_ops(void) {
    printf("  test_calendar_bitfield_ops...");

    uint32_t bitfield[4] = {0, 0, 0, 0};

    set_bin_bit(bitfield, 0);
    assert(test_bin_bit(bitfield, 0));
    assert(!test_bin_bit(bitfield, 1));

    set_bin_bit(bitfield, 31);
    assert(test_bin_bit(bitfield, 31));

    set_bin_bit(bitfield, 32);
    assert(test_bin_bit(bitfield, 32));

    set_bin_bit(bitfield, 63);
    assert(test_bin_bit(bitfield, 63));

    clear_bin_bit(bitfield, 0);
    assert(!test_bin_bit(bitfield, 0));

    clear_bin_bit(bitfield, 32);
    assert(!test_bin_bit(bitfield, 32));

    printf(" PASSED\n");
}

// ============================================================================
// Test 6: Group Index Calculation
// ============================================================================

static void test_calendar_group_index(void) {
    printf("  test_calendar_group_index...");

    uint64_t base_interval = 1000;

    uint32_t g = calculate_group_index_from_interval(500, base_interval);
    (void)g;
    assert(g == 0);

    g = calculate_group_index_from_interval(1000, base_interval);
    assert(g == 0);

    g = calculate_group_index_from_interval(1500, base_interval);
    assert(g == 0);

    g = calculate_group_index_from_interval(2000, base_interval);
    assert(g == 1);

    g = calculate_group_index_from_interval(3000, base_interval);
    assert(g == 1);

    g = calculate_group_index_from_interval(4000, base_interval);
    assert(g == 2);

    g = calculate_group_index_from_interval(1000000000, base_interval);
    assert(g == 15);

    printf(" PASSED\n");
}

// ============================================================================
// Test 7: Bin Index Calculation
// ============================================================================

static void test_calendar_bin_calculation(void) {
    printf("  test_calendar_bin_calculation...");

    uint64_t base_interval = 1000;
    uint32_t bins_per_group = 64;

    uint32_t bin = calculate_bin_index_from_finish_time(0, 0, bins_per_group, base_interval);
    (void)bin;
    assert(bin < bins_per_group);

    bin = calculate_bin_index_from_finish_time(1000, 0, bins_per_group, base_interval);
    assert(bin < bins_per_group);

    bin = calculate_bin_index_from_finish_time(2000, 0, bins_per_group, base_interval);
    assert(bin < bins_per_group);

    uint32_t bin1 = calculate_bin_index_from_finish_time(1000, 1, bins_per_group, base_interval);
    uint32_t bin2 = calculate_bin_index_from_finish_time(2000, 1, bins_per_group, base_interval);
    (void)bin1;
    (void)bin2;
    assert(bin1 < bins_per_group);
    assert(bin2 < bins_per_group);

    printf(" PASSED\n");
}

// ============================================================================
// Test 8: Find First Set Bit
// ============================================================================

static void test_find_first_set_bit(void) {
    printf("  test_find_first_set_bit...");

    assert(find_first_set_bit(0) == 32);

    assert(find_first_set_bit(1) == 0);
    assert(find_first_set_bit(2) == 1);
    assert(find_first_set_bit(4) == 2);
    assert(find_first_set_bit(8) == 3);

    assert(find_first_set_bit(3) == 0);
    assert(find_first_set_bit(6) == 1);

    assert(find_first_set_bit(0x80000000) == 31);

    printf(" PASSED\n");
}

// ============================================================================
// Test 9: Eligibility Check
// ============================================================================

static void test_calendar_eligibility(void) {
    printf("  test_calendar_eligibility...");

    group_scheduler_t *gs = create_test_gs(16, 64);
    assert(gs != NULL);

    session_state_t *future_session = create_test_session(1000, 2000, 0);
    session_state_t *eligible_session = create_test_session(0, 1500, 0);
    assert(future_session != NULL && eligible_session != NULL);

    calendar_insert_session(gs, future_session);
    gs->active_session_count++;
    calendar_insert_session(gs, eligible_session);
    gs->active_session_count++;

    gs->virtual_time = 500;
    session_state_t *min = calendar_find_min_session(gs);
    (void)min;
    assert(min == eligible_session);

    // Note: sessions in bins are freed by group_scheduler_destroy
    destroy_test_gs(gs);

    printf(" PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("Calendar Queue Tests\n");
    printf("====================\n\n");

    test_calendar_insert_single();
    test_calendar_insert_multiple();
    test_calendar_remove();
    test_calendar_find_min();
    test_calendar_bitfield_ops();
    test_calendar_group_index();
    test_calendar_bin_calculation();
    test_find_first_set_bit();
    test_calendar_eligibility();

    printf("\nAll calendar queue tests passed!\n");
    return 0;
}
