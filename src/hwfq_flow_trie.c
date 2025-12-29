#include "hwfq_flow_trie.h"
#include "hwfq.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Bit Manipulation Helpers
// ============================================================================

// Find first set bit (1-indexed, returns 0 if no bit set)
// Uses compiler builtin for efficiency
static inline uint32_t find_first_set_bit64(uint64_t value) {
    if (value == 0) {
        return 0;
    }
    return (uint32_t)__builtin_ffsll((long long)value);
}

// Find first zero bit (1-indexed, returns 0 if all bits set)
static inline uint32_t find_first_zero_bit64(uint64_t value) {
    return find_first_set_bit64(~value);
}

// Set a bit in a uint64_t array
static inline void set_bit(uint64_t *array, uint32_t bit_index) {
    uint32_t word_index = bit_index / 64;
    uint32_t bit_offset = bit_index % 64;
    array[word_index] |= (1ULL << bit_offset);
}

// Clear a bit in a uint64_t array
static inline void clear_bit(uint64_t *array, uint32_t bit_index) {
    uint32_t word_index = bit_index / 64;
    uint32_t bit_offset = bit_index % 64;
    array[word_index] &= ~(1ULL << bit_offset);
}

// Test a bit in a uint64_t array
static inline bool test_bit(const uint64_t *array, uint32_t bit_index) {
    uint32_t word_index = bit_index / 64;
    uint32_t bit_offset = bit_index % 64;
    return (array[word_index] & (1ULL << bit_offset)) != 0;
}

// ============================================================================
// Size Calculation Helpers
// ============================================================================

// Calculate the number of uint64_t words needed for a given number of bits
static inline uint32_t bits_to_words(uint32_t num_bits) {
    return (num_bits + 63) / 64;
}

// Calculate level sizes for a given max_flows
// Each upper level bit covers 64 items in the level below
static void calculate_level_sizes(uint32_t max_flows,
                                  uint32_t *leaf_size,
                                  uint32_t *level0_size,
                                  uint32_t *level1_size) {
    // Leaf: 1 bit per flow
    *leaf_size = bits_to_words(max_flows);

    // Level 0: 1 bit per leaf word (each bit covers 64 flows)
    *level0_size = bits_to_words(*leaf_size);

    // Level 1: 1 bit per level0 word (each bit covers 64*64 = 4096 flows)
    *level1_size = bits_to_words(*level0_size);

    // Level 2 is always 1 word (64 bits, each covers 64*64*64 = 262144 flows)
    // Maximum: 64 * 262144 = 16M flows per level2 word
    // For 64M flows, level2 needs only 1 word (with 4 bits used)
}

int hwfq_flow_trie_init(hwfq_flow_trie_t *trie, uint32_t max_flows,
                        void *(*alloc_fn)(size_t), void (*free_fn)(void *)) {
    if (trie == NULL || max_flows == 0) {
        return HWFQ_ERR_INVALID_ARG;
    }

    if (alloc_fn == NULL) {
        alloc_fn = malloc;
    }
    if (free_fn == NULL) {
        free_fn = free;
    }

    calculate_level_sizes(max_flows,
                          &trie->leaf_size,
                          &trie->level0_size,
                          &trie->level1_size);

    trie->max_flows = max_flows;
    trie->allocated_count = 0;
    trie->alloc_hint = 0;

    size_t leaf_bytes = (size_t)trie->leaf_size * sizeof(uint64_t);
    trie->leaf = (uint64_t *)alloc_fn(leaf_bytes);
    if (trie->leaf == NULL) {
        return HWFQ_ERR_NO_MEMORY;
    }
    memset(trie->leaf, 0, leaf_bytes);  // All free initially

    // Allocate level 0
    size_t level0_bytes = (size_t)trie->level0_size * sizeof(uint64_t);
    trie->level0 = (uint64_t *)alloc_fn(level0_bytes);
    if (trie->level0 == NULL) {
        free_fn(trie->leaf);
        trie->leaf = NULL;
        return HWFQ_ERR_NO_MEMORY;
    }
    // All ones = all leaf words have free slots
    memset(trie->level0, 0xFF, level0_bytes);

    // Allocate level 1
    size_t level1_bytes = (size_t)trie->level1_size * sizeof(uint64_t);
    trie->level1 = (uint64_t *)alloc_fn(level1_bytes);
    if (trie->level1 == NULL) {
        free_fn(trie->level0);
        free_fn(trie->leaf);
        trie->level0 = NULL;
        trie->leaf = NULL;
        return HWFQ_ERR_NO_MEMORY;
    }
    // All ones = all level0 words have free slots
    memset(trie->level1, 0xFF, level1_bytes);

    // Initialize level 2 (all ones = all level1 words have free slots)
    trie->level2 = UINT64_MAX;

    // Clear bits for unused entries at the end of each level
    // to prevent allocating beyond max_flows

    // Mask out unused bits in the last leaf word
    uint32_t used_bits_in_last_leaf = max_flows % 64;
    if (used_bits_in_last_leaf != 0) {
        uint64_t mask = (1ULL << used_bits_in_last_leaf) - 1;
        trie->leaf[trie->leaf_size - 1] |= ~mask;  // Mark unused as "allocated"
    }

    // Mask out unused bits in the last level0 word
    uint32_t used_bits_in_last_level0 = trie->leaf_size % 64;
    if (used_bits_in_last_level0 != 0) {
        uint64_t mask = (1ULL << used_bits_in_last_level0) - 1;
        trie->level0[trie->level0_size - 1] &= mask;  // Only valid entries have free slots
    }

    // Mask out unused bits in the last level1 word
    uint32_t used_bits_in_last_level1 = trie->level0_size % 64;
    if (used_bits_in_last_level1 != 0) {
        uint64_t mask = (1ULL << used_bits_in_last_level1) - 1;
        trie->level1[trie->level1_size - 1] &= mask;
    }

    // Mask out unused bits in level2
    uint32_t used_bits_in_level2 = trie->level1_size;
    if (used_bits_in_level2 < 64) {
        uint64_t mask = (1ULL << used_bits_in_level2) - 1;
        trie->level2 &= mask;
    }

    // Reserve flow ID 0 (mark as allocated)
    trie->leaf[0] |= 1ULL;  // Bit 0 = flow ID 0
    trie->allocated_count = 1;

    // Update upper levels if leaf word 0 is now full
    if (trie->leaf[0] == UINT64_MAX) {
        clear_bit(trie->level0, 0);
        if (trie->level0[0] == 0) {
            clear_bit(trie->level1, 0);
            if (trie->level1[0] == 0) {
                trie->level2 &= ~1ULL;
            }
        }
    }

    return HWFQ_SUCCESS;
}

void hwfq_flow_trie_destroy(hwfq_flow_trie_t *trie, void (*free_fn)(void *)) {
    if (trie == NULL) {
        return;
    }

    if (free_fn == NULL) {
        free_fn = free;
    }

    if (trie->leaf != NULL) {
        free_fn(trie->leaf);
        trie->leaf = NULL;
    }

    if (trie->level0 != NULL) {
        free_fn(trie->level0);
        trie->level0 = NULL;
    }

    if (trie->level1 != NULL) {
        free_fn(trie->level1);
        trie->level1 = NULL;
    }

    trie->level2 = 0;
    trie->max_flows = 0;
    trie->allocated_count = 0;
}

int hwfq_flow_trie_alloc(hwfq_flow_trie_t *trie, uint32_t *flow_id_out) {
    if (trie == NULL || flow_id_out == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }

    if (trie->level2 == 0) {
        return HWFQ_ERR_NO_MEMORY;  // All slots allocated
    }

    // Find first free slot using hierarchical search
    // Level 2: find first set bit (indicates level1 word with free slots)
    uint32_t l2_bit = find_first_set_bit64(trie->level2);
    if (l2_bit == 0) {
        return HWFQ_ERR_NO_MEMORY;
    }
    uint32_t l1_word_idx = l2_bit - 1;

    // Level 1: find first set bit in the selected word
    uint32_t l1_bit = find_first_set_bit64(trie->level1[l1_word_idx]);
    if (l1_bit == 0) {
        return HWFQ_ERR_INTERNAL;  // Inconsistent state
    }
    uint32_t l0_word_idx = l1_word_idx * 64 + (l1_bit - 1);

    // Level 0: find first set bit in the selected word
    uint32_t l0_bit = find_first_set_bit64(trie->level0[l0_word_idx]);
    if (l0_bit == 0) {
        return HWFQ_ERR_INTERNAL;  // Inconsistent state
    }
    uint32_t leaf_word_idx = l0_word_idx * 64 + (l0_bit - 1);

    // Leaf: find first zero bit (free slot)
    uint32_t leaf_bit = find_first_zero_bit64(trie->leaf[leaf_word_idx]);
    if (leaf_bit == 0) {
        return HWFQ_ERR_INTERNAL;  // Inconsistent state
    }
    uint32_t flow_id = leaf_word_idx * 64 + (leaf_bit - 1);

    if (flow_id >= trie->max_flows) {
        return HWFQ_ERR_NO_MEMORY;
    }

    set_bit(trie->leaf, flow_id);
    trie->allocated_count++;
    *flow_id_out = flow_id;

    // Update upper levels if leaf word is now full
    if (trie->leaf[leaf_word_idx] == UINT64_MAX) {
        clear_bit(trie->level0, leaf_word_idx);

        // Check if level0 word is now empty (all full)
        if (trie->level0[l0_word_idx] == 0) {
            clear_bit(trie->level1, l0_word_idx);

            // Check if level1 word is now empty
            if (trie->level1[l1_word_idx] == 0) {
                trie->level2 &= ~(1ULL << l1_word_idx);
            }
        }
    }

    return HWFQ_SUCCESS;
}

void hwfq_flow_trie_free(hwfq_flow_trie_t *trie, uint32_t flow_id) {
    if (trie == NULL || flow_id == 0 || flow_id >= trie->max_flows) {
        return;  // Invalid or reserved ID
    }

    // Check if already free
    if (!test_bit(trie->leaf, flow_id)) {
        return;  // Already free
    }

    // Calculate word indices
    uint32_t leaf_word_idx = flow_id / 64;
    uint32_t l0_word_idx = leaf_word_idx / 64;
    uint32_t l1_word_idx = l0_word_idx / 64;

    // Check if leaf word was full before freeing
    bool was_full = (trie->leaf[leaf_word_idx] == UINT64_MAX);

    clear_bit(trie->leaf, flow_id);
    trie->allocated_count--;

    // Update upper levels if this was the first free slot in the leaf word
    if (was_full) {
        set_bit(trie->level0, leaf_word_idx);

        // Check if this was the first free slot in level0 word
        if (trie->level0[l0_word_idx] == (1ULL << (leaf_word_idx % 64))) {
            // This was the first bit set in this word
            set_bit(trie->level1, l0_word_idx);

            // Check if this was the first free slot in level1 word
            if (trie->level1[l1_word_idx] == (1ULL << (l0_word_idx % 64))) {
                trie->level2 |= (1ULL << l1_word_idx);
            }
        }
    }
}

bool hwfq_flow_trie_is_allocated(const hwfq_flow_trie_t *trie, uint32_t flow_id) {
    if (trie == NULL || flow_id >= trie->max_flows) {
        return false;
    }
    return test_bit(trie->leaf, flow_id);
}
