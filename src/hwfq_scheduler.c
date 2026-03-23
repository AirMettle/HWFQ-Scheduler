// ============================================================================
// H-WFQ Hierarchical Scheduler Implementation
// ============================================================================
//
// This file implements the hierarchical scheduling API that combines the
// system-level scheduler (for tenants) with tenant-level schedulers (for flows).
//
// ============================================================================

#include "hwfq_internal.h"
#include "hwfq_group_scheduler.h"
#include "hwfq_group_scheduler_internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Default work quantum for tenant sessions in system scheduler
#define TENANT_QUANTUM_SIZE 4096

// Cleanup callback for flow_scheduler sessions
// Handles freeing flow_session_context_t and calling user's cleanup_fn
static void flow_context_cleanup(hwfq_scheduler_t *parent, void *user_data)
{
    flow_session_context_t *context = (flow_session_context_t *)user_data;
    if (context == NULL) {
        return;
    }
    if (context->cleanup_fn != NULL) {
        context->cleanup_fn(context->user_data);
    }
    hwfq_free(parent, context);
}

// Default weight for unconfigured flows
#define DEFAULT_FLOW_WEIGHT 100

// Rate calculation window (1 second in nanoseconds)
#define RATE_WINDOW_NS (1000000000ULL)

// Hash function for user_data pointer (Knuth multiplicative hash)
static inline uint32_t hash_user_data(void *user_data) {
    uintptr_t key = (uintptr_t)user_data;
    key = key * 2654435761UL;
    return (uint32_t)(key & HWFQ_IN_FLIGHT_HASH_MASK);
}

// Insert entry into in-flight hash table and list
static void in_flight_insert(hwfq_scheduler_t *scheduler, in_flight_entry_t *entry) {
    uint32_t bucket = hash_user_data(entry->user_data);
    entry->hash_next = scheduler->in_flight_hash[bucket];
    scheduler->in_flight_hash[bucket] = entry;

    entry->list_next = NULL;
    entry->list_prev = scheduler->in_flight_tail;
    if (scheduler->in_flight_tail != NULL) {
        scheduler->in_flight_tail->list_next = entry;
    } else {
        scheduler->in_flight_head = entry;
    }
    scheduler->in_flight_tail = entry;
}

// Remove entry from in-flight hash table and list
static void in_flight_remove(hwfq_scheduler_t *scheduler, in_flight_entry_t *entry) {
    uint32_t bucket = hash_user_data(entry->user_data);
    in_flight_entry_t **prev = &scheduler->in_flight_hash[bucket];
    while (*prev != NULL) {
        if (*prev == entry) {
            *prev = entry->hash_next;
            break;
        }
        prev = &(*prev)->hash_next;
    }

    if (entry->list_prev != NULL) {
        entry->list_prev->list_next = entry->list_next;
    } else {
        scheduler->in_flight_head = entry->list_next;
    }
    if (entry->list_next != NULL) {
        entry->list_next->list_prev = entry->list_prev;
    } else {
        scheduler->in_flight_tail = entry->list_prev;
    }
}

// Find entry in hash table by user_data, tenant_id, flow_id
static in_flight_entry_t *in_flight_find(hwfq_scheduler_t *scheduler,
                                          void *user_data,
                                          hwfq_tenant_id_t tenant_id,
                                          hwfq_flow_id_t flow_id) {
    uint32_t bucket = hash_user_data(user_data);
    in_flight_entry_t *entry = scheduler->in_flight_hash[bucket];

    while (entry != NULL) {
        if (entry->user_data == user_data &&
            entry->tenant_id == tenant_id &&
            entry->flow_id == flow_id) {
            return entry;
        }
        entry = entry->hash_next;
    }
    return NULL;
}

// Get current time in nanoseconds (monotonic clock)
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Update rate tracking for a tenant
static void update_tenant_rate_tracking(tenant_config_t *tenant, uint64_t work_size, uint64_t now_ns) {
    rate_tracking_t *rt = &tenant->rate_tracking;

    // If window hasn't started yet, initialize it
    if (rt->window_start_ns == 0) {
        rt->window_start_ns = now_ns;
        rt->window_work_units = work_size;
        return;
    }

    // Check if we've exceeded the window
    uint64_t elapsed = now_ns - rt->window_start_ns;
    if (elapsed >= RATE_WINDOW_NS) {
        // Calculate rate for the completed window
        if (elapsed > 0) {
            tenant->stats.effective_rate = (double)rt->window_work_units * 1000000000.0 / (double)elapsed;
        }
        // Start new window
        rt->window_start_ns = now_ns;
        rt->window_work_units = work_size;
    } else {
        // Add to current window
        rt->window_work_units += work_size;
        // Update rate estimate (running calculation)
        if (elapsed > 0) {
            tenant->stats.effective_rate = (double)rt->window_work_units * 1000000000.0 / (double)elapsed;
        }
    }
}

// Update rate tracking for an entry (flow)
static void update_entry_rate_tracking(entry_config_t *entry, uint64_t work_size, uint64_t now_ns) {
    entry_rate_tracking_t *rt = &entry->rate_tracking;

    // If window hasn't started yet, initialize it
    if (rt->window_start_ns == 0) {
        rt->window_start_ns = now_ns;
        rt->window_work_units = work_size;
        return;
    }

    // Check if we've exceeded the window
    uint64_t elapsed = now_ns - rt->window_start_ns;
    if (elapsed >= RATE_WINDOW_NS) {
        // Calculate rate for the completed window
        if (elapsed > 0) {
            entry->stats.effective_rate = (double)rt->window_work_units * 1000000000.0 / (double)elapsed;
        }
        // Start new window
        rt->window_start_ns = now_ns;
        rt->window_work_units = work_size;
    } else {
        // Add to current window
        rt->window_work_units += work_size;
        // Update rate estimate (running calculation)
        if (elapsed > 0) {
            entry->stats.effective_rate = (double)rt->window_work_units * 1000000000.0 / (double)elapsed;
        }
    }
}

// Find flow entry in tenant's chunked entries (direct O(1) lookup)
static entry_config_t *find_flow_entry(tenant_config_t *tenant, hwfq_flow_id_t flow_id) {
    entry_config_t *entry = hwfq_chunked_get(&tenant->flow_entries, flow_id);
    if (entry != NULL && entry->configured) {
        return entry;
    }
    return NULL;
}

// Ensure a flow entry exists in tenant's scheduler
// Flow must have been created via hwfq_add_flow()
static int ensure_flow_entry(hwfq_scheduler_t *scheduler,
                             tenant_config_t *tenant,
                             hwfq_flow_id_t flow_id)
{
    (void)scheduler;  // unused

    entry_config_t *entry = hwfq_chunked_get(&tenant->flow_entries, flow_id);
    if (entry != NULL && entry->configured) {
        return HWFQ_SUCCESS;
    }
    return HWFQ_ERR_NOT_FOUND;
}

// Register tenant as having backlog in system scheduler
static int register_tenant_backlog(hwfq_scheduler_t *scheduler,
                                   tenant_config_t *tenant)
{
    if (tenant->has_backlog) {
        return HWFQ_SUCCESS;
    }

    // Enqueue a session for this tenant in the system scheduler
    // Use TENANT_QUANTUM_SIZE as representative work size
    // No cleanup needed - tenant pointer is not allocated here
    int ret = group_scheduler_enqueue(
        scheduler->system_scheduler,
        tenant->tenant_id,
        TENANT_QUANTUM_SIZE,
        tenant,
        NULL,  // No cleanup for tenant sessions
        &tenant->tenant_session);

    if (ret == HWFQ_SUCCESS) {
        tenant->has_backlog = true;
    }

    return ret;
}


int hwfq_enqueue(hwfq_scheduler_t *scheduler,
                 hwfq_tenant_id_t tenant_id,
                 hwfq_flow_id_t flow_id,
                 const hwfq_session_t *work)
{
    if (scheduler == NULL || work == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (work->work_size == 0) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (tenant_id >= scheduler->config.max_tenants) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (flow_id == HWFQ_FLOW_ID_RESERVED) {
        return HWFQ_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&scheduler->lock);
    tenant_config_t *tenant = scheduler->tenants[tenant_id];
    if (tenant == NULL || !tenant->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }
    int ret = ensure_flow_entry(scheduler, tenant, flow_id);
    if (ret != HWFQ_SUCCESS) {
        pthread_mutex_unlock(&scheduler->lock);
        return ret;
    }
    flow_session_context_t *context = (flow_session_context_t *)hwfq_alloc(
        scheduler, sizeof(flow_session_context_t));
    if (context == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_MEMORY;
    }

    context->tenant_id = tenant_id;
    context->flow_id = flow_id;
    context->user_data = work->user_data;
    context->work_size = work->work_size;
    context->enqueue_time_ns = 0;  // Set below if stats enabled
    context->timeout_ns = work->timeout_ns;
    context->cleanup_fn = work->cleanup_fn;

    // Always track backlog count (needed for flow removal guard)
    tenant->stats.current_backlog++;
    {
        entry_config_t *flow_entry = find_flow_entry(tenant, flow_id);
        if (flow_entry != NULL) {
            flow_entry->stats.current_backlog++;
        }
    }

    if (scheduler->config.enable_statistics) {
        context->enqueue_time_ns = get_time_ns();
    }

    ret = group_scheduler_enqueue(
        tenant->flow_scheduler,
        flow_id,
        work->work_size,
        context,
        flow_context_cleanup,
        NULL);

    if (ret != HWFQ_SUCCESS) {
        tenant->stats.current_backlog--;
        entry_config_t *flow_entry = find_flow_entry(tenant, flow_id);
        if (flow_entry != NULL) {
            flow_entry->stats.current_backlog--;
        }
        hwfq_free(scheduler, context);
        pthread_mutex_unlock(&scheduler->lock);
        return ret;
    }

    if (!tenant->has_backlog) {
        ret = register_tenant_backlog(scheduler, tenant);
        // Note: If registration fails (rare, memory pressure), the work is queued
        // in flow_scheduler but tenant won't be scheduled until a subsequent
        // enqueue succeeds. This is acceptable - the work is not lost.
    }

    bool should_notify = false;
    if (scheduler->config.session_available_fn != NULL &&
        scheduler->in_flight_work_size < scheduler->config.total_capacity) {
        should_notify = true;
    }

    pthread_mutex_unlock(&scheduler->lock);

    if (should_notify) {
        scheduler->config.session_available_fn(scheduler);
    }

    return HWFQ_SUCCESS;
}

int hwfq_dequeue(hwfq_scheduler_t *scheduler,
                 hwfq_session_t *work_out,
                 hwfq_tenant_id_t *tenant_id_out,
                 hwfq_flow_id_t *flow_id_out)
{
    if (scheduler == NULL || work_out == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&scheduler->lock);

    if (group_scheduler_is_empty(scheduler->system_scheduler)) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_WORK;
    }

    session_state_t *tenant_session = group_scheduler_dequeue(scheduler->system_scheduler);
    if (tenant_session == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_WORK;
    }

    tenant_config_t *tenant = (tenant_config_t *)session_get_user_data(tenant_session);
    if (tenant == NULL) {
        hwfq_free(scheduler, tenant_session);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_INTERNAL;
    }

    tenant->has_backlog = false;
    tenant->tenant_session = NULL;

    hwfq_free(scheduler, tenant_session);

    session_state_t *flow_session = group_scheduler_dequeue(tenant->flow_scheduler);
    if (flow_session == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_INTERNAL;
    }

    flow_session_context_t *context = (flow_session_context_t *)session_get_user_data(flow_session);
    if (context == NULL) {
        hwfq_free(scheduler, flow_session);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_INTERNAL;
    }

    if (scheduler->config.total_capacity > 0 &&
        scheduler->in_flight_work_size + context->work_size > scheduler->config.total_capacity) {
        register_tenant_backlog(scheduler, tenant);
        group_scheduler_enqueue(tenant->flow_scheduler, context->flow_id,
                                context->work_size, context, flow_context_cleanup, NULL);
        hwfq_free(scheduler, flow_session);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_WORK;
    }

    work_out->user_data = context->user_data;
    work_out->work_size = context->work_size;
    work_out->timestamp = context->enqueue_time_ns;  // Return enqueue time if tracked

    if (tenant_id_out != NULL) {
        *tenant_id_out = context->tenant_id;
    }
    if (flow_id_out != NULL) {
        *flow_id_out = context->flow_id;
    }

    uint64_t dequeue_time = get_time_ns();
    in_flight_entry_t *entry = (in_flight_entry_t *)hwfq_alloc(scheduler, sizeof(in_flight_entry_t));
    if (entry == NULL) {
        register_tenant_backlog(scheduler, tenant);
        group_scheduler_enqueue(tenant->flow_scheduler, context->flow_id,
                                context->work_size, context, flow_context_cleanup, NULL);
        hwfq_free(scheduler, flow_session);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_MEMORY;
    }

    entry->tenant_id = context->tenant_id;
    entry->flow_id = context->flow_id;
    entry->work_size = context->work_size;
    entry->dequeue_time_ns = dequeue_time;
    entry->timeout_ns = context->timeout_ns;
    entry->user_data = context->user_data;
    in_flight_insert(scheduler, entry);
    scheduler->in_flight_work_size += context->work_size;
    scheduler->in_flight_count++;
    if (scheduler->config.enable_statistics) {
        entry_config_t *flow_entry = find_flow_entry(tenant, context->flow_id);
        if (flow_entry != NULL) {
            uint64_t wait_time_ns = 0;
            if (context->enqueue_time_ns > 0) {
                wait_time_ns = dequeue_time - context->enqueue_time_ns;
            }
            tenant->stats.total_wait_time_ns += wait_time_ns;
            flow_entry->stats.total_wait_time_ns += wait_time_ns;
        }
    }

    hwfq_free(scheduler, context);
    hwfq_free(scheduler, flow_session);
    if (!group_scheduler_is_empty(tenant->flow_scheduler)) {
        register_tenant_backlog(scheduler, tenant);
    }

    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

void hwfq_complete(hwfq_scheduler_t *scheduler,
                   const hwfq_session_t *work,
                   hwfq_tenant_id_t tenant_id,
                   hwfq_flow_id_t flow_id,
                   uint64_t completion_time_ns)
{
    if (scheduler == NULL) {
        return;
    }

    pthread_mutex_lock(&scheduler->lock);

    void *user_data = (work != NULL) ? work->user_data : NULL;
    in_flight_entry_t *found = NULL;

    if (user_data != NULL) {
        found = in_flight_find(scheduler, user_data, tenant_id, flow_id);
    } else {
        in_flight_entry_t *entry = scheduler->in_flight_head;
        while (entry != NULL) {
            if (entry->tenant_id == tenant_id && entry->flow_id == flow_id) {
                found = entry;
                break;
            }
            entry = entry->list_next;
        }
    }

    if (found == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return;
    }

    in_flight_remove(scheduler, found);
    scheduler->in_flight_work_size -= found->work_size;
    scheduler->in_flight_count--;

    tenant_config_t *tenant = NULL;
    entry_config_t *flow_entry = NULL;

    if (tenant_id < scheduler->config.max_tenants) {
        tenant = scheduler->tenants[tenant_id];
        if (tenant != NULL && tenant->configured) {
            flow_entry = find_flow_entry(tenant, flow_id);
        }
    }

    // Always track backlog count (needed for flow removal guard)
    if (tenant != NULL) {
        if (tenant->stats.current_backlog > 0) {
            tenant->stats.current_backlog--;
        }
        if (flow_entry != NULL && flow_entry->stats.current_backlog > 0) {
            flow_entry->stats.current_backlog--;
        }
    }

    if (scheduler->config.enable_statistics && tenant != NULL) {
        tenant->stats.work_units_processed += found->work_size;
        tenant->stats.operations_completed++;
        if (flow_entry != NULL) {
            flow_entry->stats.work_units_processed += found->work_size;
            flow_entry->stats.operations_completed++;
        }
        update_tenant_rate_tracking(tenant, found->work_size, completion_time_ns);
        if (flow_entry != NULL) {
            update_entry_rate_tracking(flow_entry, found->work_size, completion_time_ns);
        }
    }

    hwfq_free(scheduler, found);

    bool has_capacity = scheduler->in_flight_work_size < scheduler->config.total_capacity;
    bool has_work = !group_scheduler_is_empty(scheduler->system_scheduler);

    if (tenant != NULL && !group_scheduler_is_empty(tenant->flow_scheduler)) {
        register_tenant_backlog(scheduler, tenant);
        has_work = true;
    }

    pthread_mutex_unlock(&scheduler->lock);

    if (has_capacity && has_work && scheduler->config.session_available_fn != NULL) {
        scheduler->config.session_available_fn(scheduler);
    }
}

int hwfq_cancel(hwfq_scheduler_t *scheduler,
                const hwfq_session_t *work,
                hwfq_tenant_id_t tenant_id,
                hwfq_flow_id_t flow_id)
{
    if (scheduler == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&scheduler->lock);

    void *user_data = (work != NULL) ? work->user_data : NULL;
    in_flight_entry_t *found = NULL;

    if (user_data != NULL) {
        found = in_flight_find(scheduler, user_data, tenant_id, flow_id);
    } else {
        in_flight_entry_t *entry = scheduler->in_flight_head;
        while (entry != NULL) {
            if (entry->tenant_id == tenant_id && entry->flow_id == flow_id) {
                found = entry;
                break;
            }
            entry = entry->list_next;
        }
    }

    if (found == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    in_flight_remove(scheduler, found);
    scheduler->in_flight_work_size -= found->work_size;
    scheduler->in_flight_count--;

    // Always track backlog count (needed for flow removal guard)
    {
        tenant_config_t *tenant = NULL;
        entry_config_t *flow_entry = NULL;
        if (found->tenant_id < scheduler->config.max_tenants) {
            tenant = scheduler->tenants[found->tenant_id];
            if (tenant != NULL && tenant->configured) {
                flow_entry = find_flow_entry(tenant, found->flow_id);
            }
        }
        if (tenant != NULL) {
            if (tenant->stats.current_backlog > 0) {
                tenant->stats.current_backlog--;
            }
            if (flow_entry != NULL && flow_entry->stats.current_backlog > 0) {
                flow_entry->stats.current_backlog--;
            }
        }
    }

    bool has_capacity = scheduler->in_flight_work_size < scheduler->config.total_capacity;
    bool has_work = !group_scheduler_is_empty(scheduler->system_scheduler);

    pthread_mutex_unlock(&scheduler->lock);

    hwfq_free(scheduler, found);

    if (has_capacity && has_work && scheduler->config.session_available_fn != NULL) {
        scheduler->config.session_available_fn(scheduler);
    }

    return HWFQ_SUCCESS;
}

uint32_t hwfq_check_timeouts(hwfq_scheduler_t *scheduler, uint64_t current_time_ns)
{
    if (scheduler == NULL) {
        return 0;
    }

    pthread_mutex_lock(&scheduler->lock);

    uint32_t timeout_count = 0;

    in_flight_entry_t *entry = scheduler->in_flight_head;
    in_flight_entry_t *timed_out_list = NULL;

    while (entry != NULL) {
        in_flight_entry_t *next = entry->list_next;

        bool is_timed_out = false;
        if (entry->timeout_ns > 0) {
            uint64_t elapsed = current_time_ns - entry->dequeue_time_ns;
            is_timed_out = (elapsed >= entry->timeout_ns);
        }

        if (is_timed_out) {
            in_flight_remove(scheduler, entry);
            scheduler->in_flight_work_size -= entry->work_size;
            scheduler->in_flight_count--;
            entry->hash_next = timed_out_list;
            timed_out_list = entry;
            timeout_count++;
        }
        entry = next;
    }

    bool has_capacity = scheduler->in_flight_work_size < scheduler->config.total_capacity;
    bool has_work = !group_scheduler_is_empty(scheduler->system_scheduler);

    pthread_mutex_unlock(&scheduler->lock);

    while (timed_out_list != NULL) {
        in_flight_entry_t *e = timed_out_list;
        timed_out_list = e->hash_next;

        if (scheduler->config.session_timeout_fn != NULL) {
            scheduler->config.session_timeout_fn(scheduler,
                                                  e->tenant_id,
                                                  e->flow_id,
                                                  e->user_data,
                                                  e->work_size,
                                                  e->timeout_ns);
        }

        hwfq_free(scheduler, e);
    }

    if (timeout_count > 0 && has_capacity && has_work &&
        scheduler->config.session_available_fn != NULL) {
        scheduler->config.session_available_fn(scheduler);
    }

    return timeout_count;
}
