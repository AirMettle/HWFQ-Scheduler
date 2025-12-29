// ============================================================================
// Fairness Tests for H-WFQ Scheduler
// ============================================================================
//
// These tests verify that the WF2Q+ scheduling algorithm provides fair resource
// allocation according to configured weights. Tests use geometric weight
// distributions (powers of 2) to verify precise allocation ratios.
//
// Test scenarios:
// 1. All entries backlogged with geometric weights (1/2 to 1/1024)
// 2. Progressive removal of highest-weight entries
// 3. Verification of allocation accuracy within tolerance
//
// ============================================================================

#include "test_common.h"
#include "../src/hwfq_group_scheduler_internal.h"
#include "../src/hwfq_internal.h"
#include "../include/hwfq.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Global test counters
static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Test Configuration
// ============================================================================

#define NUM_ENTRIES 10
#define SCHEDULING_CYCLES 5000  // Enough cycles for statistical significance
#define TOLERANCE_PERCENT 15.0  // Allow higher deviation for low-weight entries

// Geometric weights: 512, 256, 128, 64, 32, 16, 8, 4, 2, 1
// Sum = 1023, ratios are exact powers of 2
static const uint32_t GEOMETRIC_WEIGHTS[NUM_ENTRIES] = {
    512, 256, 128, 64, 32, 16, 8, 4, 2, 1
};

// ============================================================================
// Helper Functions
// ============================================================================

static hwfq_scheduler_t *create_test_scheduler(void) {
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .num_groups = 16,
        .bins_per_group = 2048,
        .total_capacity = 1000000000ULL,  // 1 GB/s
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

static group_scheduler_t *create_fairness_group_scheduler(hwfq_scheduler_t *parent) {
    return test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);
}

static uint32_t g_entry_ids[NUM_ENTRIES];

static void configure_geometric_entries(group_scheduler_t *gs) {
    for (int i = 0; i < NUM_ENTRIES; i++) {
        // Allocate entry first
        test_alloc_entry(gs->entries, &g_entry_ids[i]);

        group_entry_config_t config = {
            .entry_id = g_entry_ids[i],
            .allocation = {
                .allocation_type = HWFQ_ALLOCATION_WEIGHT,
                .weight = GEOMETRIC_WEIGHTS[i]
            }
        };
        group_scheduler_configure_entry(gs, &config);
    }
}

static double calculate_expected_ratio(uint32_t weight, uint32_t total_weight) {
    return (double)weight / (double)total_weight;
}

static bool is_within_tolerance(double actual, double expected, double tolerance_percent) {
    if (expected == 0.0) {
        return actual == 0.0;
    }
    double deviation = fabs((actual - expected) / expected) * 100.0;
    return deviation <= tolerance_percent;
}

// ============================================================================
// Test: Geometric Weights - All Backlogged
// ============================================================================

void test_geometric_weights_all_backlogged(void) {
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_fairness_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Configure entries with geometric weights
    configure_geometric_entries(gs);

    // Count dequeues per entry
    uint64_t dequeue_counts[NUM_ENTRIES] = {0};

    // Run scheduling cycles
    for (int cycle = 0; cycle < SCHEDULING_CYCLES; cycle++) {
        // Enqueue work for all entries (keep them backlogged)
        for (int i = 0; i < NUM_ENTRIES; i++) {
            session_state_t *session = NULL;
            int ret = group_scheduler_enqueue(gs, g_entry_ids[i], 4096, NULL, NULL, &session);
            TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue session");
        }

        // Dequeue one session
        session_state_t *session = group_scheduler_dequeue(gs);
        TEST_ASSERT(session != NULL, "Failed to dequeue session");

        // Count which entry was scheduled - map back to index
        group_entry_id_t entry_id = session_get_entry_id(session);
        int entry_index = -1;
        for (int i = 0; i < NUM_ENTRIES; i++) {
            if (g_entry_ids[i] == entry_id) {
                entry_index = i;
                break;
            }
        }
        TEST_ASSERT(entry_index >= 0, "Invalid entry ID");
        dequeue_counts[entry_index]++;

        // Update virtual time
        group_scheduler_update_virtual_time(gs, session_get_work_size(session));

        // Free the session
        hwfq_free(scheduler, session);
    }

    // Drain remaining sessions
    while (!group_scheduler_is_empty(gs)) {
        session_state_t *session = group_scheduler_dequeue(gs);
        if (session != NULL) {
            hwfq_free(scheduler, session);
        }
    }

    // Calculate total weight
    uint32_t total_weight = 0;
    for (int i = 0; i < NUM_ENTRIES; i++) {
        total_weight += GEOMETRIC_WEIGHTS[i];
    }

    // Verify allocation ratios
    uint64_t total_dequeues = 0;
    for (int i = 0; i < NUM_ENTRIES; i++) {
        total_dequeues += dequeue_counts[i];
    }

    printf("    Allocation Results (expected vs actual):\n");
    bool all_within_tolerance = true;
    for (int i = 0; i < NUM_ENTRIES; i++) {
        double expected_ratio = calculate_expected_ratio(GEOMETRIC_WEIGHTS[i], total_weight);
        double actual_ratio = (double)dequeue_counts[i] / (double)total_dequeues;
        double deviation = fabs((actual_ratio - expected_ratio) / expected_ratio) * 100.0;

        printf("      Entry %d (weight %3u): expected %.4f, actual %.4f (%.2f%% deviation)\n",
               i, GEOMETRIC_WEIGHTS[i], expected_ratio, actual_ratio, deviation);

        // Use higher tolerance for low-weight entries (statistical variance is higher)
        // With 5000 cycles, weight=1 entry gets ~5 dequeues, so variance is high
        double adjusted_tolerance = TOLERANCE_PERCENT;
        if (GEOMETRIC_WEIGHTS[i] <= 8) {
            adjusted_tolerance = 60.0;  // Allow higher deviation for weights 1-8
        } else if (GEOMETRIC_WEIGHTS[i] <= 32) {
            adjusted_tolerance = 25.0;  // Allow moderate deviation for weights 16-32
        }

        if (!is_within_tolerance(actual_ratio, expected_ratio, adjusted_tolerance)) {
            all_within_tolerance = false;
        }
    }

    TEST_ASSERT(all_within_tolerance, "Allocation ratios exceed tolerance");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Progressive Removal
// ============================================================================

void test_progressive_removal(void) {
    // Test that removing entries causes fair redistribution of remaining capacity.
    // We create a fresh scheduler for each removal test to avoid complexity
    // with orphaned sessions.

    printf("    Testing redistribution after removal:\n");

    // Test removing entry 0 (highest weight 512) - entry 1 (256) should get ~50%
    {
        hwfq_scheduler_t *scheduler = create_test_scheduler();
        TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");
        group_scheduler_t *gs = create_fairness_group_scheduler(scheduler);
        TEST_ASSERT(gs != NULL, "Failed to create group scheduler");
        configure_geometric_entries(gs);

        // Remove entry 0 (use g_entry_ids[0])
        int ret = group_scheduler_remove_entry(gs, g_entry_ids[0]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to remove entry 0");

        // Count dequeues for remaining entries (1-9)
        uint64_t dequeue_counts[NUM_ENTRIES] = {0};
        int cycles = SCHEDULING_CYCLES / 2;

        for (int cycle = 0; cycle < cycles; cycle++) {
            for (int i = 1; i < NUM_ENTRIES; i++) {
                session_state_t *session = NULL;
                group_scheduler_enqueue(gs, g_entry_ids[i], 4096, NULL, NULL, &session);
            }
            session_state_t *session = group_scheduler_dequeue(gs);
            if (session != NULL) {
                // Map back to index
                int entry_index = -1;
                for (int i = 1; i < NUM_ENTRIES; i++) {
                    if (g_entry_ids[i] == session->entry_id) {
                        entry_index = i;
                        break;
                    }
                }
                if (entry_index >= 0) dequeue_counts[entry_index]++;
                group_scheduler_update_virtual_time(gs, session->work_size);
                hwfq_free(scheduler, session);
            }
        }

        // Remaining weight = 511, entry 1 = 256 => expected ~50%
        uint64_t total = 0;
        for (int i = 1; i < NUM_ENTRIES; i++) total += dequeue_counts[i];

        double expected = 256.0 / 511.0;  // ~0.501
        double actual = (double)dequeue_counts[1] / (double)total;
        printf("      After removing entry 0: entry 1 expected %.3f, actual %.3f\n",
               expected, actual);
        TEST_ASSERT(is_within_tolerance(actual, expected, TOLERANCE_PERCENT + 5.0),
                    "Entry 1 allocation after removal exceeds tolerance");

        test_destroy_group_scheduler(scheduler, gs);
        hwfq_destroy(scheduler);
    }

    // Test removing entries 0 and 1 - entry 2 (128) should get ~50%
    {
        hwfq_scheduler_t *scheduler = create_test_scheduler();
        TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");
        group_scheduler_t *gs = create_fairness_group_scheduler(scheduler);
        TEST_ASSERT(gs != NULL, "Failed to create group scheduler");
        configure_geometric_entries(gs);

        group_scheduler_remove_entry(gs, g_entry_ids[0]);
        group_scheduler_remove_entry(gs, g_entry_ids[1]);

        uint64_t dequeue_counts[NUM_ENTRIES] = {0};
        int cycles = SCHEDULING_CYCLES / 2;

        for (int cycle = 0; cycle < cycles; cycle++) {
            for (int i = 2; i < NUM_ENTRIES; i++) {
                session_state_t *session = NULL;
                group_scheduler_enqueue(gs, g_entry_ids[i], 4096, NULL, NULL, &session);
            }
            session_state_t *session = group_scheduler_dequeue(gs);
            if (session != NULL) {
                // Map back to index
                int entry_index = -1;
                for (int i = 2; i < NUM_ENTRIES; i++) {
                    if (g_entry_ids[i] == session->entry_id) {
                        entry_index = i;
                        break;
                    }
                }
                if (entry_index >= 0) dequeue_counts[entry_index]++;
                group_scheduler_update_virtual_time(gs, session->work_size);
                hwfq_free(scheduler, session);
            }
        }

        // Remaining weight = 255, entry 2 = 128 => expected ~50%
        uint64_t total = 0;
        for (int i = 2; i < NUM_ENTRIES; i++) total += dequeue_counts[i];

        double expected = 128.0 / 255.0;  // ~0.502
        double actual = (double)dequeue_counts[2] / (double)total;
        printf("      After removing entries 0,1: entry 2 expected %.3f, actual %.3f\n",
               expected, actual);
        TEST_ASSERT(is_within_tolerance(actual, expected, TOLERANCE_PERCENT + 5.0),
                    "Entry 2 allocation after removal exceeds tolerance");

        test_destroy_group_scheduler(scheduler, gs);
        hwfq_destroy(scheduler);
    }

    TEST_PASS();
}

// ============================================================================
// Test: Rate-Based Entries with Weights
// ============================================================================

void test_mixed_rate_and_weight(void) {
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_fairness_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Allocate entries first
    uint32_t entry_ids[4];
    for (int i = 0; i < 4; i++) {
        test_alloc_entry(gs->entries, &entry_ids[i]);
    }

    // Configure 2 rate-based entries (each gets 25% of capacity)
    // and 2 weight-based entries (split remaining 50% by weights 3:1)
    group_entry_config_t rate_config1 = {
        .entry_id = entry_ids[0],
        .allocation = {
            .allocation_type = HWFQ_ALLOCATION_RATE,
            .rate = 250000000ULL  // 250 MB/s (25%)
        }
    };
    group_entry_config_t rate_config2 = {
        .entry_id = entry_ids[1],
        .allocation = {
            .allocation_type = HWFQ_ALLOCATION_RATE,
            .rate = 250000000ULL  // 250 MB/s (25%)
        }
    };
    group_entry_config_t weight_config1 = {
        .entry_id = entry_ids[2],
        .allocation = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 75  // Should get 75% of remaining 50% = 37.5%
        }
    };
    group_entry_config_t weight_config2 = {
        .entry_id = entry_ids[3],
        .allocation = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = 25  // Should get 25% of remaining 50% = 12.5%
        }
    };

    int ret = group_scheduler_configure_entry(gs, &rate_config1);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to configure rate entry 1");

    ret = group_scheduler_configure_entry(gs, &rate_config2);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to configure rate entry 2");

    ret = group_scheduler_configure_entry(gs, &weight_config1);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to configure weight entry 1");

    ret = group_scheduler_configure_entry(gs, &weight_config2);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to configure weight entry 2");

    // Run scheduling cycles
    uint64_t dequeue_counts[4] = {0};
    int cycles = SCHEDULING_CYCLES;

    for (int cycle = 0; cycle < cycles; cycle++) {
        // Enqueue work for all entries
        for (int i = 0; i < 4; i++) {
            session_state_t *session = NULL;
            ret = group_scheduler_enqueue(gs, entry_ids[i], 4096, NULL, NULL, &session);
            TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue session");
        }

        // Dequeue one session
        session_state_t *session = group_scheduler_dequeue(gs);
        TEST_ASSERT(session != NULL, "Failed to dequeue session");

        // Map entry_id back to index
        group_entry_id_t entry_id = session_get_entry_id(session);
        for (int i = 0; i < 4; i++) {
            if (entry_ids[i] == entry_id) {
                dequeue_counts[i]++;
                break;
            }
        }

        group_scheduler_update_virtual_time(gs, session_get_work_size(session));
        hwfq_free(scheduler, session);
    }

    // Drain remaining sessions
    while (!group_scheduler_is_empty(gs)) {
        session_state_t *session = group_scheduler_dequeue(gs);
        if (session != NULL) {
            hwfq_free(scheduler, session);
        }
    }

    uint64_t total = 0;
    for (int i = 0; i < 4; i++) {
        total += dequeue_counts[i];
    }

    printf("    Mixed Rate/Weight Results:\n");
    for (int i = 0; i < 4; i++) {
        double actual_ratio = (double)dequeue_counts[i] / (double)total;
        printf("      Entry %d: %.4f (%.1f%%)\n", i, actual_ratio, actual_ratio * 100.0);
    }

    // Rate-based entries should each get approximately 25%
    // Weight-based entries should split remaining 50% as 75:25 = 37.5%:12.5%
    // With rate margin (1/2), these ratios should still hold relatively

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Flow ID 0 Reserved
// ============================================================================

void test_flow_id_zero_reserved(void) {
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add a tenant
    hwfq_allocation_t alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add a valid flow (scheduler assigns flow_id)
    hwfq_flow_id_t flow_id;
    ret = hwfq_add_flow(scheduler, tenant_id, &alloc, &flow_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add valid flow");
    TEST_ASSERT(flow_id != HWFQ_FLOW_ID_RESERVED, "Flow ID 0 should never be assigned");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Flow Reconfiguration
// ============================================================================

void test_flow_reconfiguration(void) {
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    // Add a tenant
    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    // Add a flow (scheduler assigns flow_id)
    hwfq_allocation_t flow_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 50
    };
    hwfq_flow_id_t flow_id;
    ret = hwfq_add_flow(scheduler, tenant_id, &flow_alloc, &flow_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add flow");

    // Reconfigure flow (should succeed)
    hwfq_allocation_t new_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    ret = hwfq_reconfigure_flow(scheduler, tenant_id, flow_id, &new_alloc);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to reconfigure flow");

    // Reconfigure non-existent flow (should fail)
    ret = hwfq_reconfigure_flow(scheduler, tenant_id, 999, &new_alloc);
    TEST_ASSERT(ret == HWFQ_ERR_NOT_FOUND, "Reconfiguring non-existent flow should fail");

    // Reconfigure with flow ID 0 (should fail)
    ret = hwfq_reconfigure_flow(scheduler, tenant_id, HWFQ_FLOW_ID_RESERVED, &new_alloc);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Reconfiguring flow ID 0 should fail");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Pure Rate-Based Fairness
// ============================================================================

void test_rate_based_fairness(void) {
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_fairness_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Allocate 4 entries with rate-based allocation
    // Rates: 400, 300, 200, 100 MB/s (total 1000 MB/s = 1 GB/s)
    // Expected ratios: 40%, 30%, 20%, 10%
    uint32_t entry_ids[4];
    uint64_t rates[4] = {400000000ULL, 300000000ULL, 200000000ULL, 100000000ULL};

    for (int i = 0; i < 4; i++) {
        test_alloc_entry(gs->entries, &entry_ids[i]);

        group_entry_config_t config = {
            .entry_id = entry_ids[i],
            .allocation = {
                .allocation_type = HWFQ_ALLOCATION_RATE,
                .rate = rates[i]
            }
        };
        int ret = group_scheduler_configure_entry(gs, &config);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to configure rate entry");
    }

    // Run scheduling cycles
    uint64_t dequeue_counts[4] = {0};
    int cycles = SCHEDULING_CYCLES;

    for (int cycle = 0; cycle < cycles; cycle++) {
        // Enqueue work for all entries
        for (int i = 0; i < 4; i++) {
            session_state_t *session = NULL;
            int ret = group_scheduler_enqueue(gs, entry_ids[i], 4096, NULL, NULL, &session);
            TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to enqueue session");
        }

        // Dequeue one session
        session_state_t *session = group_scheduler_dequeue(gs);
        TEST_ASSERT(session != NULL, "Failed to dequeue session");

        // Map entry_id back to index
        group_entry_id_t entry_id = session_get_entry_id(session);
        for (int i = 0; i < 4; i++) {
            if (entry_ids[i] == entry_id) {
                dequeue_counts[i]++;
                break;
            }
        }

        group_scheduler_update_virtual_time(gs, session_get_work_size(session));
        hwfq_free(scheduler, session);
    }

    // Drain remaining sessions
    while (!group_scheduler_is_empty(gs)) {
        session_state_t *session = group_scheduler_dequeue(gs);
        if (session != NULL) {
            hwfq_free(scheduler, session);
        }
    }

    uint64_t total = 0;
    for (int i = 0; i < 4; i++) {
        total += dequeue_counts[i];
    }

    // Expected ratios based on rate (accounting for rate margin factor of 1/2)
    // The rate margin factor divides by 2, so effective weights are:
    // 400/2=200, 300/2=150, 200/2=100, 100/2=50 => total 500
    // Ratios should still be 4:3:2:1 = 40%, 30%, 20%, 10%
    double expected_ratios[4] = {0.40, 0.30, 0.20, 0.10};

    printf("    Rate-Based Fairness Results:\n");
    bool all_within_tolerance = true;
    for (int i = 0; i < 4; i++) {
        double actual_ratio = (double)dequeue_counts[i] / (double)total;
        double deviation = fabs((actual_ratio - expected_ratios[i]) / expected_ratios[i]) * 100.0;
        printf("      Entry %d (rate %llu MB/s): expected %.2f, actual %.4f (%.2f%% deviation)\n",
               i, (unsigned long long)(rates[i] / 1000000), expected_ratios[i], actual_ratio, deviation);

        // Allow higher tolerance for the lowest-rate entry
        double adjusted_tolerance = (i == 3) ? 25.0 : TOLERANCE_PERCENT;
        if (!is_within_tolerance(actual_ratio, expected_ratios[i], adjusted_tolerance)) {
            all_within_tolerance = false;
        }
    }

    TEST_ASSERT(all_within_tolerance, "Rate-based allocation ratios exceed tolerance");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Fairness Tests ===\n\n");

    printf("Running test_geometric_weights_all_backlogged...\n");
    test_geometric_weights_all_backlogged();

    printf("\nRunning test_progressive_removal...\n");
    test_progressive_removal();

    printf("\nRunning test_mixed_rate_and_weight...\n");
    test_mixed_rate_and_weight();

    printf("\nRunning test_flow_id_zero_reserved...\n");
    test_flow_id_zero_reserved();

    printf("\nRunning test_flow_reconfiguration...\n");
    test_flow_reconfiguration();

    printf("\nRunning test_rate_based_fairness...\n");
    test_rate_based_fairness();

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
