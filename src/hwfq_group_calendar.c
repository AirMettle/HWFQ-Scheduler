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
// DTS-Calendar Queue Operations (Discrete Time Scheduler)
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

    // Update cached minimum finish time, start time, and min_session for O(1) lookups
    if (session->finish_time < gs->min_finish_time) {
        gs->min_finish_time = session->finish_time;
        gs->min_session = session;
    }

    if (session->start_time < gs->min_start_time) {
        gs->min_start_time = session->start_time;
    }

    return HWFQ_SUCCESS;
}

// Helper: Find eligible session with minimum finish time using bitfield-guided search
// Returns NULL if no eligible sessions found, also updates min_start_time
static session_state_t *find_eligible_min_bitfield(group_scheduler_t *gs,
                                                    uint64_t virtual_time,
                                                    uint64_t *out_min_start_time)
{
    session_state_t *eligible_min = NULL;
    uint64_t min_eligible_finish = UINT64_MAX;
    uint64_t min_start_time = UINT64_MAX;

    // Use hierarchical bitfield search: first find active group words,
    // then search bins within those groups
    for (uint32_t gword = 0; gword < 32; gword++) {
        if (gs->group_bitfield[gword] == 0) {
            continue;  // Skip empty group words (32 bin-groups at a time)
        }

        // Find active bin-groups within this word
        uint32_t group_bits = gs->group_bitfield[gword];
        while (group_bits != 0) {
            uint32_t group_bit = (uint32_t)__builtin_ctz(group_bits);
            uint32_t bin_group_idx = gword * 32 + group_bit;
            uint32_t bin_start = bin_group_idx * 32;
            uint32_t bin_end = bin_start + 32;

            if (bin_end > gs->num_bins) {
                bin_end = gs->num_bins;
            }

            // Search bins in this group using bin bitfield
            uint32_t word_idx = bin_start / 32;
            uint32_t bin_bits = gs->bin_bitfield[word_idx];

            while (bin_bits != 0) {
                uint32_t bit_pos = (uint32_t)__builtin_ctz(bin_bits);
                uint32_t bin_idx = word_idx * 32 + bit_pos;

                if (bin_idx >= bin_end) break;

                // Check all sessions in this bin
                session_state_t *curr = gs->bin_sessions[bin_idx];
                while (curr != NULL) {
                    // Track minimum start time
                    if (curr->start_time < min_start_time) {
                        min_start_time = curr->start_time;
                    }

                    // Check eligibility and find minimum finish time
                    if (curr->start_time <= virtual_time) {
                        if (curr->finish_time < min_eligible_finish) {
                            min_eligible_finish = curr->finish_time;
                            eligible_min = curr;
                        }
                    }
                    curr = curr->next;
                }

                // Clear this bit and continue
                bin_bits &= ~(1U << bit_pos);
            }

            // Clear this group bit and continue
            group_bits &= ~(1U << group_bit);
        }
    }

    if (out_min_start_time != NULL) {
        *out_min_start_time = min_start_time;
    }

    return eligible_min;
}

session_state_t *calendar_find_min_session(group_scheduler_t *gs)
{
    if (gs == NULL || gs->active_session_count == 0) {
        return NULL;
    }

    // WF2Q+ algorithm:
    // 1. Find the eligible session (start_time <= virtual_time) with minimum finish_time
    // 2. If no eligible sessions, advance virtual_time to min_start_time, then repeat

    uint64_t virtual_time = gs->virtual_time;
    uint64_t min_start_time = UINT64_MAX;

    // Use bitfield-guided search for eligible session
    session_state_t *eligible_min = find_eligible_min_bitfield(gs, virtual_time, &min_start_time);

    // Update cached min_start_time
    gs->min_start_time = min_start_time;

    // If no eligible sessions found, advance virtual time and try again
    if (eligible_min == NULL && min_start_time != UINT64_MAX) {
        // Advance virtual time to make at least one session eligible
        gs->virtual_time = min_start_time;
        virtual_time = min_start_time;

        // Re-search with updated virtual time
        eligible_min = find_eligible_min_bitfield(gs, virtual_time, NULL);
    }

    // Update cache
    if (eligible_min != NULL) {
        gs->min_finish_time = eligible_min->finish_time;
    }

    return eligible_min;
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

        // Update group bitfield if entire 32-bin group is empty
        // Check the word in bin_bitfield directly (O(1) instead of loop)
        uint32_t word_idx = bin_index / 32;
        if (word_idx < gs->bin_bitfield_size && gs->bin_bitfield[word_idx] == 0) {
            // This 32-bin group is now empty, clear its bit in group_bitfield
            uint32_t group_word = word_idx / 32;
            uint32_t group_bit = word_idx % 32;

            if (group_word < 32) {
                gs->group_bitfield[group_word] &= ~(1U << group_bit);
            }
        }
    }

    // Invalidate cached min_session if we removed it
    if (session == gs->min_session) {
        gs->min_session = NULL;
        gs->min_finish_time = UINT64_MAX;
    }

    // Recalculate min_start_time if we removed the session with the minimum start time
    // This is needed for correct virtual time updates
    // Use bitfield-guided search instead of O(n) full scan
    if (session->start_time == gs->min_start_time) {
        gs->min_start_time = UINT64_MAX;

        // Use hierarchical bitfield search to find new min_start_time
        for (uint32_t gword = 0; gword < 32; gword++) {
            if (gs->group_bitfield[gword] == 0) {
                continue;
            }

            uint32_t group_bits = gs->group_bitfield[gword];
            while (group_bits != 0) {
                uint32_t group_bit = (uint32_t)__builtin_ctz(group_bits);
                uint32_t bin_group_idx = gword * 32 + group_bit;
                uint32_t word_idx = bin_group_idx;  // bin_start / 32

                if (word_idx < gs->bin_bitfield_size) {
                    uint32_t bin_bits = gs->bin_bitfield[word_idx];
                    while (bin_bits != 0) {
                        uint32_t bit_pos = (uint32_t)__builtin_ctz(bin_bits);
                        uint32_t bin_idx = word_idx * 32 + bit_pos;

                        if (bin_idx < gs->num_bins) {
                            session_state_t *curr = gs->bin_sessions[bin_idx];
                            while (curr != NULL) {
                                if (curr->start_time < gs->min_start_time) {
                                    gs->min_start_time = curr->start_time;
                                }
                                curr = curr->next;
                            }
                        }
                        bin_bits &= ~(1U << bit_pos);
                    }
                }
                group_bits &= ~(1U << group_bit);
            }
        }
    }

    return HWFQ_SUCCESS;
}
