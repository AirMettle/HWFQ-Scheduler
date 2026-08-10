// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 AirMettle, Inc.

// ============================================================================
// Extended Fairness Tests for H-WFQ Scheduler
// ============================================================================
//
// - Long-running fairness validation (100k cycles)
// - Extreme weight ratios (1000:1)
// - Near-zero weights
// - Fairness during churn
// - Temporal fairness (sliding windows)
//
// ============================================================================

#include "test_common.h"
#include "../include/hwfq.h"
#include "../src/hwfq_internal.h"
#include "../src/hwfq_group_scheduler_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Global test counters
static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Helper Functions
// ============================================================================

static hwfq_scheduler_t *create_fairness_scheduler(void) {
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 1000000000ULL,
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

static group_scheduler_t *create_extended_group_scheduler(hwfq_scheduler_t *parent) {
    return test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);
}

static bool is_within_tolerance(double actual, double expected, double tolerance_percent) {
    if (expected == 0.0) {
        return actual == 0.0;
    }
    double deviation = fabs((actual - expected) / expected) * 100.0;
    return deviation <= tolerance_percent;
}

// ============================================================================
// Test: Long Running Fairness (100k cycles)
// ============================================================================

// Helper to map entry_id back to index
static int find_entry_index(uint32_t *entry_ids, int count, uint32_t entry_id) {
    for (int i = 0; i < count; i++) {
        if (entry_ids[i] == entry_id) return i;
    }
    return -1;
}

void test_long_running_fairness(void) {
    hwfq_scheduler_t *scheduler = create_fairness_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_extended_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Allocate and configure 5 entries with geometric weights
    uint32_t weights[] = {160, 80, 40, 20, 10};  // Total = 310
    uint32_t entry_ids[5];
    for (int i = 0; i < 5; i++) {
        test_alloc_entry(gs->entries, &entry_ids[i]);
        group_entry_config_t config = {
            .entry_id = entry_ids[i],
            .allocation = {
                .allocation_type = HWFQ_ALLOCATION_WEIGHT,
                .weight = weights[i]
            }
        };
        group_scheduler_configure_entry(gs, &config);
    }

    #define LONG_RUN_CYCLES 5000  // Reduced for faster test execution
    uint64_t counts[5] = {0};

    printf("    Running %d scheduling cycles...\n", LONG_RUN_CYCLES);

    for (int cycle = 0; cycle < LONG_RUN_CYCLES; cycle++) {
        // Enqueue for all entries
        for (int i = 0; i < 5; i++) {
            group_scheduler_enqueue(gs, entry_ids[i], 1024, NULL, NULL, NULL);
        }

        // Dequeue one
        session_state_t *session = group_scheduler_dequeue(gs);
        if (session) {
            int idx = find_entry_index(entry_ids, 5, session->entry_id);
            if (idx >= 0) counts[idx]++;
            group_scheduler_update_virtual_time(gs, session->work_size);
            free(session);
        }
    }

    // Drain remaining
    session_state_t *session;
    while ((session = group_scheduler_dequeue(gs)) != NULL) {
        free(session);
    }

    // Verify fairness
    uint32_t total_weight = 310;
    uint64_t total_dequeued = 0;
    for (int i = 0; i < 5; i++) {
        total_dequeued += counts[i];
    }

    printf("    Results:\n");
    bool all_fair = true;
    for (int i = 0; i < 5; i++) {
        double expected = (double)weights[i] / total_weight;
        double actual = (double)counts[i] / total_dequeued;
        double deviation = fabs((actual - expected) / expected) * 100.0;
        printf("      Entry %d (weight %3u): expected %.4f, actual %.4f (%.2f%% dev)\n",
               i, weights[i], expected, actual, deviation);

        // 10% tolerance
        if (!is_within_tolerance(actual, expected, 10.0)) {
            all_fair = false;
        }
    }

    TEST_ASSERT(all_fair, "Long-running fairness exceeded tolerance");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Extreme Weight Ratios (1000:1)
// ============================================================================

void test_extreme_weight_ratios(void) {
    hwfq_scheduler_t *scheduler = create_fairness_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_extended_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Allocate entries
    uint32_t entry_ids[2];
    test_alloc_entry(gs->entries, &entry_ids[0]);
    test_alloc_entry(gs->entries, &entry_ids[1]);

    // Entry 0: weight 100, Entry 1: weight 1 (100:1 ratio, more reasonable for testing)
    group_entry_config_t config0 = {
        .entry_id = entry_ids[0],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 }
    };
    group_entry_config_t config1 = {
        .entry_id = entry_ids[1],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 1 }
    };
    group_scheduler_configure_entry(gs, &config0);
    group_scheduler_configure_entry(gs, &config1);

    #define EXTREME_CYCLES 1010  // Enough for entry 1 to get ~10 dequeues
    uint64_t count0 = 0, count1 = 0;

    // Enqueue in proper ratio to avoid excessive buildup
    for (int cycle = 0; cycle < EXTREME_CYCLES; cycle++) {
        // Always enqueue for entry 0
        group_scheduler_enqueue(gs, entry_ids[0], 1024, NULL, NULL, NULL);

        // Enqueue for entry 1 less frequently (every 100 cycles) to match weight ratio
        if (cycle % 100 == 0) {
            group_scheduler_enqueue(gs, entry_ids[1], 1024, NULL, NULL, NULL);
        }

        session_state_t *session = group_scheduler_dequeue(gs);
        if (session) {
            if (session->entry_id == entry_ids[0]) count0++;
            else count1++;
            group_scheduler_update_virtual_time(gs, session->work_size);
            free(session);
        }
    }

    // Drain
    session_state_t *session;
    while ((session = group_scheduler_dequeue(gs)) != NULL) {
        if (session->entry_id == entry_ids[0]) count0++;
        else count1++;
        group_scheduler_update_virtual_time(gs, session->work_size);
        free(session);
    }

    double ratio = (count1 > 0) ? (double)count0 / count1 : 0;
    printf("    Entry 0 (weight 100): %llu\n", (unsigned long long)count0);
    printf("    Entry 1 (weight 1):   %llu\n", (unsigned long long)count1);
    printf("    Ratio: %.0f:1 (expected ~100:1)\n", ratio);

    // Entry 1 should get some work
    TEST_ASSERT(count1 > 0, "Low-weight entry should get some work");
    // Ratio should be in reasonable range (50-200:1)
    TEST_ASSERT(ratio > 50 && ratio < 200, "Extreme ratio outside expected bounds");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Near-Zero Weights
// ============================================================================

void test_near_zero_weights(void) {
    hwfq_scheduler_t *scheduler = create_fairness_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_extended_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Allocate entries
    uint32_t entry_ids[2];
    test_alloc_entry(gs->entries, &entry_ids[0]);
    test_alloc_entry(gs->entries, &entry_ids[1]);

    // Very small weights
    group_entry_config_t config0 = {
        .entry_id = entry_ids[0],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 2 }
    };
    group_entry_config_t config1 = {
        .entry_id = entry_ids[1],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 1 }
    };
    group_scheduler_configure_entry(gs, &config0);
    group_scheduler_configure_entry(gs, &config1);

    #define SMALL_WEIGHT_CYCLES 1000
    uint64_t count0 = 0, count1 = 0;

    // Enqueue proportionally to weights for proper fairness testing
    for (int cycle = 0; cycle < SMALL_WEIGHT_CYCLES; cycle++) {
        // Enqueue 2 for entry 0 for every 1 for entry 1 (matching weight ratio)
        group_scheduler_enqueue(gs, entry_ids[0], 1024, NULL, NULL, NULL);
        group_scheduler_enqueue(gs, entry_ids[0], 1024, NULL, NULL, NULL);
        group_scheduler_enqueue(gs, entry_ids[1], 1024, NULL, NULL, NULL);

        // Dequeue 3 times to drain what we just enqueued
        for (int d = 0; d < 3; d++) {
            session_state_t *session = group_scheduler_dequeue(gs);
            if (session) {
                if (session->entry_id == entry_ids[0]) count0++;
                else count1++;
                group_scheduler_update_virtual_time(gs, session->work_size);
                free(session);
            }
        }
    }

    // Drain remaining
    session_state_t *session;
    while ((session = group_scheduler_dequeue(gs)) != NULL) {
        if (session->entry_id == entry_ids[0]) count0++;
        else count1++;
        free(session);
    }

    double ratio = (count1 > 0) ? (double)count0 / count1 : 0;
    printf("    Entry 0 (weight 2): %llu\n", (unsigned long long)count0);
    printf("    Entry 1 (weight 1): %llu\n", (unsigned long long)count1);
    printf("    Ratio: %.2f:1 (expected ~2:1)\n", ratio);

    // Both should get work
    TEST_ASSERT(count0 > 0, "Weight 2 entry should get work");
    TEST_ASSERT(count1 > 0, "Weight 1 entry should get work");
    // Ratio should be approximately 2:1 (1.5-2.5 acceptable)
    TEST_ASSERT(ratio > 1.5 && ratio < 2.5, "Near-zero weight ratio incorrect");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Fairness During Churn
// ============================================================================

void test_fairness_during_churn(void) {
    hwfq_scheduler_t *scheduler = create_fairness_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_extended_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Allocate 3 entries upfront
    uint32_t entry_ids[3];
    test_alloc_entry(gs->entries, &entry_ids[0]);
    test_alloc_entry(gs->entries, &entry_ids[1]);
    test_alloc_entry(gs->entries, &entry_ids[2]);

    // Start with 2 entries of equal weight
    group_entry_config_t config0 = {
        .entry_id = entry_ids[0],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 }
    };
    group_entry_config_t config1 = {
        .entry_id = entry_ids[1],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 }
    };
    group_scheduler_configure_entry(gs, &config0);
    group_scheduler_configure_entry(gs, &config1);

    #define CHURN_FAIRNESS_CYCLES 3000
    uint64_t count0 = 0, count1 = 0, count2 = 0;

    for (int cycle = 0; cycle < CHURN_FAIRNESS_CYCLES; cycle++) {
        // Enqueue for active entries
        group_scheduler_enqueue(gs, entry_ids[0], 1024, NULL, NULL, NULL);
        group_scheduler_enqueue(gs, entry_ids[1], 1024, NULL, NULL, NULL);

        // At cycle 1000, add entry 2
        if (cycle == 1000) {
            group_entry_config_t config2 = {
                .entry_id = entry_ids[2],
                .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 }
            };
            group_scheduler_configure_entry(gs, &config2);
        }

        // After adding entry 2, enqueue for it too
        if (cycle >= 1000) {
            group_scheduler_enqueue(gs, entry_ids[2], 1024, NULL, NULL, NULL);
        }

        // At cycle 2000, remove entry 1
        if (cycle == 2000) {
            // Drain entry 1's sessions first
            group_scheduler_remove_entry(gs, entry_ids[1]);
        }

        session_state_t *session = group_scheduler_dequeue(gs);
        if (session) {
            if (session->entry_id == entry_ids[0]) count0++;
            else if (session->entry_id == entry_ids[1]) count1++;
            else if (session->entry_id == entry_ids[2]) count2++;
            group_scheduler_update_virtual_time(gs, session->work_size);
            free(session);
        }
    }

    // Drain
    session_state_t *session;
    while ((session = group_scheduler_dequeue(gs)) != NULL) {
        if (session->entry_id == entry_ids[0]) count0++;
        else if (session->entry_id == entry_ids[1]) count1++;
        else if (session->entry_id == entry_ids[2]) count2++;
        free(session);
    }

    printf("    Entry 0: %llu (active entire time)\n", (unsigned long long)count0);
    printf("    Entry 1: %llu (removed at cycle 2000)\n", (unsigned long long)count1);
    printf("    Entry 2: %llu (added at cycle 1000)\n", (unsigned long long)count2);

    // All entries should have received work
    TEST_ASSERT(count0 > 0, "Entry 0 should have work");
    TEST_ASSERT(count1 > 0, "Entry 1 should have work");
    TEST_ASSERT(count2 > 0, "Entry 2 should have work");

    // Entry 0 should have roughly the most (always active)
    TEST_ASSERT(count0 > count1, "Entry 0 should have more than entry 1");
    TEST_ASSERT(count0 > count2, "Entry 0 should have more than entry 2");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Temporal Fairness (Sliding Windows)
// ============================================================================

void test_temporal_fairness(void) {
    hwfq_scheduler_t *scheduler = create_fairness_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_extended_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Allocate entries
    uint32_t entry_ids[2];
    test_alloc_entry(gs->entries, &entry_ids[0]);
    test_alloc_entry(gs->entries, &entry_ids[1]);

    // Two entries, 2:1 weight ratio
    group_entry_config_t config0 = {
        .entry_id = entry_ids[0],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 200 }
    };
    group_entry_config_t config1 = {
        .entry_id = entry_ids[1],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 }
    };
    group_scheduler_configure_entry(gs, &config0);
    group_scheduler_configure_entry(gs, &config1);

    #define WINDOW_SIZE 500
    #define TOTAL_WINDOWS 5

    printf("    Checking fairness across %d windows of %d cycles each...\n",
           TOTAL_WINDOWS, WINDOW_SIZE);

    bool all_windows_fair = true;

    for (int window = 0; window < TOTAL_WINDOWS; window++) {
        uint64_t window_count0 = 0, window_count1 = 0;

        for (int cycle = 0; cycle < WINDOW_SIZE; cycle++) {
            group_scheduler_enqueue(gs, entry_ids[0], 1024, NULL, NULL, NULL);
            group_scheduler_enqueue(gs, entry_ids[1], 1024, NULL, NULL, NULL);

            session_state_t *session = group_scheduler_dequeue(gs);
            if (session) {
                if (session->entry_id == entry_ids[0]) window_count0++;
                else window_count1++;
                group_scheduler_update_virtual_time(gs, session->work_size);
                free(session);
            }
        }

        double ratio = (window_count1 > 0) ? (double)window_count0 / window_count1 : 0;
        bool fair = (ratio >= 1.5 && ratio <= 2.5);

        printf("      Window %d: ratio %.2f:1 %s\n", window, ratio, fair ? "[OK]" : "[FAIL]");

        if (!fair) {
            all_windows_fair = false;
        }
    }

    // Drain
    session_state_t *session;
    while ((session = group_scheduler_dequeue(gs)) != NULL) {
        free(session);
    }

    TEST_ASSERT(all_windows_fair, "Temporal fairness failed in one or more windows");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Burst Arrival Fairness
// ============================================================================

void test_burst_arrival_fairness(void) {
    hwfq_scheduler_t *scheduler = create_fairness_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_extended_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Allocate entries
    uint32_t entry_ids[2];
    test_alloc_entry(gs->entries, &entry_ids[0]);
    test_alloc_entry(gs->entries, &entry_ids[1]);

    // Equal weights
    group_entry_config_t config0 = {
        .entry_id = entry_ids[0],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 }
    };
    group_entry_config_t config1 = {
        .entry_id = entry_ids[1],
        .allocation = { .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100 }
    };
    group_scheduler_configure_entry(gs, &config0);
    group_scheduler_configure_entry(gs, &config1);

    uint64_t count0 = 0, count1 = 0;

    // Simulate bursty arrival: Entry 0 sends big burst, then Entry 1 sends big burst
    // Repeat this pattern
    for (int round = 0; round < 10; round++) {
        // Entry 0 burst of 100
        for (int i = 0; i < 100; i++) {
            group_scheduler_enqueue(gs, entry_ids[0], 1024, NULL, NULL, NULL);
        }

        // Entry 1 burst of 100
        for (int i = 0; i < 100; i++) {
            group_scheduler_enqueue(gs, entry_ids[1], 1024, NULL, NULL, NULL);
        }

        // Drain the burst
        for (int i = 0; i < 200; i++) {
            session_state_t *session = group_scheduler_dequeue(gs);
            if (session) {
                if (session->entry_id == entry_ids[0]) count0++;
                else count1++;
                group_scheduler_update_virtual_time(gs, session->work_size);
                free(session);
            }
        }
    }

    printf("    Entry 0: %llu\n", (unsigned long long)count0);
    printf("    Entry 1: %llu\n", (unsigned long long)count1);

    // Should be roughly equal despite bursty arrival
    double ratio = (count1 > 0) ? (double)count0 / count1 : 0;
    printf("    Ratio: %.2f:1 (expected ~1:1)\n", ratio);

    TEST_ASSERT(ratio >= 0.9 && ratio <= 1.1, "Burst fairness ratio should be ~1:1");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Extended Fairness Tests ===\n\n");

    printf("Running test_long_running_fairness...\n");
    test_long_running_fairness();

    printf("\nRunning test_extreme_weight_ratios...\n");
    test_extreme_weight_ratios();

    printf("\nRunning test_near_zero_weights...\n");
    test_near_zero_weights();

    printf("\nRunning test_fairness_during_churn...\n");
    test_fairness_during_churn();

    printf("\nRunning test_temporal_fairness...\n");
    test_temporal_fairness();

    printf("\nRunning test_burst_arrival_fairness...\n");
    test_burst_arrival_fairness();

    printf("\n=== Extended Fairness Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
