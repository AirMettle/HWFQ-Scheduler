// ============================================================================
// Shared Entry Pool Implementation
// ============================================================================

#include "hwfq_entry_pool.h"
#include "hwfq_group_scheduler_internal.h"
#include "hwfq.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Pool Lifecycle
// ============================================================================

int hwfq_entry_pool_init(hwfq_entry_pool_t *pool, uint32_t max_entries,
                         void *(*alloc_fn)(size_t), void (*free_fn)(void *))
{
    if (pool == NULL || max_entries == 0) {
        return HWFQ_ERR_INVALID_ARG;
    }

    // Use provided allocators or defaults
    pool->alloc_fn = alloc_fn ? alloc_fn : malloc;
    pool->free_fn = free_fn ? free_fn : free;
    pool->max_entries = max_entries;
    pool->allocated_count = 0;

    // Allocate contiguous entry array
    size_t entries_size = sizeof(entry_config_t) * max_entries;
    pool->entries = (entry_config_t *)pool->alloc_fn(entries_size);
    if (pool->entries == NULL) {
        return HWFQ_ERR_NO_MEMORY;
    }
    memset(pool->entries, 0, entries_size);

    // Initialize allocation trie
    int ret = hwfq_flow_trie_init(&pool->alloc_trie, max_entries, pool->alloc_fn);
    if (ret != 0) {
        pool->free_fn(pool->entries);
        pool->entries = NULL;
        return HWFQ_ERR_NO_MEMORY;
    }

    // Initialize mutex
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        hwfq_flow_trie_destroy(&pool->alloc_trie, pool->free_fn);
        pool->free_fn(pool->entries);
        pool->entries = NULL;
        return HWFQ_ERR_INTERNAL;
    }

    return HWFQ_SUCCESS;
}

void hwfq_entry_pool_destroy(hwfq_entry_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }

    pthread_mutex_destroy(&pool->lock);
    hwfq_flow_trie_destroy(&pool->alloc_trie, pool->free_fn);

    if (pool->entries != NULL) {
        pool->free_fn(pool->entries);
        pool->entries = NULL;
    }

    pool->max_entries = 0;
    pool->allocated_count = 0;
}

// ============================================================================
// Allocation Functions
// ============================================================================

entry_config_t *hwfq_entry_pool_alloc(hwfq_entry_pool_t *pool)
{
    if (pool == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&pool->lock);

    // Allocate index from trie
    uint32_t index;
    int ret = hwfq_flow_trie_alloc(&pool->alloc_trie, &index);
    if (ret != 0) {
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    pool->allocated_count++;
    entry_config_t *entry = &pool->entries[index];

    // Zero-initialize the entry
    memset(entry, 0, sizeof(entry_config_t));

    pthread_mutex_unlock(&pool->lock);

    return entry;
}

void hwfq_entry_pool_free(hwfq_entry_pool_t *pool, entry_config_t *entry)
{
    if (pool == NULL || entry == NULL) {
        return;
    }

    // Calculate index from pointer
    ptrdiff_t diff = entry - pool->entries;
    if (diff < 0 || (uint32_t)diff >= pool->max_entries) {
        // Entry not from this pool
        return;
    }
    uint32_t index = (uint32_t)diff;

    pthread_mutex_lock(&pool->lock);

    // Free index back to trie
    hwfq_flow_trie_free(&pool->alloc_trie, index);

    if (pool->allocated_count > 0) {
        pool->allocated_count--;
    }

    pthread_mutex_unlock(&pool->lock);
}
