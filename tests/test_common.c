// ============================================================================
// Test Helpers Implementation
// ============================================================================

#include "test_common.h"
#include "../src/hwfq_internal.h"

// ============================================================================
// Portable Barrier Implementation
// ============================================================================

int portable_barrier_init(portable_barrier_t *b, int count) {
    b->count = 0;
    b->target = count;
    b->generation = 0;
    if (pthread_mutex_init(&b->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&b->cond, NULL) != 0) {
        pthread_mutex_destroy(&b->mutex);
        return -1;
    }
    return 0;
}

void portable_barrier_destroy(portable_barrier_t *b) {
    pthread_mutex_destroy(&b->mutex);
    pthread_cond_destroy(&b->cond);
}

void portable_barrier_wait(portable_barrier_t *b) {
    pthread_mutex_lock(&b->mutex);
    int gen = b->generation;
    b->count++;
    if (b->count >= b->target) {
        b->count = 0;
        b->generation++;
        pthread_cond_broadcast(&b->cond);
    } else {
        while (gen == b->generation) {
            pthread_cond_wait(&b->cond, &b->mutex);
        }
    }
    pthread_mutex_unlock(&b->mutex);
}

// ============================================================================
// Sleep Helper
// ============================================================================

void msleep(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// ============================================================================
// Test Helper Functions
// ============================================================================

group_scheduler_t *test_create_group_scheduler(
    hwfq_scheduler_t *parent,
    uint32_t num_groups,
    uint32_t bins_per_group,
    uint64_t total_capacity,
    uint32_t max_entries)
{
    hwfq_chunked_entries_t *entries = (hwfq_chunked_entries_t *)hwfq_alloc(
        parent, sizeof(hwfq_chunked_entries_t));
    if (entries == NULL) {
        return NULL;
    }

    hwfq_chunked_entries_init(entries, max_entries, NULL, NULL);

    group_scheduler_t *gs = group_scheduler_init_with_entries(
        parent, num_groups, bins_per_group, total_capacity, max_entries, entries);
    if (gs == NULL) {
        hwfq_chunked_entries_destroy(entries);
        hwfq_free(parent, entries);
        return NULL;
    }

    return gs;
}

void test_drain_all_sessions(hwfq_scheduler_t *parent, group_scheduler_t *gs)
{
    (void)parent;
    (void)gs;
    // Sessions are now automatically freed by group_scheduler_destroy
}

void test_destroy_group_scheduler(hwfq_scheduler_t *parent, group_scheduler_t *gs)
{
    if (gs == NULL) {
        return;
    }

    // Save entries pointer before destroying gs (since gs gets freed)
    hwfq_chunked_entries_t *entries = gs->entries;

    // Destroy the group scheduler (this frees gs and all remaining sessions)
    group_scheduler_destroy(parent, gs);

    // Now destroy the entries that were created by test_create_group_scheduler
    if (entries != NULL) {
        hwfq_chunked_entries_destroy(entries);
        hwfq_free(parent, entries);
    }
}

void test_hwfq_destroy(hwfq_scheduler_t *scheduler)
{
    // Sessions are now automatically freed by group_scheduler_destroy
    hwfq_destroy(scheduler);
}

int test_alloc_entry(hwfq_chunked_entries_t *entries, uint32_t *entry_id_out) {
    return hwfq_chunked_entries_alloc(entries, entry_id_out);
}

hwfq_flow_id_t test_add_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id) {
    hwfq_allocation_t alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = 100
    };
    hwfq_flow_id_t flow_id;
    int ret = hwfq_add_flow(scheduler, tenant_id, &alloc, &flow_id);
    if (ret != HWFQ_SUCCESS) {
        return 0;
    }
    return flow_id;
}

hwfq_flow_id_t test_add_flow_with_weight(hwfq_scheduler_t *scheduler,
                                          hwfq_tenant_id_t tenant_id,
                                          uint32_t weight) {
    hwfq_allocation_t alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT,
        .weight = weight
    };
    hwfq_flow_id_t flow_id;
    int ret = hwfq_add_flow(scheduler, tenant_id, &alloc, &flow_id);
    if (ret != HWFQ_SUCCESS) {
        return 0;
    }
    return flow_id;
}
