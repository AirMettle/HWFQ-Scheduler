// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 AirMettle, Inc.

#ifndef HWFQ_GROUP_SCHEDULER_INTERNAL_H
#define HWFQ_GROUP_SCHEDULER_INTERNAL_H

#include "hwfq_group_scheduler.h"
#include "hwfq_chunked_entries.h"
#include "hwfq_internal.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

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

// Virtual time rebasing threshold to prevent overflow.
// At 10 GB/s with 4K blocks (~2.5M blocks/s), overflow would occur after ~79 days.
// Rebasing triggers when virtual_time exceeds this threshold, shifting all timestamps
// down to preserve relative ordering while preventing overflow.
#define HWFQ_REBASE_THRESHOLD (UINT64_MAX / 2)

// Internal cleanup callback for session user_data during destroy
// Called with parent scheduler for access to free_fn
typedef void (*session_cleanup_fn)(hwfq_scheduler_t *parent, void *user_data);

struct session_state_t {
    group_entry_id_t entry_id;
    uint64_t start_time;
    uint64_t finish_time;
    uint64_t service_interval;
    uint32_t group_index;
    uint32_t bin_index;
    void *user_data;
    uint64_t work_size;
    struct session_state_t *bin_next;
    struct session_state_t *bin_prev;
    uint64_t enqueue_time_ns;
    session_cleanup_fn cleanup_fn;
};

typedef struct entry_rate_tracking_t {
    uint64_t window_start_ns;
    uint64_t window_work_units;
} entry_rate_tracking_t;

typedef struct entry_config_t {
    group_entry_id_t entry_id;
    bool configured;
    hwfq_allocation_t allocation;
    uint64_t calculated_rate;
    uint64_t last_finish_time;
    struct entry_config_t *next_configured;
    hwfq_flow_stats_t stats;
    entry_rate_tracking_t rate_tracking;
} entry_config_t;

struct group_scheduler_t {
    uint32_t *bin_bitfield;
    session_state_t **bin_heads;
    uint32_t group_bitfield[32];
    hwfq_chunked_entries_t *entries;
    uint32_t max_entries;
    entry_config_t *configured_entries_head;
    uint64_t allocated_rate_capacity;
    uint32_t total_weight;
    uint32_t num_groups;
    uint32_t bins_per_group;
    uint32_t num_bins;
    uint32_t bin_bitfield_size;
    uint64_t base_interval;
    uint64_t total_capacity;
    uint64_t virtual_time;
    uint32_t active_session_count;
    uint64_t min_finish_time;
    uint64_t min_start_time;
    session_state_t *min_session;
    pthread_mutex_t lock;
    hwfq_scheduler_t *parent;
};

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

// Virtual time rebasing to prevent overflow
// Shifts all timestamps down when virtual_time exceeds HWFQ_REBASE_THRESHOLD
void group_scheduler_rebase_if_needed(group_scheduler_t *gs);

// Bitfield operations
void set_bin_bit(uint32_t *bitfield, uint32_t bin_index);

void clear_bin_bit(uint32_t *bitfield, uint32_t bin_index);

bool test_bin_bit(const uint32_t *bitfield, uint32_t bin_index);

uint32_t find_first_set_bit(uint32_t value);

// Recalculate all weight-based entry rates
// Called when weights change (entry added/removed/reconfigured)
void recalculate_weight_based_rates(group_scheduler_t *gs);

// Find entry by ID via direct chunked lookup
// Returns NULL if entry not found/not configured
entry_config_t *group_scheduler_find_entry(group_scheduler_t *gs, group_entry_id_t entry_id);

// Get entry by ID (assumes entry_id was allocated via hwfq_chunked_entries_alloc)
// Returns NULL if entry not found
entry_config_t *group_scheduler_get_entry(group_scheduler_t *gs, group_entry_id_t entry_id);

#endif // HWFQ_GROUP_SCHEDULER_INTERNAL_H
