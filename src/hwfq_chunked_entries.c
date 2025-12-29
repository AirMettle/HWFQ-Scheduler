#include "hwfq_chunked_entries.h"
#include "hwfq_group_scheduler_internal.h"
#include "hwfq.h"

entry_config_t *hwfq_chunked_get(hwfq_chunked_entries_t *ce, uint32_t entry_id)
{
    if (ce == NULL) {
        return NULL;
    }
    uint32_t chunk = entry_id >> HWFQ_CHUNK_SHIFT;
    uint32_t slot = entry_id & HWFQ_CHUNK_MASK;
    if (chunk >= ce->num_chunks || ce->chunks[chunk] == NULL) {
        return NULL;
    }
    return &ce->chunks[chunk][slot];
}

void hwfq_chunked_entries_init(hwfq_chunked_entries_t *ce, uint32_t max_entries,
                                void *(*alloc_fn)(size_t), void (*free_fn)(void *))
{
    if (ce == NULL) {
        return;
    }

    memset(ce, 0, sizeof(hwfq_chunked_entries_t));
    ce->free_head = HWFQ_ENTRY_NONE;
    ce->max_entries = max_entries;
    ce->alloc_fn = alloc_fn ? alloc_fn : malloc;
    ce->free_fn = free_fn ? free_fn : free;
}

void hwfq_chunked_entries_destroy(hwfq_chunked_entries_t *ce)
{
    if (ce == NULL) {
        return;
    }

    if (ce->chunks != NULL) {
        for (uint32_t i = 0; i < ce->num_chunks; i++) {
            if (ce->chunks[i] != NULL) {
                ce->free_fn(ce->chunks[i]);
            }
        }
        ce->free_fn(ce->chunks);
    }

    memset(ce, 0, sizeof(hwfq_chunked_entries_t));
    ce->free_head = HWFQ_ENTRY_NONE;
}

static int allocate_chunk(hwfq_chunked_entries_t *ce, uint32_t chunk_index)
{
    if (chunk_index >= HWFQ_MAX_CHUNKS) {
        return HWFQ_ERR_NO_MEMORY;
    }

    if (chunk_index >= ce->num_chunks) {
        uint32_t new_num_chunks = chunk_index + 1;
        uint32_t alloc_chunks = 1;
        while (alloc_chunks < new_num_chunks) {
            alloc_chunks *= 2;
        }
        if (alloc_chunks > HWFQ_MAX_CHUNKS) {
            alloc_chunks = HWFQ_MAX_CHUNKS;
        }

        entry_config_t **new_chunks = (entry_config_t **)ce->alloc_fn(
            sizeof(entry_config_t *) * alloc_chunks);
        if (new_chunks == NULL) {
            return HWFQ_ERR_NO_MEMORY;
        }

        if (ce->chunks != NULL) {
            memcpy(new_chunks, ce->chunks, sizeof(entry_config_t *) * ce->num_chunks);
            ce->free_fn(ce->chunks);
        }

        for (uint32_t i = ce->num_chunks; i < alloc_chunks; i++) {
            new_chunks[i] = NULL;
        }

        ce->chunks = new_chunks;
        ce->num_chunks = alloc_chunks;
    }

    if (ce->chunks[chunk_index] == NULL) {
        entry_config_t *chunk = (entry_config_t *)ce->alloc_fn(
            sizeof(entry_config_t) * HWFQ_CHUNK_SIZE);
        if (chunk == NULL) {
            return HWFQ_ERR_NO_MEMORY;
        }

        memset(chunk, 0, sizeof(entry_config_t) * HWFQ_CHUNK_SIZE);
        ce->chunks[chunk_index] = chunk;
    }

    return HWFQ_SUCCESS;
}

int hwfq_chunked_entries_alloc(hwfq_chunked_entries_t *ce, uint32_t *entry_id_out)
{
    if (ce == NULL || entry_id_out == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }

    uint32_t max = (ce->max_entries > 0) ? ce->max_entries : (HWFQ_MAX_CHUNKS * HWFQ_CHUNK_SIZE);
    if (ce->num_configured >= max) {
        return HWFQ_ERR_NO_MEMORY;
    }

    uint32_t entry_id;

    if (ce->free_head != HWFQ_ENTRY_NONE) {
        entry_id = ce->free_head;
        entry_config_t *entry = hwfq_chunked_get(ce, entry_id);
        if (entry != NULL) {
            ce->free_head = entry->entry_id;
            entry->entry_id = entry_id;
            entry->configured = false;
            ce->num_configured++;
            *entry_id_out = entry_id;
            return HWFQ_SUCCESS;
        }
        ce->free_head = HWFQ_ENTRY_NONE;
    }

    uint32_t chunk_index = ce->num_allocated >> HWFQ_CHUNK_SHIFT;
    uint32_t slot = ce->num_allocated & HWFQ_CHUNK_MASK;

    int ret = allocate_chunk(ce, chunk_index);
    if (ret != HWFQ_SUCCESS) {
        return ret;
    }

    entry_id = hwfq_chunked_encode(chunk_index, slot);
    entry_config_t *entry = &ce->chunks[chunk_index][slot];

    memset(entry, 0, sizeof(entry_config_t));
    entry->entry_id = entry_id;
    entry->configured = false;

    ce->num_allocated++;
    ce->num_configured++;
    *entry_id_out = entry_id;

    return HWFQ_SUCCESS;
}

void hwfq_chunked_entries_free(hwfq_chunked_entries_t *ce, uint32_t entry_id)
{
    if (ce == NULL) {
        return;
    }

    entry_config_t *entry = hwfq_chunked_get(ce, entry_id);
    if (entry == NULL) {
        return;
    }

    entry->configured = false;

    entry->entry_id = ce->free_head;
    ce->free_head = entry_id;

    if (ce->num_configured > 0) {
        ce->num_configured--;
    }
}
