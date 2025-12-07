#ifndef HWFQ_MEMORY_POOL_H
#define HWFQ_MEMORY_POOL_H

#include "hwfq.h"
#include "hwfq_flow_trie.h"
#include "hwfq_group_scheduler_internal.h"  // Need full session_state_t definition
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================================
// Pre-allocated Memory Pool for Session/Flow States
// ============================================================================
//
// This module provides a pre-allocated memory pool for session states,
// avoiding dynamic allocation during scheduling operations. The pool uses
// a hierarchical Trie for O(1) allocation/deallocation tracking.
//
// Benefits:
// - No memory fragmentation during operation
// - Predictable memory usage (fixed at init time)
// - Cache-friendly layout (contiguous array, lower IDs preferred)
// - O(1) allocation and deallocation
//
// Memory layout:
// - Single contiguous array of session_state_t
// - Trie tracks which slots are in use
// - Free list using session prev/next pointers for fast reuse
//
// ============================================================================

// Forward declaration
typedef struct hwfq_memory_pool_t hwfq_memory_pool_t;

// Memory pool structure
struct hwfq_memory_pool_t {
    // Pre-allocated contiguous array of session states
    session_state_t *sessions;        // Array of max_sessions entries
    uint32_t max_sessions;            // Total capacity
    uint32_t active_count;            // Currently allocated count

    // Hierarchical Trie for O(1) allocation tracking
    hwfq_flow_trie_t allocation_trie;

    // Memory allocator functions (from config)
    void *(*alloc_fn)(size_t);
    void (*free_fn)(void *);
};

// ============================================================================
// Pool Lifecycle Functions
// ============================================================================

// Initialize a memory pool with the specified capacity
//
// pool - Pointer to pool structure to initialize
// max_sessions - Maximum number of sessions to support
// alloc_fn - Memory allocator function (NULL = use malloc)
// free_fn - Memory deallocator function (NULL = use free)
// returns - 0 on success, negative error code on failure
//
// Memory usage: approximately max_sessions * 88 bytes for sessions
//               plus Trie overhead (~130 KB per 1M sessions)
int hwfq_memory_pool_init(hwfq_memory_pool_t *pool, uint32_t max_sessions,
                          void *(*alloc_fn)(size_t), void (*free_fn)(void *));

// Destroy a memory pool and free all memory
//
// pool - Pointer to pool structure to destroy
void hwfq_memory_pool_destroy(hwfq_memory_pool_t *pool);

// ============================================================================
// Session Allocation Functions
// ============================================================================

// Allocate a session from the pool
//
// pool - Pointer to initialized pool
// returns - Pointer to allocated session, or NULL if pool is exhausted
//
// The returned session is zero-initialized except for internal bookkeeping.
// Caller must set all required fields before use.
session_state_t *hwfq_memory_pool_alloc_session(hwfq_memory_pool_t *pool);

// Return a session to the pool
//
// pool - Pointer to initialized pool
// session - Session to return (must have been allocated from this pool)
//
// The session's next/prev pointers are cleared.
// Passing NULL is safe (no-op).
void hwfq_memory_pool_free_session(hwfq_memory_pool_t *pool, session_state_t *session);

// ============================================================================
// Query Functions
// ============================================================================

// Get the number of currently allocated sessions
static inline uint32_t hwfq_memory_pool_allocated_count(const hwfq_memory_pool_t *pool) {
    return pool->active_count;
}

// Get the number of free sessions available
static inline uint32_t hwfq_memory_pool_free_count(const hwfq_memory_pool_t *pool) {
    return pool->max_sessions - pool->active_count;
}

// Get the total capacity of the pool
static inline uint32_t hwfq_memory_pool_capacity(const hwfq_memory_pool_t *pool) {
    return pool->max_sessions;
}

// Check if pool has available capacity
static inline bool hwfq_memory_pool_has_capacity(const hwfq_memory_pool_t *pool) {
    return pool->active_count < pool->max_sessions;
}

// Get session by index (for iteration/debugging)
// Returns NULL if index is out of range or session is not allocated
session_state_t *hwfq_memory_pool_get_session(hwfq_memory_pool_t *pool, uint32_t index);

// Convert session pointer to pool index
// Returns UINT32_MAX if session is not from this pool
uint32_t hwfq_memory_pool_session_index(const hwfq_memory_pool_t *pool,
                                        const session_state_t *session);

#endif // HWFQ_MEMORY_POOL_H
