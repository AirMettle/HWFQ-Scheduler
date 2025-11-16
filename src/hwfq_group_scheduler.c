#define _POSIX_C_SOURCE 199309L

#include "hwfq_group_scheduler_internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Helper Functions for Rate Calculations
// ============================================================================

uint64_t calculate_effective_rate(const group_scheduler_t *gs, const entry_config_t *entry)
{
    if (entry->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
        return entry->allocation.rate;
    }
    if (gs->total_weight == 0) {
        return 0;
    }
    uint64_t available = gs->total_capacity - gs->allocated_rate_capacity;
    return (entry->allocation.weight * available) / gs->total_weight;
}

void recalculate_weight_based_rates(group_scheduler_t *gs)
{
    entry_config_t *entry = gs->configured_entries_head;
    while (entry != NULL) {
        if (entry->allocation.allocation_type == HWFQ_ALLOCATION_WEIGHT) {
            entry->calculated_rate = calculate_effective_rate(gs, entry);
        }
        entry = entry->next_configured;
    }
}

// ============================================================================
// Group Scheduler Lifecycle
// ============================================================================

group_scheduler_t *group_scheduler_init(hwfq_scheduler_t *parent, uint32_t num_groups,
                                        uint32_t bins_per_group, uint64_t total_capacity)
{
    if (parent == NULL || num_groups == 0 || bins_per_group == 0) {
        return NULL;
    }
    group_scheduler_t *gs = (group_scheduler_t *)hwfq_alloc(parent, sizeof(group_scheduler_t));
    if (gs == NULL) {
        return NULL;
    }
    memset(gs, 0, sizeof(group_scheduler_t));
    gs->parent = parent;
    gs->num_groups = num_groups;
    gs->bins_per_group = bins_per_group;
    gs->num_bins = num_groups * bins_per_group;
    gs->total_capacity = total_capacity;
    gs->base_interval = 1;
    gs->bin_bitfield_size = (gs->num_bins + 31) / 32;
    gs->bin_bitfield = (uint32_t *)hwfq_alloc(parent, sizeof(uint32_t) * gs->bin_bitfield_size);
    if (gs->bin_bitfield == NULL) {
        hwfq_free(parent, gs);
        return NULL;
    }
    memset(gs->bin_bitfield, 0, sizeof(uint32_t) * gs->bin_bitfield_size);
    gs->bin_sessions =
        (session_state_t **)hwfq_alloc(parent, sizeof(session_state_t *) * gs->num_bins);
    if (gs->bin_sessions == NULL) {
        hwfq_free(parent, gs->bin_bitfield);
        hwfq_free(parent, gs);
        return NULL;
    }
    memset(gs->bin_sessions, 0, sizeof(session_state_t *) * gs->num_bins);
    memset(gs->group_bitfield, 0, sizeof(gs->group_bitfield));
    gs->max_entries = parent->config.max_tenants;
    gs->entries = (entry_config_t *)hwfq_alloc(parent, sizeof(entry_config_t) * gs->max_entries);
    if (gs->entries == NULL) {
        hwfq_free(parent, gs->bin_sessions);
        hwfq_free(parent, gs->bin_bitfield);
        hwfq_free(parent, gs);
        return NULL;
    }
    memset(gs->entries, 0, sizeof(entry_config_t) * gs->max_entries);
    gs->virtual_time = 0;
    gs->active_session_count = 0;
    gs->min_finish_time = UINT64_MAX;
    gs->min_start_time = UINT64_MAX;
    gs->allocated_rate_capacity = 0;
    gs->total_weight = 0;
    gs->configured_entries_head = NULL;
    return gs;
}

void group_scheduler_destroy(hwfq_scheduler_t *parent, group_scheduler_t *gs)
{
    if (parent == NULL || gs == NULL) {
        return;
    }
    if (gs->entries != NULL) {
        hwfq_free(parent, gs->entries);
    }
    if (gs->bin_sessions != NULL) {
        hwfq_free(parent, gs->bin_sessions);
    }
    if (gs->bin_bitfield != NULL) {
        hwfq_free(parent, gs->bin_bitfield);
    }
    hwfq_free(parent, gs);
}

// ============================================================================
// Entry Configuration
// ============================================================================

int group_scheduler_configure_entry(group_scheduler_t *gs, const group_entry_config_t *config)
{
    if (gs == NULL || config == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (config->entry_id >= gs->max_entries) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (config->allocation.allocation_type != HWFQ_ALLOCATION_RATE &&
        config->allocation.allocation_type != HWFQ_ALLOCATION_WEIGHT) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (config->allocation.allocation_type == HWFQ_ALLOCATION_RATE &&
        config->allocation.rate == 0) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (config->allocation.allocation_type == HWFQ_ALLOCATION_WEIGHT &&
        config->allocation.weight == 0) {
        return HWFQ_ERR_INVALID_ARG;
    }
    entry_config_t *entry = &gs->entries[config->entry_id];
    if (entry->configured) {
        if (entry->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
            gs->allocated_rate_capacity -= entry->allocation.rate;
        } else {
            gs->total_weight -= entry->allocation.weight;
        }
    }
    if (config->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
        uint64_t new_total = gs->allocated_rate_capacity + config->allocation.rate;
        if (new_total > gs->total_capacity) {
            if (entry->configured) {
                if (entry->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
                    gs->allocated_rate_capacity += entry->allocation.rate;
                } else {
                    gs->total_weight += entry->allocation.weight;
                }
            }
            return HWFQ_ERR_OVERBOOKED;
        }
        gs->allocated_rate_capacity += config->allocation.rate;
        entry->calculated_rate = config->allocation.rate;
    } else {
        gs->total_weight += config->allocation.weight;
        entry->calculated_rate = 0;
    }
    entry->entry_id = config->entry_id;
    entry->allocation = config->allocation;
    entry->last_finish_time = 0;
    if (!entry->configured) {
        entry->next_configured = gs->configured_entries_head;
        gs->configured_entries_head = entry;
        entry->configured = true;
    }
    recalculate_weight_based_rates(gs);
    return HWFQ_SUCCESS;
}

int group_scheduler_remove_entry(group_scheduler_t *gs, group_entry_id_t entry_id)
{
    if (gs == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (entry_id >= gs->max_entries) {
        return HWFQ_ERR_INVALID_ARG;
    }
    entry_config_t *entry = &gs->entries[entry_id];
    if (!entry->configured) {
        return HWFQ_ERR_NOT_FOUND;
    }
    if (entry->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
        gs->allocated_rate_capacity -= entry->allocation.rate;
    } else {
        gs->total_weight -= entry->allocation.weight;
    }
    if (gs->configured_entries_head == entry) {
        gs->configured_entries_head = entry->next_configured;
    } else {
        entry_config_t *prev = gs->configured_entries_head;
        while (prev != NULL && prev->next_configured != entry) {
            prev = prev->next_configured;
        }
        if (prev != NULL) {
            prev->next_configured = entry->next_configured;
        }
    }
    entry->configured = false;
    entry->calculated_rate = 0;
    entry->last_finish_time = 0;
    entry->next_configured = NULL;
    recalculate_weight_based_rates(gs);
    return HWFQ_SUCCESS;
}

// ============================================================================
// Session Scheduling - Enqueue
// ============================================================================

int group_scheduler_enqueue(group_scheduler_t *gs, group_entry_id_t entry_id, uint64_t work_size,
                            void *user_data, session_state_t **session_out)
{
    if (gs == NULL || work_size == 0) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (entry_id >= gs->max_entries) {
        return HWFQ_ERR_INVALID_ARG;
    }
    entry_config_t *entry = &gs->entries[entry_id];
    if (!entry->configured) {
        return HWFQ_ERR_NOT_FOUND;
    }
    uint64_t rate = (entry->allocation.allocation_type == HWFQ_ALLOCATION_RATE)
                        ? entry->calculated_rate
                        : calculate_effective_rate(gs, entry);
    if (rate == 0) {
        return HWFQ_ERR_INTERNAL;
    }

    // Calculate service interval: Φ_i = (L << shift) / r_i
    // Scale work_size to preserve precision in integer division
    // Without scaling, small work_size or large rate would truncate to 0
    uint64_t scaled_work = work_size << HWFQ_TIME_PRECISION_SHIFT;
    uint64_t service_interval = scaled_work / rate;
    if (service_interval == 0) {
        service_interval = 1;
    }
    session_state_t *session = (session_state_t *)hwfq_alloc(gs->parent, sizeof(session_state_t));
    if (session == NULL) {
        return HWFQ_ERR_NO_MEMORY;
    }
    memset(session, 0, sizeof(session_state_t));

    // Calculate WF2Q+ times
    // S_i = max(V_WF2Q+(t), last_F_i for this entry)
    session->start_time =
        (gs->virtual_time > entry->last_finish_time) ? gs->virtual_time : entry->last_finish_time;
    session->service_interval = service_interval;

    // F_i = S_i + Φ_i
    // Check for overflow before adding
    if (session->start_time > UINT64_MAX - session->service_interval) {
        session->finish_time = UINT64_MAX;
    } else {
        session->finish_time = session->start_time + session->service_interval;
    }
    session->entry_id = entry_id;
    session->work_size = work_size;
    session->user_data = user_data;
    session->group_index = calculate_group_index_from_interval(service_interval, gs->base_interval);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    session->enqueue_time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    int ret = calendar_insert_session(gs, session);
    if (ret != HWFQ_SUCCESS) {
        hwfq_free(gs->parent, session);
        return ret;
    }
    entry->last_finish_time = session->finish_time;
    gs->active_session_count++;
    if (session_out != NULL) {
        *session_out = session;
    }
    return HWFQ_SUCCESS;
}

// ============================================================================
// Session Scheduling - Dequeue
// ============================================================================

session_state_t *group_scheduler_dequeue(group_scheduler_t *gs)
{
    if (gs == NULL || gs->active_session_count == 0) {
        return NULL;
    }
    session_state_t *session = calendar_find_min_session(gs);
    if (session == NULL) {
        return NULL;
    }
    // Check eligibility: S_i ≤ V_WF2Q+(t)
    if (session->start_time > gs->virtual_time) {
        // Session not eligible yet; advance virtual time to make it eligible
        gs->virtual_time = session->start_time;
    }
    int ret = calendar_remove_session(gs, session);
    if (ret != HWFQ_SUCCESS) {
        return NULL;
    }
    gs->active_session_count--;
    return session;
}

// ============================================================================
// Session Scheduling - Remove
// ============================================================================

int group_scheduler_remove_session(group_scheduler_t *gs, session_state_t *session)
{
    if (gs == NULL || session == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    int ret = calendar_remove_session(gs, session);
    if (ret != HWFQ_SUCCESS) {
        return ret;
    }
    gs->active_session_count--;
    hwfq_free(gs->parent, session);
    return HWFQ_SUCCESS;
}

// ============================================================================
// Virtual Time Management
// ============================================================================

uint64_t group_scheduler_get_virtual_time(group_scheduler_t *gs)
{
    if (gs == NULL) {
        return 0;
    }
    return gs->virtual_time;
}

void group_scheduler_update_virtual_time(group_scheduler_t *gs, uint64_t work_completed)
{
    if (gs == NULL) {
        return;
    }
    uint64_t scaled_work = work_completed << HWFQ_TIME_PRECISION_SHIFT;
    uint64_t normal_advance;
    if (gs->virtual_time > UINT64_MAX - scaled_work) {
        normal_advance = UINT64_MAX;
    } else {
        normal_advance = gs->virtual_time + scaled_work;
    }

    // Jump-ahead: min{S_i} across all backlogged sessions (now cached, O(1))
    uint64_t min_start = gs->min_start_time;

    // V_WF2Q+(t + Δt) = max(V_WF2Q+(t) + Δt, min{S_i})
    gs->virtual_time = (normal_advance > min_start) ? normal_advance : min_start;
}

// ============================================================================
// Session Data Access
// ============================================================================

group_entry_id_t session_get_entry_id(const session_state_t *session)
{
    return (session != NULL) ? session->entry_id : 0;
}

void *session_get_user_data(const session_state_t *session)
{
    return (session != NULL) ? session->user_data : NULL;
}

uint64_t session_get_work_size(const session_state_t *session)
{
    return (session != NULL) ? session->work_size : 0;
}

uint64_t session_get_finish_time(const session_state_t *session)
{
    return (session != NULL) ? session->finish_time : 0;
}

// ============================================================================
// Utility Functions
// ============================================================================

uint32_t group_scheduler_get_session_count(group_scheduler_t *gs)
{
    return (gs != NULL) ? gs->active_session_count : 0;
}

bool group_scheduler_is_empty(group_scheduler_t *gs)
{
    return (gs != NULL) ? (gs->active_session_count == 0) : true;
}

int group_scheduler_get_entry_rate(group_scheduler_t *gs, group_entry_id_t entry_id,
                                   uint64_t *rate_out)
{
    if (gs == NULL || rate_out == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (entry_id >= gs->max_entries) {
        return HWFQ_ERR_INVALID_ARG;
    }
    entry_config_t *entry = &gs->entries[entry_id];

    if (!entry->configured) {
        return HWFQ_ERR_NOT_FOUND;
    }
    if (entry->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
        *rate_out = entry->calculated_rate;
    } else {
        *rate_out = calculate_effective_rate(gs, entry);
    }
    return HWFQ_SUCCESS;
}
