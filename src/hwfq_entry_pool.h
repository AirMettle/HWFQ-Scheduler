#ifndef HWFQ_ENTRY_POOL_H
#define HWFQ_ENTRY_POOL_H

#include "hwfq_flow_trie.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// ============================================================================
// Shared Entry Pool for Flow Configurations
// ============================================================================
//
// This module provides a single pre-allocated pool for all entry_config_t
// structures across all tenants. This approach:
//
// - Eliminates per-tenant entry array overhead (major memory savings)
// - Prevents memory fragmentation (single contiguous allocation)
// - Uses hierarchical trie for O(1) allocation/deallocation
// - Is thread-safe with its own mutex
//
// Per reviewer feedback: "allocate all flow state from a single array...
// pre-allocate things and keep this from getting fragmented"
//
// ============================================================================

// Forward declaration (full definition in hwfq_group_scheduler_internal.h)
typedef struct entry_config_t entry_config_t;

// Entry pool structure
typedef struct hwfq_entry_pool_t {
    // Pre-allocated contiguous array of entries
    entry_config_t *entries;
    uint32_t max_entries;       // Total capacity (from config.max_total_flows)
    uint32_t allocated_count;   // Currently allocated entries

    // Hierarchical trie for O(1) allocation tracking
    hwfq_flow_trie_t alloc_trie;

    // Thread-safe access
    pthread_mutex_t lock;

    // Memory allocator functions
    void *(*alloc_fn)(size_t);
    void (*free_fn)(void *);
} hwfq_entry_pool_t;

// ============================================================================
// Pool Lifecycle Functions
// ============================================================================

// Initialize entry pool with specified capacity
//
// pool       - Pointer to pool structure to initialize
// max_entries - Maximum number of entries to support
// alloc_fn   - Memory allocator (NULL = malloc)
// free_fn    - Memory deallocator (NULL = free)
// returns    - 0 on success, negative error code on failure
int hwfq_entry_pool_init(hwfq_entry_pool_t *pool, uint32_t max_entries,
                         void *(*alloc_fn)(size_t), void (*free_fn)(void *));

// Destroy entry pool and free all memory
//
// pool - Pointer to pool to destroy
void hwfq_entry_pool_destroy(hwfq_entry_pool_t *pool);

// ============================================================================
// Allocation Functions (Thread-Safe)
// ============================================================================

// Allocate an entry from the pool
//
// pool - Pointer to initialized pool
// returns - Pointer to allocated entry, or NULL if pool exhausted
//
// The returned entry is zero-initialized.
// Thread-safe: acquires pool lock internally.
entry_config_t *hwfq_entry_pool_alloc(hwfq_entry_pool_t *pool);

// Free an entry back to the pool
//
// pool  - Pointer to initialized pool
// entry - Entry to free (must be from this pool)
//
// Thread-safe: acquires pool lock internally.
// Passing NULL is safe (no-op).
void hwfq_entry_pool_free(hwfq_entry_pool_t *pool, entry_config_t *entry);

// ============================================================================
// Query Functions
// ============================================================================

// Get number of allocated entries
static inline uint32_t hwfq_entry_pool_allocated_count(const hwfq_entry_pool_t *pool) {
    return pool->allocated_count;
}

// Get number of free entries
static inline uint32_t hwfq_entry_pool_free_count(const hwfq_entry_pool_t *pool) {
    return pool->max_entries - pool->allocated_count;
}

// Get pool capacity
static inline uint32_t hwfq_entry_pool_capacity(const hwfq_entry_pool_t *pool) {
    return pool->max_entries;
}

// Check if pool has available capacity
static inline bool hwfq_entry_pool_has_capacity(const hwfq_entry_pool_t *pool) {
    return pool->allocated_count < pool->max_entries;
}

#endif // HWFQ_ENTRY_POOL_H
