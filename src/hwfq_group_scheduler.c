#define _POSIX_C_SOURCE 199309L
#include "hwfq_group_scheduler_internal.h"
#include "hwfq_chunked_entries.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Rate Margin Configuration
// ============================================================================

// Apply a 1/2 margin factor to effective rates for work-conserving behavior.
//
// This allows virtual time to advance more aggressively via the jump-ahead
// mechanism (V = max(V + work, min_start_time)), improving work-conserving
// behavior. The factor is applied uniformly to all entries, preserving
// fairness ratios. Within a group/tenant, allocations are mostly "relative" units.
#define HWFQ_RATE_MARGIN_SHIFT 1  // Divide by 2 (right shift by 1)

uint64_t calculate_effective_rate(const group_scheduler_t *gs, const entry_config_t *entry)
{
    uint64_t rate;

    if (entry->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
        rate = entry->allocation.rate;
    } else {
        if (gs->total_weight == 0) {
            return 0;
        }
        uint64_t available = gs->total_capacity - gs->allocated_rate_capacity;
        rate = (entry->allocation.weight * available) / gs->total_weight;
    }

    // Apply rate margin factor (divide by 2) for work-conserving behavior
    // This provides margin for virtual time advancement via jump-ahead
    rate = rate >> HWFQ_RATE_MARGIN_SHIFT;

    // Ensure rate doesn't go to zero
    if (rate == 0) {
        rate = 1;
    }

    return rate;
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

// Internal helper to initialize common group scheduler fields
static group_scheduler_t *group_scheduler_init_common(hwfq_scheduler_t *parent, uint32_t num_groups,
                                                       uint32_t bins_per_group, uint64_t total_capacity,
                                                       uint32_t max_entries)
{
    if (parent == NULL || num_groups == 0 || bins_per_group == 0 || max_entries == 0) {
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

    gs->bin_heads = (session_state_t **)hwfq_alloc(parent, sizeof(session_state_t *) * gs->num_bins);
    if (gs->bin_heads == NULL) {
        hwfq_free(parent, gs->bin_bitfield);
        hwfq_free(parent, gs);
        return NULL;
    }
    memset(gs->bin_heads, 0, sizeof(session_state_t *) * gs->num_bins);
    memset(gs->group_bitfield, 0, sizeof(gs->group_bitfield));
    gs->max_entries = max_entries;
    gs->virtual_time = 0;
    gs->active_session_count = 0;
    gs->min_finish_time = UINT64_MAX;
    gs->min_start_time = UINT64_MAX;
    gs->min_session = NULL;
    gs->allocated_rate_capacity = 0;
    gs->total_weight = 0;
    gs->configured_entries_head = NULL;
    pthread_mutex_init(&gs->lock, NULL);
    return gs;
}

group_scheduler_t *group_scheduler_init_with_entries(hwfq_scheduler_t *parent, uint32_t num_groups,
                                                       uint32_t bins_per_group, uint64_t total_capacity,
                                                       uint32_t max_entries, hwfq_chunked_entries_t *entries)
{
    if (entries == NULL) {
        return NULL;
    }

    group_scheduler_t *gs = group_scheduler_init_common(parent, num_groups, bins_per_group,
                                                         total_capacity, max_entries);
    if (gs == NULL) {
        return NULL;
    }

    gs->entries = entries;
    return gs;
}

void group_scheduler_destroy(hwfq_scheduler_t *parent, group_scheduler_t *gs)
{
    if (parent == NULL || gs == NULL) {
        return;
    }

    // Note: Entries are NOT freed here - they are owned by the parent
    // (tenant or system) and freed when chunked_entries is destroyed.
    // Just clear the configured list pointer.
    gs->configured_entries_head = NULL;

    // Free all remaining sessions in the bins
    if (gs->bin_heads != NULL) {
        uint32_t total_bins = gs->num_groups * gs->bins_per_group;
        for (uint32_t i = 0; i < total_bins; i++) {
            session_state_t *session = gs->bin_heads[i];
            while (session != NULL) {
                session_state_t *next = session->bin_next;
                if (session->cleanup_fn != NULL) {
                    session->cleanup_fn(parent, session->user_data);
                }
                hwfq_free(parent, session);
                session = next;
            }
        }
        hwfq_free(parent, gs->bin_heads);
    }
    if (gs->bin_bitfield != NULL) {
        hwfq_free(parent, gs->bin_bitfield);
    }

    pthread_mutex_destroy(&gs->lock);
    hwfq_free(parent, gs);
}

entry_config_t *group_scheduler_find_entry(group_scheduler_t *gs, group_entry_id_t entry_id)
{
    if (gs == NULL || gs->entries == NULL) {
        return NULL;
    }

    entry_config_t *entry = hwfq_chunked_get(gs->entries, entry_id);
    if (entry != NULL && entry->configured) {
        return entry;
    }
    return NULL;
}

entry_config_t *group_scheduler_get_entry(group_scheduler_t *gs, group_entry_id_t entry_id)
{
    if (gs == NULL || gs->entries == NULL) {
        return NULL;
    }
    return hwfq_chunked_get(gs->entries, entry_id);
}

int group_scheduler_configure_entry(group_scheduler_t *gs, const group_entry_config_t *config)
{
    if (gs == NULL || config == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&gs->lock);

    if (config->entry_id >= gs->max_entries) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_INVALID_ARG;
    }
    if (config->allocation.allocation_type != HWFQ_ALLOCATION_RATE &&
        config->allocation.allocation_type != HWFQ_ALLOCATION_WEIGHT) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_INVALID_ARG;
    }
    if (config->allocation.allocation_type == HWFQ_ALLOCATION_RATE &&
        config->allocation.rate == 0) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_INVALID_ARG;
    }
    if (config->allocation.allocation_type == HWFQ_ALLOCATION_WEIGHT &&
        config->allocation.weight == 0) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_INVALID_ARG;
    }

    entry_config_t *entry = group_scheduler_get_entry(gs, config->entry_id);
    if (entry == NULL) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

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
            pthread_mutex_unlock(&gs->lock);
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

    pthread_mutex_unlock(&gs->lock);
    return HWFQ_SUCCESS;
}

int group_scheduler_remove_entry(group_scheduler_t *gs, group_entry_id_t entry_id)
{
    if (gs == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&gs->lock);

    if (entry_id >= gs->max_entries) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_INVALID_ARG;
    }

    entry_config_t *entry = group_scheduler_find_entry(gs, entry_id);
    if (entry == NULL || !entry->configured) {
        pthread_mutex_unlock(&gs->lock);
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

    // Mark as unconfigured (caller is responsible for freeing via chunked_entries_free)
    entry->configured = false;
    entry->next_configured = NULL;

    recalculate_weight_based_rates(gs);

    pthread_mutex_unlock(&gs->lock);
    return HWFQ_SUCCESS;
}

int group_scheduler_enqueue(group_scheduler_t *gs, group_entry_id_t entry_id, uint64_t work_size,
                            void *user_data, group_session_cleanup_fn cleanup_fn,
                            session_state_t **session_out)
{
    if (gs == NULL || work_size == 0) {
        return HWFQ_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&gs->lock);

    if (entry_id >= gs->max_entries) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_INVALID_ARG;
    }

    entry_config_t *entry = group_scheduler_find_entry(gs, entry_id);
    if (entry == NULL || !entry->configured) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_NOT_FOUND;
    }
    uint64_t rate = (entry->allocation.allocation_type == HWFQ_ALLOCATION_RATE)
                        ? entry->calculated_rate
                        : calculate_effective_rate(gs, entry);
    if (rate == 0) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_INTERNAL;
    }

    // Calculate service interval: Φ_i = (L << shift) / r_i
    // Scale work_size to preserve precision in integer division
    // Without scaling, small work_size or large rate would truncate to 0
    // Check for overflow before shifting - cap at UINT64_MAX if too large
    uint64_t scaled_work;
    if (work_size > (UINT64_MAX >> HWFQ_TIME_PRECISION_SHIFT)) {
        scaled_work = UINT64_MAX;  // Prevent overflow for very large work sizes
    } else {
        scaled_work = work_size << HWFQ_TIME_PRECISION_SHIFT;
    }
    uint64_t service_interval = scaled_work / rate;
    if (service_interval == 0) {
        service_interval = 1;
    }
    session_state_t *session = (session_state_t *)hwfq_alloc(gs->parent, sizeof(session_state_t));
    if (session == NULL) {
        pthread_mutex_unlock(&gs->lock);
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
    session->cleanup_fn = cleanup_fn;
    session->group_index = calculate_group_index_from_interval(service_interval, gs->base_interval);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    session->enqueue_time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    int ret = calendar_insert_session(gs, session);
    if (ret != HWFQ_SUCCESS) {
        hwfq_free(gs->parent, session);
        pthread_mutex_unlock(&gs->lock);
        return ret;
    }
    entry->last_finish_time = session->finish_time;
    gs->active_session_count++;
    if (session_out != NULL) {
        *session_out = session;
    }

    pthread_mutex_unlock(&gs->lock);
    return HWFQ_SUCCESS;
}

session_state_t *group_scheduler_dequeue(group_scheduler_t *gs)
{
    if (gs == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&gs->lock);

    if (gs->active_session_count == 0) {
        pthread_mutex_unlock(&gs->lock);
        return NULL;
    }
    // calendar_find_min_session returns the eligible session with minimum finish time
    // It also handles advancing virtual_time if no sessions are currently eligible
    session_state_t *session = calendar_find_min_session(gs);
    if (session == NULL) {
        pthread_mutex_unlock(&gs->lock);
        return NULL;
    }
    int ret = calendar_remove_session(gs, session);
    if (ret != HWFQ_SUCCESS) {
        pthread_mutex_unlock(&gs->lock);
        return NULL;
    }
    gs->active_session_count--;

    pthread_mutex_unlock(&gs->lock);
    return session;
}

int group_scheduler_remove_session(group_scheduler_t *gs, session_state_t *session)
{
    if (gs == NULL || session == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&gs->lock);

    int ret = calendar_remove_session(gs, session);
    if (ret != HWFQ_SUCCESS) {
        pthread_mutex_unlock(&gs->lock);
        return ret;
    }
    gs->active_session_count--;
    hwfq_free(gs->parent, session);

    pthread_mutex_unlock(&gs->lock);
    return HWFQ_SUCCESS;
}

uint64_t group_scheduler_get_virtual_time(group_scheduler_t *gs)
{
    if (gs == NULL) {
        return 0;
    }
    pthread_mutex_lock(&gs->lock);
    uint64_t vt = gs->virtual_time;
    pthread_mutex_unlock(&gs->lock);
    return vt;
}

// Rebase all virtual timestamps to prevent overflow.
// This function shifts all timestamps down by a calculated offset, preserving
// relative ordering. Must be called with exclusive access to the scheduler
// (NOT thread-safe - caller must hold any external lock).
//
// The rebase is designed to be consistent: all timestamps are updated atomically
// from the scheduler's logical perspective. No partial state is visible.
void group_scheduler_rebase_if_needed(group_scheduler_t *gs)
{
    if (gs == NULL || gs->virtual_time < HWFQ_REBASE_THRESHOLD) {
        return;
    }

    // Calculate offset: rebase to half of current virtual_time
    // This gives maximum headroom before next rebase
    uint64_t rebase_offset = gs->virtual_time / 2;

    // Phase 1: Rebase scheduler-level cached values
    gs->virtual_time -= rebase_offset;

    if (gs->min_start_time != UINT64_MAX) {
        gs->min_start_time = (gs->min_start_time >= rebase_offset)
                                 ? (gs->min_start_time - rebase_offset)
                                 : 0;
    }

    if (gs->min_finish_time != UINT64_MAX) {
        gs->min_finish_time = (gs->min_finish_time >= rebase_offset)
                                  ? (gs->min_finish_time - rebase_offset)
                                  : 0;
    }

    // Phase 2: Rebase all sessions in calendar queue
    // Iterate through all bins (most will be empty, skipped quickly)
    for (uint32_t bin = 0; bin < gs->num_bins; bin++) {
        session_state_t *session = gs->bin_heads[bin];
        while (session != NULL) {
            session->start_time = (session->start_time >= rebase_offset)
                                      ? (session->start_time - rebase_offset)
                                      : 0;
            session->finish_time = (session->finish_time >= rebase_offset)
                                       ? (session->finish_time - rebase_offset)
                                       : 0;
            session = session->bin_next;
        }
    }

    // Phase 3: Rebase all entry last_finish_times
    entry_config_t *entry = gs->configured_entries_head;
    while (entry != NULL) {
        entry->last_finish_time = (entry->last_finish_time >= rebase_offset)
                                      ? (entry->last_finish_time - rebase_offset)
                                      : 0;
        entry = entry->next_configured;
    }
}

void group_scheduler_update_virtual_time(group_scheduler_t *gs, uint64_t work_completed)
{
    if (gs == NULL) {
        return;
    }

    pthread_mutex_lock(&gs->lock);

    // In WF2Q+, virtual time advances based on the aggregate work done.
    // When servicing a backlogged system, V advances at rate equal to the
    // total system capacity. The scaling factor converts work to virtual time.
    //
    // For proper fairness, we compute Δt = W / total_capacity (in virtual time units).
    // With precision scaling: Δt = (W << shift) / total_capacity
    //
    // This ensures that entries with higher rates get more opportunities
    // to be scheduled (smaller service intervals = smaller finish times).
    uint64_t scaled_work = work_completed << HWFQ_TIME_PRECISION_SHIFT;

    // Advance virtual time by work / capacity (scaled)
    uint64_t delta;
    if (gs->total_capacity > 0) {
        delta = scaled_work / gs->total_capacity;
        if (delta == 0) {
            delta = 1;  // Minimum advancement
        }
    } else {
        delta = scaled_work;  // Fallback
    }

    uint64_t normal_advance;
    if (gs->virtual_time > UINT64_MAX - delta) {
        normal_advance = UINT64_MAX;
    } else {
        normal_advance = gs->virtual_time + delta;
    }

    // Jump-ahead: min{S_i} across all backlogged sessions (now cached, O(1))
    uint64_t min_start = gs->min_start_time;

    // V_WF2Q+(t + Δt) = max(V_WF2Q+(t) + Δt, min{S_i})
    gs->virtual_time = (normal_advance > min_start) ? normal_advance : min_start;

    group_scheduler_rebase_if_needed(gs);
    pthread_mutex_unlock(&gs->lock);
}

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

uint32_t group_scheduler_get_session_count(group_scheduler_t *gs)
{
    if (gs == NULL) {
        return 0;
    }
    pthread_mutex_lock(&gs->lock);
    uint32_t count = gs->active_session_count;
    pthread_mutex_unlock(&gs->lock);
    return count;
}

bool group_scheduler_is_empty(group_scheduler_t *gs)
{
    if (gs == NULL) {
        return true;
    }
    pthread_mutex_lock(&gs->lock);
    bool empty = (gs->active_session_count == 0);
    pthread_mutex_unlock(&gs->lock);
    return empty;
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

    pthread_mutex_lock(&gs->lock);

    entry_config_t *entry = group_scheduler_find_entry(gs, entry_id);
    if (entry == NULL || !entry->configured) {
        pthread_mutex_unlock(&gs->lock);
        return HWFQ_ERR_NOT_FOUND;
    }
    if (entry->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
        *rate_out = entry->calculated_rate;
    } else {
        *rate_out = calculate_effective_rate(gs, entry);
    }
    pthread_mutex_unlock(&gs->lock);
    return HWFQ_SUCCESS;
}
