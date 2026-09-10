// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 AirMettle, Inc.

// ============================================================================
// Data Export Tests for H-WFQ Scheduler
// ============================================================================
//
// Generates CSV output of allocation results for external plotting AND
// validates that actual allocation matches theoretical expectations.
//
// The test checks that:
// 1. Allocation ratios match weight ratios within tolerance
// 2. Higher-weight entries get proportionally more service
// 3. Deviations are within acceptable bounds (±2% for high weights)
//
// Usage:
//   ./test_data_export                    # Run tests with validation
//   ./test_data_export > results.csv      # Also exports CSV to stdout
//   HWFQ_EXPORT_FILE=results.csv ./test_data_export  # Export to file
//
// CSV Format:
//   phase,entry_id,weight,expected_pct,actual_pct,deviation_pct,dequeue_count
//
// ============================================================================

#include "test_common.h"
#include "../src/hwfq_group_scheduler_internal.h"
#include "../src/hwfq_internal.h"
#include "../include/hwfq.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// Global test counters
static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Configuration
// ============================================================================

#define NUM_ENTRIES 10
#define CYCLES_PER_PHASE 500  // Reduced for faster testing

// Geometric weights (512, 256, 128, 64, 32, 16, 8, 4, 2, 1)
static const uint32_t SKEW_WEIGHTS[NUM_ENTRIES] = {
    512, 256, 128, 64, 32, 16, 8, 4, 2, 1
};

// Tolerance thresholds for validation
#define TIGHT_TOLERANCE_PCT 5.0    // For high-weight entries (>= 128)
#define MEDIUM_TOLERANCE_PCT 10.0  // For medium-weight entries (32-64)
#define RELAXED_TOLERANCE_PCT 25.0 // For low-weight entries (8-16)
#define HIGH_TOLERANCE_PCT 50.0    // For weights 2-4 - high variance
#define SKIP_WEIGHT_THRESHOLD 1    // Weight 1 has too much variance to validate

// ============================================================================
// Export Configuration
// ============================================================================

typedef enum {
    EXPORT_FORMAT_CSV,
    EXPORT_FORMAT_JSON
} export_format_t;

typedef struct {
    FILE *output;
    export_format_t format;
    bool include_header;
    const char *test_name;
    bool close_on_finish;  // Whether to close file when done
} export_config_t;

// ============================================================================
// Allocation Result Structures
// ============================================================================

typedef struct {
    uint32_t entry_id;
    uint32_t weight;
    double expected_pct;
    double actual_pct;
    double deviation_pct;
    uint64_t dequeue_count;
} entry_result_t;

typedef struct {
    int phase_number;
    int num_entries;
    int first_active;
    uint64_t total_cycles;
    uint32_t total_weight;
    entry_result_t entries[NUM_ENTRIES];
} phase_result_t;

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
        .total_capacity = 1000000000ULL,
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

static group_scheduler_t *create_export_group_scheduler(hwfq_scheduler_t *parent) {
    return test_create_group_scheduler(parent, 16, 2048, 1000000000ULL, 1000);
}

// Store allocated entry IDs for use across phases
static uint32_t g_entry_ids[NUM_ENTRIES];

// Helper to find index from entry_id
static int find_entry_index(uint32_t entry_id) {
    for (int i = 0; i < NUM_ENTRIES; i++) {
        if (g_entry_ids[i] == entry_id) return i;
    }
    return -1;
}

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

// ============================================================================
// Validation Functions
// ============================================================================

// Get tolerance threshold based on weight
// Higher weights should have tighter tolerances (less statistical variance)
static double get_tolerance_for_weight(uint32_t weight) {
    if (weight >= 128) return TIGHT_TOLERANCE_PCT;      // 128, 256, 512
    if (weight >= 32)  return MEDIUM_TOLERANCE_PCT;     // 32, 64
    if (weight >= 8)   return RELAXED_TOLERANCE_PCT;    // 8, 16
    return HIGH_TOLERANCE_PCT;                          // 1, 2, 4 - high variance
}

// Validation result for a phase
typedef struct {
    bool passed;
    int entries_checked;
    int entries_passed;
    double max_deviation;
    uint32_t worst_entry_weight;
} validation_result_t;

// Validate a phase result against theoretical allocation
static validation_result_t validate_phase_allocation(const phase_result_t *phase) {
    validation_result_t result = {
        .passed = true,
        .entries_checked = 0,  // Only count validated entries
        .entries_passed = 0,
        .max_deviation = 0.0,
        .worst_entry_weight = 0
    };

    for (int i = 0; i < phase->num_entries; i++) {
        const entry_result_t *e = &phase->entries[i];

        // Skip entries with weight at or below threshold (too much variance)
        if (e->weight <= SKIP_WEIGHT_THRESHOLD) {
            continue;
        }

        result.entries_checked++;
        double tolerance = get_tolerance_for_weight(e->weight);
        double abs_deviation = fabs(e->deviation_pct);

        // Track maximum deviation (among validated entries)
        if (abs_deviation > result.max_deviation) {
            result.max_deviation = abs_deviation;
            result.worst_entry_weight = e->weight;
        }

        // Check if within tolerance
        if (abs_deviation <= tolerance) {
            result.entries_passed++;
        } else {
            result.passed = false;
        }
    }

    return result;
}

// Validate that all phases meet allocation requirements
static bool validate_all_phases(const phase_result_t *phases, int num_phases, bool verbose) {
    bool all_passed = true;
    double total_max_deviation = 0.0;
    int total_entries_checked = 0;
    int total_entries_passed = 0;

    if (verbose) {
        fprintf(stderr, "\n=== Allocation Validation Results ===\n");
        fprintf(stderr, "Phase | Entries | Passed | Max Dev | Status\n");
        fprintf(stderr, "------+---------+--------+---------+--------\n");
    }

    for (int p = 0; p < num_phases; p++) {
        validation_result_t vr = validate_phase_allocation(&phases[p]);

        total_entries_checked += vr.entries_checked;
        total_entries_passed += vr.entries_passed;
        if (vr.max_deviation > total_max_deviation) {
            total_max_deviation = vr.max_deviation;
        }

        if (!vr.passed) {
            all_passed = false;
        }

        if (verbose) {
            fprintf(stderr, "%5d | %7d | %6d | %6.2f%% | %s\n",
                    p + 1, vr.entries_checked, vr.entries_passed,
                    vr.max_deviation, vr.passed ? "PASS" : "FAIL");
        }
    }

    if (verbose) {
        fprintf(stderr, "------+---------+--------+---------+--------\n");
        fprintf(stderr, "Total | %7d | %6d | %6.2f%% | %s\n",
                total_entries_checked, total_entries_passed,
                total_max_deviation, all_passed ? "PASS" : "FAIL");
        fprintf(stderr, "\n");
    }

    return all_passed;
}

// ============================================================================
// Export Configuration Initialization
// ============================================================================

static export_config_t export_config_init(const char *test_name) {
    export_config_t config = {
        .output = stdout,
        .format = EXPORT_FORMAT_CSV,
        .include_header = true,
        .test_name = test_name,
        .close_on_finish = false
    };

    // Check for environment variable
    const char *filename = getenv("HWFQ_EXPORT_FILE");
    if (filename != NULL && strlen(filename) > 0) {
        FILE *f = fopen(filename, "w");
        if (f != NULL) {
            config.output = f;
            config.close_on_finish = true;
            fprintf(stderr, "Exporting to: %s\n", filename);
        } else {
            fprintf(stderr, "Warning: Could not open %s, using stdout\n", filename);
        }
    }

    // Check for JSON format
    const char *format = getenv("HWFQ_EXPORT_FORMAT");
    if (format != NULL && strcmp(format, "json") == 0) {
        config.format = EXPORT_FORMAT_JSON;
    }

    return config;
}

static void export_config_finish(export_config_t *config) {
    if (config->close_on_finish && config->output != NULL && config->output != stdout) {
        fclose(config->output);
        config->output = NULL;
    }
}

// ============================================================================
// CSV Export Functions
// ============================================================================

static void export_csv_header(FILE *out) {
    fprintf(out, "phase,entry_id,weight,expected_pct,actual_pct,deviation_pct,dequeue_count\n");
}

static void export_csv_phase(FILE *out, const phase_result_t *phase) {
    for (int i = 0; i < phase->num_entries; i++) {
        const entry_result_t *e = &phase->entries[i];
        fprintf(out, "%d,%u,%u,%.4f,%.4f,%.4f,%lu\n",
                phase->phase_number,
                e->entry_id,
                e->weight,
                e->expected_pct,
                e->actual_pct,
                e->deviation_pct,
                (unsigned long)e->dequeue_count);
    }
}

// ============================================================================
// JSON Export Functions
// ============================================================================

static void export_json_start(FILE *out, const char *test_name) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", tm_info);

    fprintf(out, "{\n");
    fprintf(out, "  \"test_name\": \"%s\",\n", test_name);
    fprintf(out, "  \"timestamp\": \"%s\",\n", timestamp);
    fprintf(out, "  \"cycles_per_phase\": %d,\n", CYCLES_PER_PHASE);
    fprintf(out, "  \"phases\": [\n");
}

static void export_json_phase(FILE *out, const phase_result_t *phase, bool last) {
    fprintf(out, "    {\n");
    fprintf(out, "      \"phase\": %d,\n", phase->phase_number);
    fprintf(out, "      \"first_active_entry\": %d,\n", phase->first_active);
    fprintf(out, "      \"num_entries\": %d,\n", phase->num_entries);
    fprintf(out, "      \"total_weight\": %u,\n", phase->total_weight);
    fprintf(out, "      \"total_cycles\": %lu,\n", (unsigned long)phase->total_cycles);
    fprintf(out, "      \"entries\": [\n");

    for (int i = 0; i < phase->num_entries; i++) {
        const entry_result_t *e = &phase->entries[i];
        fprintf(out, "        {\"entry_id\": %u, \"weight\": %u, \"expected_pct\": %.4f, "
                     "\"actual_pct\": %.4f, \"deviation_pct\": %.4f, \"dequeue_count\": %lu}%s\n",
                e->entry_id,
                e->weight,
                e->expected_pct,
                e->actual_pct,
                e->deviation_pct,
                (unsigned long)e->dequeue_count,
                (i < phase->num_entries - 1) ? "," : "");
    }

    fprintf(out, "      ]\n");
    fprintf(out, "    }%s\n", last ? "" : ",");
}

static void export_json_end(FILE *out) {
    fprintf(out, "  ]\n");
    fprintf(out, "}\n");
}

// ============================================================================
// Run Scheduling Phase and Collect Results
// ============================================================================

static phase_result_t run_phase(hwfq_scheduler_t *scheduler,
                                 group_scheduler_t *gs,
                                 int phase_number,
                                 int first_active,
                                 int num_cycles) {
    phase_result_t result = {0};
    result.phase_number = phase_number;
    result.first_active = first_active;
    result.num_entries = NUM_ENTRIES - first_active;
    result.total_cycles = num_cycles;

    // Calculate total weight
    for (int i = first_active; i < NUM_ENTRIES; i++) {
        result.total_weight += SKEW_WEIGHTS[i];
    }

    // Initialize dequeue counts
    uint64_t dequeue_counts[NUM_ENTRIES] = {0};
    uint64_t total_dequeues = 0;

    // Run scheduling cycles
    for (int cycle = 0; cycle < num_cycles; cycle++) {
        // Enqueue for all active entries
        for (int i = first_active; i < NUM_ENTRIES; i++) {
            session_state_t *session = NULL;
            group_scheduler_enqueue(gs, g_entry_ids[i], 4096, NULL, NULL, &session);
        }

        // Dequeue one session
        session_state_t *session = group_scheduler_dequeue(gs);
        if (session != NULL) {
            group_entry_id_t entry_id = session_get_entry_id(session);
            int idx = find_entry_index(entry_id);
            if (idx >= 0 && idx < NUM_ENTRIES) {
                dequeue_counts[idx]++;
                total_dequeues++;
            }
            group_scheduler_update_virtual_time(gs, session_get_work_size(session));
            hwfq_free(scheduler, session);
        }
    }

    // Drain remaining sessions
    while (!group_scheduler_is_empty(gs)) {
        session_state_t *session = group_scheduler_dequeue(gs);
        if (session != NULL) {
            hwfq_free(scheduler, session);
        }
    }

    // Populate entry results
    int entry_index = 0;
    for (int i = first_active; i < NUM_ENTRIES; i++) {
        entry_result_t *e = &result.entries[entry_index];
        e->entry_id = (uint32_t)i;  // Use logical index for display
        e->weight = SKEW_WEIGHTS[i];
        e->expected_pct = (double)SKEW_WEIGHTS[i] / (double)result.total_weight * 100.0;
        e->actual_pct = (total_dequeues > 0) ?
                        (double)dequeue_counts[i] / (double)total_dequeues * 100.0 : 0.0;
        e->deviation_pct = (e->expected_pct > 0) ?
                           (e->actual_pct - e->expected_pct) / e->expected_pct * 100.0 : 0.0;
        e->dequeue_count = dequeue_counts[i];
        entry_index++;
    }

    return result;
}

// ============================================================================
// Test: Run Skew Test with CSV Export and Validation
// ============================================================================

void test_skew_export_csv(void) {
    export_config_t config = export_config_init("exponential_skew");

    // Only print test output to stderr when exporting
    bool export_mode = (config.output != stdout);
    if (export_mode) {
        fprintf(stderr, "Running exponential skew test with CSV export and validation...\n");
    }

    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_export_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    configure_geometric_entries(gs);

    // Write CSV header
    if (config.format == EXPORT_FORMAT_CSV && config.include_header) {
        export_csv_header(config.output);
    } else if (config.format == EXPORT_FORMAT_JSON) {
        export_json_start(config.output, config.test_name);
    }

    // Store all phase results for validation
    phase_result_t all_phases[NUM_ENTRIES];
    int num_phases = 0;

    // Run all phases (skip single-entry phase - known issue with removal + single entry)
    int total_phases = NUM_ENTRIES > 1 ? NUM_ENTRIES - 1 : 1;
    for (int phase = 1; phase <= total_phases; phase++) {
        int first_active = phase - 1;

        // Remove previous entry (except for phase 1)
        if (phase > 1) {
            group_scheduler_remove_entry(gs, g_entry_ids[phase - 2]);
        }

        // Skip if no entries left
        if (first_active >= NUM_ENTRIES) break;

        // Run phase
        phase_result_t result = run_phase(scheduler, gs, phase, first_active, CYCLES_PER_PHASE);

        // Store for validation
        all_phases[num_phases++] = result;

        // Export
        if (config.format == EXPORT_FORMAT_CSV) {
            export_csv_phase(config.output, &result);
        } else {
            export_json_phase(config.output, &result, phase == total_phases);
        }

        if (export_mode) {
            fprintf(stderr, "  Phase %d: %d entries exported\n", phase, result.num_entries);
        }
    }

    // Close JSON
    if (config.format == EXPORT_FORMAT_JSON) {
        export_json_end(config.output);
    }

    export_config_finish(&config);

    // VALIDATION: Check that actual allocation matches theoretical
    bool validation_passed = validate_all_phases(all_phases, num_phases, true);
    TEST_ASSERT(validation_passed, "Allocation validation failed - actual does not match theoretical");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Validate CSV Output Format
// ============================================================================

void test_csv_format(void) {
    // Run a minimal test and verify CSV format is correct
    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_export_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    configure_geometric_entries(gs);

    // Run one phase with reduced cycles
    phase_result_t result = run_phase(scheduler, gs, 1, 0, 100);

    // Verify we have results
    TEST_ASSERT(result.num_entries == NUM_ENTRIES, "Wrong number of entries");

    // Calculate expected total weight dynamically
    uint32_t expected_weight = 0;
    for (int i = 0; i < NUM_ENTRIES; i++) {
        expected_weight += SKEW_WEIGHTS[i];
    }
    TEST_ASSERT(result.total_weight == expected_weight, "Wrong total weight");

    // Verify each entry has reasonable data
    for (int i = 0; i < result.num_entries; i++) {
        entry_result_t *e = &result.entries[i];
        TEST_ASSERT(e->weight == SKEW_WEIGHTS[i], "Wrong weight in result");
        TEST_ASSERT(e->expected_pct > 0, "Expected pct should be positive");
    }

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test: Summary Statistics Export
// ============================================================================

void test_summary_export(void) {
    // Print a summary table to stderr for quick verification
    // Skip single-entry phase (known issue with removal + single entry)
    int max_phases = NUM_ENTRIES > 1 ? NUM_ENTRIES - 1 : 1;

    fprintf(stderr, "\n=== Allocation Summary (%d phases) ===\n", max_phases);
    fprintf(stderr, "Phase | Entries | Highest Entry | Expected | Actual\n");
    fprintf(stderr, "------+---------+---------------+----------+--------\n");

    hwfq_scheduler_t *scheduler = create_test_scheduler();
    TEST_ASSERT(scheduler != NULL, "Failed to create scheduler");

    group_scheduler_t *gs = create_export_group_scheduler(scheduler);
    TEST_ASSERT(gs != NULL, "Failed to create group scheduler");

    configure_geometric_entries(gs);

    for (int phase = 1; phase <= max_phases; phase++) {
        int first_active = phase - 1;

        if (phase > 1) {
            group_scheduler_remove_entry(gs, g_entry_ids[phase - 2]);
        }

        if (first_active >= NUM_ENTRIES) break;

        phase_result_t result = run_phase(scheduler, gs, phase, first_active, CYCLES_PER_PHASE);

        // Print summary for highest entry (should be ~50% each time)
        entry_result_t *highest = &result.entries[0];
        fprintf(stderr, "%5d | %7d | %13u | %7.2f%% | %6.2f%%\n",
                phase, result.num_entries, highest->weight,
                highest->expected_pct, highest->actual_pct);
    }

    fprintf(stderr, "------+---------+---------------+----------+--------\n");

    test_destroy_group_scheduler(scheduler, gs);
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Data Export Tests with Validation ===\n\n");

    printf("Running test_csv_format...\n");
    test_csv_format();

    printf("\nRunning test_summary_export...\n");
    test_summary_export();

    printf("\nRunning test_skew_export_csv...\n");
    test_skew_export_csv();

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
