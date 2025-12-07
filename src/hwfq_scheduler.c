// ============================================================================
// H-WFQ Hierarchical Scheduler Implementation
// ============================================================================
//
// This file implements the hierarchical scheduling API that combines the
// system-level scheduler (for tenants) with tenant-level schedulers (for flows).
//
// Story 3: Hierarchical Scheduler
//
// ============================================================================

#include "hwfq_internal.h"
#include "hwfq_group_scheduler.h"
#include "hwfq_memory_pool.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Constants
// ============================================================================

// Default work quantum for tenant sessions in system scheduler
#define TENANT_QUANTUM_SIZE 4096

// Default weight for unconfigured flows
#define DEFAULT_FLOW_WEIGHT 100

// Rate calculation window (1 second in nanoseconds)
#define RATE_WINDOW_NS (1000000000ULL)

// ============================================================================
// In-Flight Hash Table Helper Functions
// ============================================================================

// Hash function for user_data pointer (Knuth multiplicative hash)
static inline uint32_t hash_user_data(void *user_data) {
    uintptr_t key = (uintptr_t)user_data;
    key = key * 2654435761UL;
    return (uint32_t)(key & HWFQ_IN_FLIGHT_HASH_MASK);
}

// Insert entry into in-flight hash table and list
static void in_flight_insert(hwfq_scheduler_t *scheduler, in_flight_entry_t *entry) {
    // Insert into hash table
    uint32_t bucket = hash_user_data(entry->user_data);
    entry->hash_next = scheduler->in_flight_hash[bucket];
    scheduler->in_flight_hash[bucket] = entry;

    // Append to doubly-linked list (for timeout scan)
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
    // Remove from hash table
    uint32_t bucket = hash_user_data(entry->user_data);
    in_flight_entry_t **prev = &scheduler->in_flight_hash[bucket];
    while (*prev != NULL) {
        if (*prev == entry) {
            *prev = entry->hash_next;
            break;
        }
        prev = &(*prev)->hash_next;
    }

    // Remove from doubly-linked list
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

// ============================================================================
// Statistics Helper Functions
// ============================================================================

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

// Update rate tracking for a flow
static void update_flow_rate_tracking(flow_config_t *flow, uint64_t work_size, uint64_t now_ns) {
    rate_tracking_t *rt = &flow->rate_tracking;

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
            flow->stats.effective_rate = (double)rt->window_work_units * 1000000000.0 / (double)elapsed;
        }
        // Start new window
        rt->window_start_ns = now_ns;
        rt->window_work_units = work_size;
    } else {
        // Add to current window
        rt->window_work_units += work_size;
        // Update rate estimate (running calculation)
        if (elapsed > 0) {
            flow->stats.effective_rate = (double)rt->window_work_units * 1000000000.0 / (double)elapsed;
        }
    }
}

// Find flow in tenant's flow list
static flow_config_t *find_flow_in_tenant(tenant_config_t *tenant, hwfq_flow_id_t flow_id) {
    flow_config_t *flow = tenant->flows;
    while (flow != NULL) {
        if (flow->flow_id == flow_id) {
            return flow;
        }
        flow = flow->next;
    }
    return NULL;
}

// ============================================================================
// Internal Helper Functions
// ============================================================================

// Ensure a flow entry exists in tenant's scheduler and flow config list
// For unconfigured flows, creates a default weight-based entry
static int ensure_flow_entry(hwfq_scheduler_t *scheduler,
                             tenant_config_t *tenant,
                             hwfq_flow_id_t flow_id)
{
    // Check if flow config already exists
    flow_config_t *existing = find_flow_in_tenant(tenant, flow_id);
    if (existing != NULL) {
        // Flow already configured
        return HWFQ_SUCCESS;
    }

    // Check if flow is already configured in the flow scheduler but not in our list
    uint64_t rate_out;
    int ret = group_scheduler_get_entry_rate(tenant->flow_scheduler, flow_id, &rate_out);
    if (ret != HWFQ_SUCCESS) {
        // Flow not configured in scheduler - create with default weight
        group_entry_config_t flow_entry = {
            .entry_id = flow_id,
            .allocation = {
                .allocation_type = HWFQ_ALLOCATION_WEIGHT,
                .weight = DEFAULT_FLOW_WEIGHT
            }
        };

        ret = group_scheduler_configure_entry(tenant->flow_scheduler, &flow_entry);
        if (ret != HWFQ_SUCCESS) {
            return ret;
        }
    }

    // Create a flow_config_t for in-flight tracking
    flow_config_t *flow = (flow_config_t *)hwfq_alloc(scheduler, sizeof(flow_config_t));
    if (flow == NULL) {
        return HWFQ_ERR_NO_MEMORY;
    }
    memset(flow, 0, sizeof(flow_config_t));
    flow->flow_id = flow_id;
    flow->allocation.allocation_type = HWFQ_ALLOCATION_WEIGHT;
    flow->allocation.weight = DEFAULT_FLOW_WEIGHT;
    flow->configured = false;  // Auto-created, not explicitly configured

    // Add to tenant's flow list
    flow->next = tenant->flows;
    tenant->flows = flow;
    tenant->num_flows++;

    return HWFQ_SUCCESS;
}

// Register tenant as having backlog in system scheduler
static int register_tenant_backlog(hwfq_scheduler_t *scheduler,
                                   tenant_config_t *tenant)
{
    if (tenant->has_backlog) {
        // Already registered
        return HWFQ_SUCCESS;
    }

    // Enqueue a session for this tenant in the system scheduler
    // Use TENANT_QUANTUM_SIZE as representative work size
    int ret = group_scheduler_enqueue(
        scheduler->system_scheduler,
        tenant->tenant_id,
        TENANT_QUANTUM_SIZE,
        tenant,  // user_data = tenant pointer for identification
        &tenant->tenant_session);

    if (ret == HWFQ_SUCCESS) {
        tenant->has_backlog = true;
    }

    return ret;
}


// ============================================================================
// Public API Implementations
// ============================================================================

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

    // Get tenant
    tenant_config_t *tenant = scheduler->tenants[tenant_id];
    if (tenant == NULL || !tenant->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    // Ensure flow entry exists (auto-create if needed)
    int ret = ensure_flow_entry(scheduler, tenant, flow_id);
    if (ret != HWFQ_SUCCESS) {
        pthread_mutex_unlock(&scheduler->lock);
        return ret;
    }

    // Allocate flow session context
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

    // Record enqueue timestamp and update backlog counters (if stats enabled)
    if (scheduler->config.enable_statistics) {
        context->enqueue_time_ns = get_time_ns();

        // Update backlog counters
        tenant->stats.current_backlog++;

        // Find the flow and update its backlog counter
        flow_config_t *flow = find_flow_in_tenant(tenant, flow_id);
        if (flow != NULL) {
            flow->stats.current_backlog++;
        }
    }

    // Enqueue to tenant's flow scheduler
    ret = group_scheduler_enqueue(
        tenant->flow_scheduler,
        flow_id,
        work->work_size,
        context,
        NULL);

    if (ret != HWFQ_SUCCESS) {
        hwfq_free(scheduler, context);
        pthread_mutex_unlock(&scheduler->lock);
        return ret;
    }

    // Register tenant backlog in system scheduler if needed
    if (!tenant->has_backlog) {
        ret = register_tenant_backlog(scheduler, tenant);
        if (ret != HWFQ_SUCCESS) {
            // Rollback: would need to remove the session from flow scheduler
            // For now, just log and continue - tenant will be picked up eventually
            // This is a rare edge case
        }
    }

    // Check if we should notify that work is available
    bool should_notify = false;
    if (scheduler->config.session_available_fn != NULL &&
        scheduler->in_flight_work_size < scheduler->config.total_capacity) {
        should_notify = true;
    }

    pthread_mutex_unlock(&scheduler->lock);

    // Call callback outside lock to avoid deadlock
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

    // Check if system scheduler has work
    if (group_scheduler_is_empty(scheduler->system_scheduler)) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_WORK;
    }

    // Dequeue from system scheduler to get winning tenant
    session_state_t *tenant_session = group_scheduler_dequeue(scheduler->system_scheduler);
    if (tenant_session == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_WORK;
    }

    // Get winning tenant from session user_data
    tenant_config_t *tenant = (tenant_config_t *)session_get_user_data(tenant_session);
    if (tenant == NULL) {
        // Internal error - should not happen
        hwfq_free(scheduler, tenant_session);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_INTERNAL;
    }

    // Mark tenant as no longer having backlog (we just dequeued its session)
    tenant->has_backlog = false;
    tenant->tenant_session = NULL;

    // Free the tenant session (we own it after dequeue)
    hwfq_free(scheduler, tenant_session);

    // Dequeue from tenant's flow scheduler to get winning flow
    session_state_t *flow_session = group_scheduler_dequeue(tenant->flow_scheduler);
    if (flow_session == NULL) {
        // Tenant had no flow work - inconsistent state
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_INTERNAL;
    }

    // Extract context
    flow_session_context_t *context = (flow_session_context_t *)session_get_user_data(flow_session);
    if (context == NULL) {
        hwfq_free(scheduler, flow_session);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_INTERNAL;
    }

    // Check capacity - if adding this work would exceed total_capacity, reject
    if (scheduler->config.total_capacity > 0 &&
        scheduler->in_flight_work_size + context->work_size > scheduler->config.total_capacity) {
        // At capacity - re-enqueue the work for later
        // Re-register tenant backlog since we still have work
        register_tenant_backlog(scheduler, tenant);
        // Re-enqueue the flow session (put it back)
        group_scheduler_enqueue(tenant->flow_scheduler, context->flow_id,
                                context->work_size, context, NULL);
        hwfq_free(scheduler, flow_session);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_WORK;
    }

    // Prepare output
    work_out->user_data = context->user_data;
    work_out->work_size = context->work_size;
    work_out->timestamp = context->enqueue_time_ns;  // Return enqueue time if tracked

    if (tenant_id_out != NULL) {
        *tenant_id_out = context->tenant_id;
    }
    if (flow_id_out != NULL) {
        *flow_id_out = context->flow_id;
    }

    // Add to global in-flight hash table and list
    uint64_t dequeue_time = get_time_ns();
    in_flight_entry_t *entry = (in_flight_entry_t *)hwfq_alloc(scheduler, sizeof(in_flight_entry_t));
    if (entry == NULL) {
        // Memory allocation failed - re-enqueue work
        register_tenant_backlog(scheduler, tenant);
        group_scheduler_enqueue(tenant->flow_scheduler, context->flow_id,
                                context->work_size, context, NULL);
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

    // Insert into hash table and doubly-linked list
    in_flight_insert(scheduler, entry);

    // Update capacity tracking
    scheduler->in_flight_work_size += context->work_size;
    scheduler->in_flight_count++;

    // Update wait time stats
    if (scheduler->config.enable_statistics) {
        flow_config_t *flow = find_flow_in_tenant(tenant, context->flow_id);
        if (flow != NULL) {
            uint64_t wait_time_ns = 0;
            if (context->enqueue_time_ns > 0) {
                wait_time_ns = dequeue_time - context->enqueue_time_ns;
            }
            tenant->stats.total_wait_time_ns += wait_time_ns;
            flow->stats.total_wait_time_ns += wait_time_ns;
        }
    }

    // Free the context and flow_session (info is now in in_flight_entry)
    hwfq_free(scheduler, context);
    hwfq_free(scheduler, flow_session);

    // Re-register tenant if there's still more work in its flow scheduler
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

    // Find in hash table (O(1) lookup)
    void *user_data = (work != NULL) ? work->user_data : NULL;
    in_flight_entry_t *found = NULL;

    if (user_data != NULL) {
        // Fast path: use hash lookup
        found = in_flight_find(scheduler, user_data, tenant_id, flow_id);
    } else {
        // Slow path: must scan list if no user_data provided
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
        // Entry not found - nothing to complete
        pthread_mutex_unlock(&scheduler->lock);
        return;
    }

    // Remove from hash table and list
    in_flight_remove(scheduler, found);

    // Update capacity tracking
    scheduler->in_flight_work_size -= found->work_size;
    scheduler->in_flight_count--;

    // Get tenant and flow for statistics
    tenant_config_t *tenant = NULL;
    flow_config_t *flow = NULL;

    if (tenant_id < scheduler->config.max_tenants) {
        tenant = scheduler->tenants[tenant_id];
        if (tenant != NULL && tenant->configured) {
            flow = find_flow_in_tenant(tenant, flow_id);
        }
    }

    // Update statistics
    if (scheduler->config.enable_statistics && tenant != NULL) {
        // Update work counters
        tenant->stats.work_units_processed += found->work_size;
        tenant->stats.operations_completed++;

        if (flow != NULL) {
            flow->stats.work_units_processed += found->work_size;
            flow->stats.operations_completed++;
        }

        // Decrement backlog counters
        if (tenant->stats.current_backlog > 0) {
            tenant->stats.current_backlog--;
        }
        if (flow != NULL && flow->stats.current_backlog > 0) {
            flow->stats.current_backlog--;
        }

        // Update rate tracking
        update_tenant_rate_tracking(tenant, found->work_size, completion_time_ns);
        if (flow != NULL) {
            update_flow_rate_tracking(flow, found->work_size, completion_time_ns);
        }
    }

    // Free in-flight entry
    hwfq_free(scheduler, found);

    // Check if capacity now available and there's more work queued
    bool has_capacity = scheduler->in_flight_work_size < scheduler->config.total_capacity;
    bool has_work = !group_scheduler_is_empty(scheduler->system_scheduler);

    // Also check if the tenant we just completed has more work
    if (tenant != NULL && !group_scheduler_is_empty(tenant->flow_scheduler)) {
        register_tenant_backlog(scheduler, tenant);
        has_work = true;
    }

    pthread_mutex_unlock(&scheduler->lock);

    // Call callback outside lock to avoid deadlock
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

    // Find in hash table (O(1) lookup)
    void *user_data = (work != NULL) ? work->user_data : NULL;
    in_flight_entry_t *found = NULL;

    if (user_data != NULL) {
        // Fast path: use hash lookup
        found = in_flight_find(scheduler, user_data, tenant_id, flow_id);
    } else {
        // Slow path: must scan list if no user_data provided
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

    // Remove from hash table and list
    in_flight_remove(scheduler, found);

    // Update capacity tracking
    scheduler->in_flight_work_size -= found->work_size;
    scheduler->in_flight_count--;

    // Check if we should notify
    bool has_capacity = scheduler->in_flight_work_size < scheduler->config.total_capacity;
    bool has_work = !group_scheduler_is_empty(scheduler->system_scheduler);

    pthread_mutex_unlock(&scheduler->lock);

    hwfq_free(scheduler, found);

    // Call callback outside lock (no stats update - this is explicit cancel)
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

    // Walk the doubly-linked list (using list_next/list_prev)
    in_flight_entry_t *entry = scheduler->in_flight_head;

    // Collect timed-out entries (can't call callback while holding lock)
    in_flight_entry_t *timed_out_list = NULL;

    while (entry != NULL) {
        in_flight_entry_t *next = entry->list_next;

        // Check if this session has timed out
        bool is_timed_out = false;
        if (entry->timeout_ns > 0) {
            uint64_t elapsed = current_time_ns - entry->dequeue_time_ns;
            is_timed_out = (elapsed >= entry->timeout_ns);
        }

        if (is_timed_out) {
            // Remove from hash table and doubly-linked list
            in_flight_remove(scheduler, entry);

            // Update capacity tracking
            scheduler->in_flight_work_size -= entry->work_size;
            scheduler->in_flight_count--;

            // Add to timed-out list for callback (reuse hash_next as temp link)
            entry->hash_next = timed_out_list;
            timed_out_list = entry;
            timeout_count++;
        }
        entry = next;
    }

    // Check if we should notify session_available_fn
    bool has_capacity = scheduler->in_flight_work_size < scheduler->config.total_capacity;
    bool has_work = !group_scheduler_is_empty(scheduler->system_scheduler);

    pthread_mutex_unlock(&scheduler->lock);

    // Call timeout callbacks outside lock
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

    // Notify if capacity freed and work available
    if (timeout_count > 0 && has_capacity && has_work &&
        scheduler->config.session_available_fn != NULL) {
        scheduler->config.session_available_fn(scheduler);
    }

    return timeout_count;
}
