// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 AirMettle, Inc.

// ============================================================================
// Exponential Skew Test with Progressive Removal
// ============================================================================
//
// This test validates scheduler precision with geometrically-weighted entries
// (512, 256, 128... 1) while progressively removing the highest-weight entry
// during continuous operation.
//
// Key difference from test_fairness.c: Uses a SINGLE scheduler instance with
// live removal, rather than creating fresh schedulers per phase.
//
// Test phases:
// 1. All 10 entries active, validate allocation ratios
// 2. Remove entry 0 (512), validate remaining 9 entries
// 3. Remove entry 1 (256), validate remaining 8 entries
// ... continue through single remaining entry
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
#define CYCLES_PER_PHASE 2000   // Reduced for faster testing; increase for precision
#define TIGHT_TOLERANCE 5.0     // +/- 5% tolerance for ideal backlogged conditions
#define RELAXED_TOLERANCE 10.0  // Relaxed tolerance for lower-weight entries

// Geometric weights: 512, 256, 128, 64, 32, 16, 8, 4, 2, 1
// These represent 1/2, 1/4, 1/8... 1/1024 of total
static const uint32_t SKEW_WEIGHTS[NUM_ENTRIES] = {
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

static group_scheduler_t *create_skew_group_scheduler(hwfq_scheduler_t *parent) {
    return test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);
}

// Store allocated entry IDs for use across phases
static uint32_t g_entry_ids[NUM_ENTRIES];

static void configure_geometric_entries(group_scheduler_t *gs) {
    for (int i = 0; i < NUM_ENTRIES; i++) {
        // Allocate entry from chunked storage
        test_alloc_entry(gs->entries, &g_entry_ids[i]);

        group_entry_config_t config = {
            .entry_id = g_entry_ids[i],
            .allocation = {
                .allocation_type = HWFQ_ALLOCATION_WEIGHT,
                .weight = SKEW_WEIGHTS[i]
            }
        };
        group_scheduler_configure_entry(gs, &config);
    }
}

static double calculate_deviation(double actual, double expected) {
    if (expected == 0.0) {
        return (actual == 0.0) ? 0.0 : 100.0;
    }
    return ((actual - expected) / expected) * 100.0;
}

static bool is_within_tolerance(double actual, double expected, double tolerance_percent) {
    if (expected == 0.0) {
        return actual == 0.0;
    }
    double deviation = fabs((actual - expected) / expected) * 100.0;
    return deviation <= tolerance_percent;
}

// Get tolerance based on entry weight (lower weights have higher statistical variance)
static double get_tolerance_for_weight(uint32_t weight) {
    if (weight >= 128) {
        return TIGHT_TOLERANCE;
    } else if (weight >= 32) {
        return RELAXED_TOLERANCE;
    } else if (weight >= 8) {
        return 20.0;
    } else if (weight >= 2) {
        return 50.0;  // Very low weights have high variance
    } else {
        return 100.0;  // Weight 1 can have extreme variance with few cycles
    }
}

// ============================================================================
// Phase Result Structure
// ============================================================================

typedef struct {
    int phase_number;
    int first_active_entry;  // First entry that's still active (0, 1, 2, ...)
    int num_active_entries;
    uint64_t dequeue_counts[NUM_ENTRIES];
    uint64_t total_dequeues;
    uint32_t total_weight;
    bool all_within_tolerance;
} phase_result_t;

// ============================================================================
// Run a single phase: enqueue/dequeue cycles for active entries
// ============================================================================

// Helper to find index from entry_id
static int find_entry_index(uint32_t entry_id) {
    for (int i = 0; i < NUM_ENTRIES; i++) {
        if (g_entry_ids[i] == entry_id) return i;
    }
    return -1;
}

static phase_result_t run_phase(hwfq_scheduler_t *scheduler,
                                 group_scheduler_t *gs,
                                 int first_active,
                                 int num_cycles) {
    phase_result_t result = {0};
    result.first_active_entry = first_active;
    result.num_active_entries = NUM_ENTRIES - first_active;

    // Calculate total weight for active entries
    for (int i = first_active; i < NUM_ENTRIES; i++) {
        result.total_weight += SKEW_WEIGHTS[i];
    }

    // Run scheduling cycles
    for (int cycle = 0; cycle < num_cycles; cycle++) {
        // Enqueue work for all ACTIVE entries (keep them backlogged)
        for (int i = first_active; i < NUM_ENTRIES; i++) {
            session_state_t *session = NULL;
            int ret = group_scheduler_enqueue(gs, g_entry_ids[i], 4096, NULL, NULL, &session);
            if (ret != HWFQ_SUCCESS) {
                printf("      ERROR: Failed to enqueue for entry %d (id=%u)\n", i, g_entry_ids[i]);
                return result;
            }
        }

        // Dequeue one session
        session_state_t *session = group_scheduler_dequeue(gs);
        if (session == NULL) {
            printf("      ERROR: Failed to dequeue at cycle %d\n", cycle);
            return result;
        }

        // Count which entry was scheduled
        group_entry_id_t entry_id = session_get_entry_id(session);
        int idx = find_entry_index(entry_id);
        if (idx >= 0 && idx < NUM_ENTRIES) {
            result.dequeue_counts[idx]++;
            result.total_dequeues++;
        }

        // Update virtual time
        group_scheduler_update_virtual_time(gs, session_get_work_size(session));

        // Free the session
        hwfq_free(scheduler, session);
    }

    // Drain remaining sessions from this phase
    while (!group_scheduler_is_empty(gs)) {
        session_state_t *session = group_scheduler_dequeue(gs);
        if (session != NULL) {
            hwfq_free(scheduler, session);
        }
    }

    return result;
}

// ============================================================================
// Validate phase results
// ============================================================================

static bool validate_phase(const phase_result_t *result, int phase_number, bool verbose) {
    if (result->total_dequeues == 0) {
        printf("      Phase %d: No dequeues recorded\n", phase_number);
        return false;
    }

    bool all_ok = true;
    int first = result->first_active_entry;

    if (verbose) {
        printf("    Phase %d: %d entries active (entries %d-%d), total_weight=%u\n",
               phase_number, result->num_active_entries, first, NUM_ENTRIES - 1,
               result->total_weight);
    }

    for (int i = first; i < NUM_ENTRIES; i++) {
        double expected = (double)SKEW_WEIGHTS[i] / (double)result->total_weight;
        double actual = (double)result->dequeue_counts[i] / (double)result->total_dequeues;
        double deviation = calculate_deviation(actual, expected);
        double tolerance = get_tolerance_for_weight(SKEW_WEIGHTS[i]);
        bool ok = is_within_tolerance(actual, expected, tolerance);

        if (!ok) {
            all_ok = false;
        }

        if (verbose) {
            printf("      Entry %d (w=%3u): expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
                   i, SKEW_WEIGHTS[i], expected, actual, deviation,
                   ok ? "OK" : "FAIL");
        }
    }

    return all_ok;
}

// ============================================================================
// Test: All Entries Backlogged (Phase 1)
// ============================================================================

void test_all_backlogged(void) {
    printf("    Testing all 10 entries backlogged...\n");

    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_skew_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Configure entries with geometric weights
    configure_geometric_entries(gs);

    // Run phase with all entries active
    phase_result_t result = run_phase(scheduler, gs, 0, CYCLES_PER_PHASE);
    result.phase_number = 1;

    // Validate results
    bool ok = validate_phase(&result, 1, true);
    TEST_ASSERT(ok, "Phase 1 allocation ratios exceed tolerance");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Progressive Removal with Live Scheduler
// ============================================================================

void test_progressive_removal_live(void) {
    printf("    Testing progressive removal (live scheduler)...\n");

    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_skew_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    // Configure entries with geometric weights
    configure_geometric_entries(gs);

    bool all_phases_ok = true;

    // Run through all phases, removing highest-weight entry each time
    for (int phase = 1; phase <= NUM_ENTRIES; phase++) {
        int first_active = phase - 1;  // Phase 1 = entry 0+, Phase 2 = entry 1+, etc.
        int num_active = NUM_ENTRIES - first_active;

        if (num_active <= 0) break;

        // Remove the entry that was highest in previous phase
        // (except for phase 1 where all entries are active)
        if (phase > 1) {
            int entry_to_remove = phase - 2;
            int ret = group_scheduler_remove_entry(gs, g_entry_ids[entry_to_remove]);
            if (ret != HWFQ_SUCCESS) {
                printf("      WARNING: Failed to remove entry %d (id=%u, may already be removed)\n",
                       entry_to_remove, g_entry_ids[entry_to_remove]);
            }
        }

        // Run phase
        phase_result_t result = run_phase(scheduler, gs, first_active, CYCLES_PER_PHASE);
        result.phase_number = phase;

        // Validate (less verbose for phases after 1)
        bool verbose = (phase <= 3 || phase == NUM_ENTRIES);  // Show first 3 and last
        bool ok = validate_phase(&result, phase, verbose);

        if (!ok) {
            all_phases_ok = false;
            printf("      Phase %d: FAILED\n", phase);
        } else if (!verbose) {
            printf("      Phase %d: OK (%d entries, highest gets ~%.1f%%)\n",
                   phase, num_active,
                   100.0 * SKEW_WEIGHTS[first_active] / (double)result.total_weight);
        }
    }

    TEST_ASSERT(all_phases_ok, "One or more phases failed tolerance check");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Verify Highest Entry Always Gets ~50%
// ============================================================================

void test_highest_gets_half(void) {
    printf("    Verifying highest-weight entry gets ~50%% at each phase...\n");

    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_skew_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    configure_geometric_entries(gs);

    bool all_ok = true;

    // For each phase, the highest remaining entry should get approximately 50%
    // because weights are powers of 2: w_i = sum(w_{i+1} ... w_n) + 1
    for (int phase = 1; phase <= NUM_ENTRIES - 1; phase++) {
        int first_active = phase - 1;

        // Remove previous highest
        if (phase > 1) {
            group_scheduler_remove_entry(gs, g_entry_ids[phase - 2]);
        }

        phase_result_t result = run_phase(scheduler, gs, first_active, CYCLES_PER_PHASE);

        // Check that highest active entry got close to 50%
        double highest_ratio = (double)result.dequeue_counts[first_active] /
                               (double)result.total_dequeues;

        // Expected: weight[first_active] / total_weight ≈ 0.50
        // For geometric series: 512/1023 ≈ 0.5005, 256/511 ≈ 0.501, etc.
        double expected = (double)SKEW_WEIGHTS[first_active] / (double)result.total_weight;
        double deviation = fabs(highest_ratio - expected) / expected * 100.0;

        bool ok = (deviation < 5.0);  // Allow 5% deviation
        if (!ok) {
            all_ok = false;
        }

        printf("      Phase %d: entry %d (w=%u) got %.2f%% (expected %.2f%%) [%s]\n",
               phase, first_active, SKEW_WEIGHTS[first_active],
               highest_ratio * 100.0, expected * 100.0, ok ? "OK" : "FAIL");
    }

    TEST_ASSERT(all_ok, "Highest entry did not get expected ~50% in one or more phases");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Exponential Skew Tests ===\n\n");

    printf("Configuration:\n");
    printf("  Entries: %d\n", NUM_ENTRIES);
    printf("  Weights: 512, 256, 128, 64, 32, 16, 8, 4, 2, 1\n");
    printf("  Cycles per phase: %d\n", CYCLES_PER_PHASE);
    printf("  Tight tolerance: %.1f%%\n", TIGHT_TOLERANCE);
    printf("\n");

    printf("Running test_all_backlogged...\n");
    test_all_backlogged();

    printf("\nRunning test_progressive_removal_live...\n");
    test_progressive_removal_live();

    printf("\nRunning test_highest_gets_half...\n");
    test_highest_gets_half();

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
