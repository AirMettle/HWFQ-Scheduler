#include "hwfq_group_scheduler_internal.h"
#include <stdint.h>

// ============================================================================
// Bitfield Operations
// ============================================================================

void set_bin_bit(uint32_t *bitfield, uint32_t bin_index)
{
    uint32_t word_index = bin_index / 32;
    uint32_t bit_offset = bin_index % 32;
    bitfield[word_index] |= (1U << bit_offset);
}

void clear_bin_bit(uint32_t *bitfield, uint32_t bin_index)
{
    uint32_t word_index = bin_index / 32;
    uint32_t bit_offset = bin_index % 32;
    bitfield[word_index] &= ~(1U << bit_offset);
}

bool test_bin_bit(const uint32_t *bitfield, uint32_t bin_index)
{
    uint32_t word_index = bin_index / 32;
    uint32_t bit_offset = bin_index % 32;
    return (bitfield[word_index] & (1U << bit_offset)) != 0;
}

uint32_t find_first_set_bit(uint32_t value)
{
    if (value == 0) {
        return 32;
    }
    return (uint32_t)__builtin_ctz(value); // Count trailing zeros
}

// ============================================================================
// Group and Bin Index Calculations
// ============================================================================

// Calculate group index from service interval using CLZ
//
// Groups are exponentially spaced with factor of 2:
// - Group 0: [Φ_min, 2×Φ_min)
// - Group 1: [2×Φ_min, 4×Φ_min)
// - Group g: [2^g × Φ_min, 2^(g+1) × Φ_min)
uint32_t calculate_group_index_from_interval(uint64_t service_interval, uint64_t base_interval)
{
    if (base_interval == 0) {
        return 0; // Avoid division by zero
    }

    if (service_interval <= base_interval) {
        return 0; // Minimum group - handles equal and smaller intervals
    }

    // Calculate ratio and find MSB position
    // For service_interval > base_interval, ratio is >= 2
    // We want floor(log2(ratio)) which is the position of the MSB
    uint64_t ratio = service_interval / base_interval;

    // Find position of most significant bit using CLZ
    // 63 - clz(ratio) gives us the MSB position (0-indexed)
    uint32_t group = (uint32_t)(63 - __builtin_clzll(ratio));

    // Clamp to maximum group (15 for 16 groups)
    return (group > 15) ? 15 : group;
}

// Calculate bin index within a group from finish time
uint32_t calculate_bin_index_from_finish_time(uint64_t finish_time, uint32_t group_index,
                                              uint32_t bins_per_group, uint64_t base_interval)
{
    if (bins_per_group == 0 || base_interval == 0) {
        return 0;
    }

    // Calculate the range of finish times this group covers
    // Group g covers interval range: [2^g × Φ_min, 2^(g+1) × Φ_min)
    uint64_t group_interval_range = base_interval << (group_index + 1);

    // Calculate bin size for this group
    uint64_t bin_size = group_interval_range / bins_per_group;
    if (bin_size == 0) {
        bin_size = 1;
    }

    // Map finish time to bin within the group
    // We use modulo to wrap around the calendar
    uint64_t offset_in_group = finish_time % group_interval_range;
    uint32_t bin = (uint32_t)(offset_in_group / bin_size);

    // Clamp to valid range
    if (bin >= bins_per_group) {
        bin = bins_per_group - 1;
    }

    return bin;
}

// ============================================================================
// Calendar Queue Operations
// ============================================================================

int calendar_insert_session(group_scheduler_t *gs, session_state_t *session)
{
    if (gs == NULL || session == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }

    // Calculate bin index from session's group and finish time
    uint32_t bin_offset = calculate_bin_index_from_finish_time(
        session->finish_time, session->group_index, gs->bins_per_group, gs->base_interval);

    // Absolute bin index
    uint32_t bin_index = session->group_index * gs->bins_per_group + bin_offset;

    if (bin_index >= gs->num_bins) {
        return HWFQ_ERR_INTERNAL;
    }

    session->bin_index = bin_index;

    // Insert at head of linked list for this bin
    session->next = gs->bin_sessions[bin_index];
    session->prev = NULL;

    if (gs->bin_sessions[bin_index] != NULL) {
        gs->bin_sessions[bin_index]->prev = session;
    }

    gs->bin_sessions[bin_index] = session;

    // Update bitfields
    set_bin_bit(gs->bin_bitfield, bin_index);

    // Update group bitfield (groups of 32 bins)
    uint32_t group_bitfield_index = bin_index / 32;
    uint32_t group_word = group_bitfield_index / 32;
    uint32_t group_bit = group_bitfield_index % 32;

    if (group_word < 32) {
        gs->group_bitfield[group_word] |= (1U << group_bit);
    }

    // Update cached minimum finish time and start time for O(1) lookups
    if (session->finish_time < gs->min_finish_time) {
        gs->min_finish_time = session->finish_time;
    }

    if (session->start_time < gs->min_start_time) {
        gs->min_start_time = session->start_time;
    }

    return HWFQ_SUCCESS;
}

session_state_t *calendar_find_min_session(group_scheduler_t *gs)
{
    if (gs == NULL || gs->active_session_count == 0) {
        return NULL;
    }

    // Use hierarchical bitfields to efficiently find minimum finish time
    // We need to scan all non-empty bins to find the true minimum

    session_state_t *global_min = NULL;
    uint64_t min_finish_time = UINT64_MAX;

    // Level 1: Scan group bitfield to find non-empty groups
    for (uint32_t group_word_idx = 0; group_word_idx < 32; group_word_idx++) {
        if (gs->group_bitfield[group_word_idx] == 0) {
            continue; // Skip empty groups
        }

        // Found a group with sessions - scan its bins
        uint32_t start_bin = group_word_idx * 32 * 32;
        uint32_t end_bin = start_bin + (32 * 32);
        if (end_bin > gs->num_bins) {
            end_bin = gs->num_bins;
        }

        // Level 2: Scan bins within this group
        for (uint32_t bin = start_bin; bin < end_bin; bin++) {
            if (!test_bin_bit(gs->bin_bitfield, bin)) {
                continue; // Skip empty bins
            }

            session_state_t *head = gs->bin_sessions[bin];
            if (head == NULL) {
                continue;
            }

            // Find minimum finish time in this bin
            session_state_t *curr = head;
            while (curr != NULL) {
                if (curr->finish_time < min_finish_time) {
                    min_finish_time = curr->finish_time;
                    global_min = curr;
                }
                curr = curr->next;
            }
        }
    }

    return global_min;
}

int calendar_remove_session(group_scheduler_t *gs, session_state_t *session)
{
    if (gs == NULL || session == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }

    uint32_t bin_index = session->bin_index;
    if (bin_index >= gs->num_bins) {
        return HWFQ_ERR_INTERNAL;
    }

    // Remove from linked list
    if (session->prev != NULL) {
        session->prev->next = session->next;
    } else {
        // This was the head of the list
        gs->bin_sessions[bin_index] = session->next;
    }

    if (session->next != NULL) {
        session->next->prev = session->prev;
    }

    // Update bitfields if bin is now empty
    if (gs->bin_sessions[bin_index] == NULL) {
        clear_bin_bit(gs->bin_bitfield, bin_index);

        // Update group bitfield if entire group is empty
        uint32_t group_start = (bin_index / 32) * 32;
        bool group_empty = true;
        for (uint32_t i = 0; i < 32 && (group_start + i) < gs->num_bins; i++) {
            if (test_bin_bit(gs->bin_bitfield, group_start + i)) {
                group_empty = false;
                break;
            }
        }

        if (group_empty) {
            uint32_t group_bitfield_index = bin_index / 32;
            uint32_t group_word = group_bitfield_index / 32;
            uint32_t group_bit = group_bitfield_index % 32;

            if (group_word < 32) {
                gs->group_bitfield[group_word] &= ~(1U << group_bit);
            }
        }
    }

    // Recalculate cached minimums if we removed the min session
    // This is O(n) but only happens when the minimum session is removed
    bool recalc_needed = false;

    if (session->finish_time == gs->min_finish_time) {
        gs->min_finish_time = UINT64_MAX;
        recalc_needed = true;
    }

    if (session->start_time == gs->min_start_time) {
        gs->min_start_time = UINT64_MAX;
        recalc_needed = true;
    }

    if (recalc_needed && gs->active_session_count > 0) {
        // Scan all sessions to find new minimums
        for (uint32_t bin = 0; bin < gs->num_bins; bin++) {
            session_state_t *curr = gs->bin_sessions[bin];
            while (curr != NULL) {
                if (curr->finish_time < gs->min_finish_time) {
                    gs->min_finish_time = curr->finish_time;
                }
                if (curr->start_time < gs->min_start_time) {
                    gs->min_start_time = curr->start_time;
                }
                curr = curr->next;
            }
        }
    }

    return HWFQ_SUCCESS;
}
