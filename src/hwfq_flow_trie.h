#ifndef HWFQ_FLOW_TRIE_H
#define HWFQ_FLOW_TRIE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================================
// Hierarchical Trie for O(1) Flow ID Allocation
// ============================================================================
//
// This data structure provides efficient allocation and deallocation of flow IDs
// from a pre-allocated pool. It uses a hierarchical bitmap (Trie) structure to
// achieve O(1) amortized allocation and O(1) deallocation.
//
// Structure (for 1M flows):
//   Level 2 (Top):   1 × uint64_t    (64 bits, each bit covers 16K flows)
//   Level 1 (Mid):  64 × uint64_t    (4096 bits, each bit covers 256 flows)
//   Level 0 (Base): 4K × uint64_t    (256K bits, each bit covers 4 flows)
//   Leaf:          16K × uint64_t    (1M bits, 1 bit per flow)
//
// Bit semantics:
//   - In leaf level: 0 = free, 1 = allocated
//   - In upper levels: 1 = has at least one free slot below, 0 = fully allocated
//
// Flow ID 0 is reserved and marked as permanently allocated.
//
// ============================================================================

// Maximum supported flows (can be configured lower at runtime)
#define HWFQ_TRIE_MAX_FLOWS (64 * 1024 * 1024)  // 64M maximum

// Trie structure for flow ID allocation tracking
typedef struct {
    // Hierarchical bitfields
    // Upper levels: 1 = has free slots below, 0 = fully allocated
    uint64_t level2;              // Top level (1 word, 64 bits)
    uint64_t *level1;             // Mid level (dynamically sized)
    uint64_t *level0;             // Base level (dynamically sized)
    uint64_t *leaf;               // Leaf level (1 bit per flow, 0=free, 1=allocated)

    // Sizes for each level (computed at init based on max_flows)
    uint32_t level1_size;         // Number of uint64_t words in level1
    uint32_t level0_size;         // Number of uint64_t words in level0
    uint32_t leaf_size;           // Number of uint64_t words in leaf

    // Configuration
    uint32_t max_flows;           // Maximum number of flows supported
    uint32_t allocated_count;     // Current number of allocated flow IDs

    // Allocation hint for cache locality (prefer lower IDs)
    uint32_t alloc_hint;          // Last successful allocation position
} hwfq_flow_trie_t;

// ============================================================================
// Trie Lifecycle Functions
// ============================================================================

// Initialize a flow trie for the given maximum number of flows
//
// trie - Pointer to trie structure to initialize
// max_flows - Maximum number of flows to support (must be > 0)
// alloc_fn - Memory allocator function (NULL = use malloc)
// returns - 0 on success, negative error code on failure
//
// NOTE: Flow ID 0 is automatically reserved and cannot be allocated.
int hwfq_flow_trie_init(hwfq_flow_trie_t *trie, uint32_t max_flows,
                        void *(*alloc_fn)(size_t));

// Destroy a flow trie and free all memory
//
// trie - Pointer to trie structure to destroy
// free_fn - Memory deallocator function (NULL = use free)
void hwfq_flow_trie_destroy(hwfq_flow_trie_t *trie, void (*free_fn)(void *));

// ============================================================================
// Allocation Functions
// ============================================================================

// Allocate a new flow ID
//
// trie - Pointer to initialized trie
// flow_id_out - Output parameter for allocated flow ID
// returns - 0 on success, HWFQ_ERR_NO_MEMORY if no free slots available
//
// Allocation strategy:
// - Prefers lower flow IDs for cache locality
// - Uses alloc_hint to resume from last successful allocation
// - O(1) amortized time complexity using hierarchical bit scanning
int hwfq_flow_trie_alloc(hwfq_flow_trie_t *trie, uint32_t *flow_id_out);

// Free a previously allocated flow ID
//
// trie - Pointer to initialized trie
// flow_id - Flow ID to free (must have been previously allocated)
//
// NOTE: Freeing flow ID 0 is a no-op (it's permanently reserved).
//       Freeing an already-free ID is safe but wasteful.
void hwfq_flow_trie_free(hwfq_flow_trie_t *trie, uint32_t flow_id);

// ============================================================================
// Query Functions
// ============================================================================

// Check if a flow ID is currently allocated
//
// trie - Pointer to initialized trie
// flow_id - Flow ID to check
// returns - true if allocated, false if free or out of range
bool hwfq_flow_trie_is_allocated(const hwfq_flow_trie_t *trie, uint32_t flow_id);

// Get the number of currently allocated flow IDs
//
// trie - Pointer to initialized trie
// returns - Number of allocated flow IDs (includes reserved ID 0)
static inline uint32_t hwfq_flow_trie_allocated_count(const hwfq_flow_trie_t *trie) {
    return trie->allocated_count;
}

// Get the number of free flow IDs available
//
// trie - Pointer to initialized trie
// returns - Number of free flow IDs
static inline uint32_t hwfq_flow_trie_free_count(const hwfq_flow_trie_t *trie) {
    return trie->max_flows - trie->allocated_count;
}

// Get the maximum number of flows supported
//
// trie - Pointer to initialized trie
// returns - Maximum number of flows
static inline uint32_t hwfq_flow_trie_capacity(const hwfq_flow_trie_t *trie) {
    return trie->max_flows;
}

#endif // HWFQ_FLOW_TRIE_H
