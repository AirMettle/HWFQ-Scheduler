#include "hwfq_memory_pool.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Pool Lifecycle Functions
// ============================================================================

int hwfq_memory_pool_init(hwfq_memory_pool_t *pool, uint32_t max_sessions,
                          void *(*alloc_fn)(size_t), void (*free_fn)(void *)) {
    if (pool == NULL || max_sessions == 0) {
        return HWFQ_ERR_INVALID_ARG;
    }

    // Use default allocators if not provided
    if (alloc_fn == NULL) {
        alloc_fn = malloc;
    }
    if (free_fn == NULL) {
        free_fn = free;
    }

    pool->alloc_fn = alloc_fn;
    pool->free_fn = free_fn;
    pool->max_sessions = max_sessions;
    pool->active_count = 0;

    // Allocate the session array
    size_t sessions_bytes = (size_t)max_sessions * sizeof(session_state_t);
    pool->sessions = (session_state_t *)alloc_fn(sessions_bytes);
    if (pool->sessions == NULL) {
        return HWFQ_ERR_NO_MEMORY;
    }

    // Zero-initialize all sessions
    memset(pool->sessions, 0, sessions_bytes);

    // Initialize the allocation trie
    int ret = hwfq_flow_trie_init(&pool->allocation_trie, max_sessions, alloc_fn);
    if (ret != HWFQ_SUCCESS) {
        free_fn(pool->sessions);
        pool->sessions = NULL;
        return ret;
    }

    return HWFQ_SUCCESS;
}

void hwfq_memory_pool_destroy(hwfq_memory_pool_t *pool) {
    if (pool == NULL) {
        return;
    }

    void (*free_fn)(void *) = pool->free_fn;
    if (free_fn == NULL) {
        free_fn = free;
    }

    // Destroy the trie first
    hwfq_flow_trie_destroy(&pool->allocation_trie, free_fn);

    // Free the session array
    if (pool->sessions != NULL) {
        free_fn(pool->sessions);
        pool->sessions = NULL;
    }

    pool->max_sessions = 0;
    pool->active_count = 0;
}

// ============================================================================
// Session Allocation Functions
// ============================================================================

session_state_t *hwfq_memory_pool_alloc_session(hwfq_memory_pool_t *pool) {
    if (pool == NULL || pool->sessions == NULL) {
        return NULL;
    }

    // Allocate a slot from the trie
    uint32_t index;
    int ret = hwfq_flow_trie_alloc(&pool->allocation_trie, &index);
    if (ret != HWFQ_SUCCESS) {
        return NULL;  // Pool exhausted
    }

    // Validate index (should always be valid if trie is consistent)
    if (index >= pool->max_sessions) {
        // Inconsistent state - free the slot and return error
        hwfq_flow_trie_free(&pool->allocation_trie, index);
        return NULL;
    }

    // Get the session pointer
    session_state_t *session = &pool->sessions[index];

    // Zero-initialize the session
    memset(session, 0, sizeof(session_state_t));

    pool->active_count++;

    return session;
}

void hwfq_memory_pool_free_session(hwfq_memory_pool_t *pool, session_state_t *session) {
    if (pool == NULL || session == NULL || pool->sessions == NULL) {
        return;
    }

    // Calculate the index from the pointer
    ptrdiff_t offset = session - pool->sessions;
    if (offset < 0 || (size_t)offset >= pool->max_sessions) {
        return;  // Not from this pool
    }

    uint32_t index = (uint32_t)offset;

    // Verify it's actually allocated
    if (!hwfq_flow_trie_is_allocated(&pool->allocation_trie, index)) {
        return;  // Already free
    }

    // Clear the session's linked list pointers
    session->next = NULL;
    session->prev = NULL;

    // Free the slot in the trie
    hwfq_flow_trie_free(&pool->allocation_trie, index);

    pool->active_count--;
}

// ============================================================================
// Query Functions
// ============================================================================

session_state_t *hwfq_memory_pool_get_session(hwfq_memory_pool_t *pool, uint32_t index) {
    if (pool == NULL || pool->sessions == NULL || index >= pool->max_sessions) {
        return NULL;
    }

    // Check if the slot is allocated
    if (!hwfq_flow_trie_is_allocated(&pool->allocation_trie, index)) {
        return NULL;
    }

    return &pool->sessions[index];
}

uint32_t hwfq_memory_pool_session_index(const hwfq_memory_pool_t *pool,
                                        const session_state_t *session) {
    if (pool == NULL || session == NULL || pool->sessions == NULL) {
        return UINT32_MAX;
    }

    ptrdiff_t offset = session - pool->sessions;
    if (offset < 0 || (size_t)offset >= pool->max_sessions) {
        return UINT32_MAX;
    }

    return (uint32_t)offset;
}
