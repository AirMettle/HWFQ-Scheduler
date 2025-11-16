#include "hwfq_internal.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Memory Management Implementation
// ============================================================================

void *hwfq_alloc(hwfq_scheduler_t *scheduler, size_t size)
{
    if (scheduler == NULL || size == 0) {
        return NULL;
    }
    if (scheduler->alloc_fn != NULL) {
        return scheduler->alloc_fn(size);
    }
    return malloc(size);
}

void hwfq_free(hwfq_scheduler_t *scheduler, void *ptr)
{
    if (scheduler == NULL || ptr == NULL) {
        return;
    }
    if (scheduler->free_fn != NULL) {
        scheduler->free_fn(ptr);
    } else {
        free(ptr);
    }
}
