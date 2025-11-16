#ifndef HWFQ_GROUP_SCHEDULER_INTERNAL_H
#define HWFQ_GROUP_SCHEDULER_INTERNAL_H

#include "hwfq_group_scheduler.h"
#include "hwfq_internal.h"
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Time Precision Configuration
// ============================================================================

// Virtual time is scaled by this factor to preserve precision in service interval
// calculations. Without scaling, integer division (work_size / rate) truncates
// for small work sizes or large rates, breaking WF2Q+ fairness.
//
// Example without scaling:
//   work_size = 1000 bytes, rate = 10 GB/sec
//   service_interval = 1000 / 10000000000 = 0 (truncated)
//
// With TIME_PRECISION_SHIFT = 20 (1M scale factor):
//   service_interval = (1000 << 20) / 10000000000 = 104 (preserves precision)
//
// TIME_PRECISION_SHIFT = 20 provides ~1 microsecond precision with typical rates
#define HWFQ_TIME_PRECISION_SHIFT 20
#define HWFQ_TIME_SCALE_FACTOR (1ULL << HWFQ_TIME_PRECISION_SHIFT)

// ============================================================================
// Internal Data Structures for Group Scheduler
// ============================================================================

// ============================================================================
// Session State
// ============================================================================

// Session state represents a queued work item in the scheduler
// This is the actual structure definition (opaque in public API)
struct session_state_t {
    // Ownership
    group_entry_id_t entry_id; // Which entry owns this session

    // WF2Q+ timing information (all times scaled by TIME_SCALE_FACTOR)
    uint64_t start_time;       // S_i (virtual start time, scaled)
    uint64_t finish_time;      // F_i (virtual finish time, scaled)
    uint64_t service_interval; // Φ_i = (L << shift) / r_i (scaled)

    // Calendar queue position
    uint32_t group_index; // Service interval group (0-15)
    uint32_t bin_index;   // Bin within group

    // User data
    void *user_data;    // User-provided data pointer
    uint64_t work_size; // L (work size in work units)

    // Linked list pointers (for sessions in same bin)
    struct session_state_t *next;
    struct session_state_t *prev;

    // Metadata
    uint64_t enqueue_time_ns; // When session was enqueued
};

// ============================================================================
// Entry Configuration
// ============================================================================

// Per-entry state and configuration
typedef struct entry_config_t {
    group_entry_id_t entry_id;
    bool configured;

    // Allocation configuration
    hwfq_allocation_t allocation; // Rate or weight-based

    // Calculated rate (for weight-based, computed dynamically)
    uint64_t calculated_rate;

    // Last finish time for this entry's sessions
    // Used to calculate start time for next session: S_i = max(V, last_F_i)
    uint64_t last_finish_time;

    // Linked list for configured entries (optimization for weight recalculation)
    struct entry_config_t *next_configured;

} entry_config_t;

// ============================================================================
// Group Scheduler Structure
// ============================================================================

// Group scheduler instance
// This implements a single-level WFQ scheduler with calendar queue
struct group_scheduler_t {
    // ========================================================================
    // Calendar Queue Data Structures
    // ========================================================================

    // Bin bitfield (one bit per bin, indicating if bin has sessions)
    // Total bins = num_groups × bins_per_group (e.g., 16 × 2048 = 32768)
    // Bitfield size = 32768 / 32 = 1024 uint32_t words
    uint32_t *bin_bitfield;

    // Session lists for each bin
    // bin_sessions[bin_index] points to head of linked list for that bin
    session_state_t **bin_sessions;

    // Hierarchical group bitfield (one bit per group of 32 bins)
    // For 32K bins, we have 1024 groups, needing 32 uint32_t words
    uint32_t group_bitfield[32];

    // ========================================================================
    // Entry Management
    // ========================================================================

    // Entry configurations (array indexed by entry_id)
    entry_config_t *entries;
    uint32_t max_entries;

    // Linked list of configured entries (for efficient iteration)
    entry_config_t *configured_entries_head;

    // Tracking for rate/weight calculations
    uint64_t allocated_rate_capacity; // Sum of rate-based allocations
    uint32_t total_weight;            // Sum of weight-based allocations

    // ========================================================================
    // Configuration
    // ========================================================================

    uint32_t num_groups;        // Number of service interval groups
    uint32_t bins_per_group;    // Bins per group
    uint32_t num_bins;          // Total bins (num_groups × bins_per_group)
    uint32_t bin_bitfield_size; // Size of bin_bitfield array

    uint64_t base_interval;  // Φ_min (minimum service interval)
    uint64_t total_capacity; // Total capacity in work units/sec

    // ========================================================================
    // WF2Q+ State
    // ========================================================================

    uint64_t virtual_time;         // V_WF2Q+(t) - current virtual time (scaled)
    uint32_t active_session_count; // Number of sessions in queue
    uint64_t min_finish_time;      // Cached minimum finish time (scaled)
    uint64_t min_start_time; // Cached minimum start time (scaled) for O(1) virtual time update

    // ========================================================================
    // Parent Context
    // ========================================================================

    // Parent scheduler (for memory allocation)
    hwfq_scheduler_t *parent;
};

// ============================================================================
// Internal Helper Functions
// ============================================================================

// Calculate effective rate for an entry
// For rate-based: returns configured rate
// For weight-based: calculates proportional share
uint64_t calculate_effective_rate(const group_scheduler_t *gs, const entry_config_t *entry);

// Calculate group index from service interval
// Groups are exponentially spaced: Group g covers [2^g × Φ_min, 2^(g+1) × Φ_min)
uint32_t calculate_group_index_from_interval(uint64_t service_interval, uint64_t base_interval);

// Calculate bin index within a group from finish time
uint32_t calculate_bin_index_from_finish_time(uint64_t finish_time, uint32_t group_index,
                                              uint32_t bins_per_group, uint64_t base_interval);

// Note: get_min_start_time() removed - now cached in gs->min_start_time

// Calendar queue operations
int calendar_insert_session(group_scheduler_t *gs, session_state_t *session);

session_state_t *calendar_find_min_session(group_scheduler_t *gs);

int calendar_remove_session(group_scheduler_t *gs, session_state_t *session);

// Bitfield operations
void set_bin_bit(uint32_t *bitfield, uint32_t bin_index);

void clear_bin_bit(uint32_t *bitfield, uint32_t bin_index);

bool test_bin_bit(const uint32_t *bitfield, uint32_t bin_index);

uint32_t find_first_set_bit(uint32_t value);

// Recalculate all weight-based entry rates
// Called when weights change (entry added/removed/reconfigured)
void recalculate_weight_based_rates(group_scheduler_t *gs);

#endif // HWFQ_GROUP_SCHEDULER_INTERNAL_H
