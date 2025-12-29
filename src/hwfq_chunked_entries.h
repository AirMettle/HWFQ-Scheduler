#ifndef HWFQ_CHUNKED_ENTRIES_H
#define HWFQ_CHUNKED_ENTRIES_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Chunked Entry Storage
// ============================================================================
//
// Per-owner chunked arrays for entry storage. Each owner (tenant or system)
// has its own chunked_entries_t that grows on demand.
//
// Entry ID encoding: (chunk_index << CHUNK_SHIFT) | slot_in_chunk
// - Supports up to MAX_CHUNKS * CHUNK_SIZE entries per owner
// - Default: 1M chunks * 1K slots = 1B entries max
//
// ============================================================================

#define HWFQ_CHUNK_SHIFT 10
#define HWFQ_CHUNK_SIZE (1U << HWFQ_CHUNK_SHIFT)      // 1,024 entries per chunk
#define HWFQ_CHUNK_MASK (HWFQ_CHUNK_SIZE - 1)
#define HWFQ_MAX_CHUNKS (1U << 20)

#define HWFQ_ENTRY_NONE UINT32_MAX

typedef struct entry_config_t entry_config_t;

typedef struct hwfq_chunked_entries_t {
    entry_config_t **chunks;
    uint32_t num_chunks;
    uint32_t num_allocated;
    uint32_t num_configured;
    uint32_t free_head;
    uint32_t max_entries;
    void *(*alloc_fn)(size_t);
    void (*free_fn)(void *);
} hwfq_chunked_entries_t;

// Decode entry_id to chunk index and slot
static inline void hwfq_chunked_decode(uint32_t entry_id, uint32_t *chunk, uint32_t *slot) {
    *chunk = entry_id >> HWFQ_CHUNK_SHIFT;
    *slot = entry_id & HWFQ_CHUNK_MASK;
}

// Encode chunk index and slot to entry_id
static inline uint32_t hwfq_chunked_encode(uint32_t chunk, uint32_t slot) {
    return (chunk << HWFQ_CHUNK_SHIFT) | slot;
}

// Get entry by ID (direct indexing) - returns NULL if not allocated
// Defined in hwfq_chunked_entries.c to avoid include order issues
entry_config_t *hwfq_chunked_get(hwfq_chunked_entries_t *ce, uint32_t entry_id);

// Initialize chunked entries (no allocation yet)
//
// ce         - Chunked entries to initialize
// max_entries - Maximum entries allowed (0 = default HWFQ_MAX_CHUNKS * HWFQ_CHUNK_SIZE)
// alloc_fn   - Memory allocator (NULL = malloc)
// free_fn    - Memory deallocator (NULL = free)
void hwfq_chunked_entries_init(hwfq_chunked_entries_t *ce, uint32_t max_entries,
                                void *(*alloc_fn)(size_t), void (*free_fn)(void *));

// Destroy chunked entries and free all chunks
//
// ce - Chunked entries to destroy
void hwfq_chunked_entries_destroy(hwfq_chunked_entries_t *ce);

// Allocate next free entry, returns entry_id
//
// ce           - Chunked entries
// entry_id_out - Output: assigned entry_id
//
// Returns 0 on success, negative error code on failure
// Allocates new chunk if needed
int hwfq_chunked_entries_alloc(hwfq_chunked_entries_t *ce, uint32_t *entry_id_out);

// Free an entry (add to free list for reuse)
//
// ce       - Chunked entries
// entry_id - Entry to free
void hwfq_chunked_entries_free(hwfq_chunked_entries_t *ce, uint32_t entry_id);

// Get number of configured entries
static inline uint32_t hwfq_chunked_entries_count(const hwfq_chunked_entries_t *ce) {
    return ce->num_configured;
}

// Check if there's capacity for more entries
static inline bool hwfq_chunked_entries_has_capacity(const hwfq_chunked_entries_t *ce) {
    // Can allocate if: free list not empty, OR can allocate more slots, OR can add chunks
    if (ce->free_head != HWFQ_ENTRY_NONE) {
        return true;
    }
    uint32_t max = (ce->max_entries > 0) ? ce->max_entries : (HWFQ_MAX_CHUNKS * HWFQ_CHUNK_SIZE);
    return ce->num_allocated < max;
}

#endif // HWFQ_CHUNKED_ENTRIES_H
