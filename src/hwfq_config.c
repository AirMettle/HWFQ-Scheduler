#include "hwfq_internal.h"
#include "hwfq_memory_pool.h"
#include "hwfq_entry_pool.h"
#include "hwfq_group_scheduler.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Internal Helper Functions
// ============================================================================

static bool validate_allocation(const hwfq_allocation_t *allocation)
{
    if (allocation == NULL) {
        return false;
    }
    if (allocation->allocation_type != HWFQ_ALLOCATION_WEIGHT &&
        allocation->allocation_type != HWFQ_ALLOCATION_RATE) {
        return false;
    }
    if (allocation->allocation_type == HWFQ_ALLOCATION_WEIGHT && allocation->weight == 0) {
        return false;
    }
    if (allocation->allocation_type == HWFQ_ALLOCATION_RATE && allocation->rate == 0) {
        return false;
    }
    return true;
}

static bool would_exceed_capacity(hwfq_scheduler_t *scheduler,
                                  const hwfq_allocation_t *new_allocation,
                                  const hwfq_allocation_t *old_allocation)
{
    if (scheduler == NULL || new_allocation == NULL) {
        return false;
    }
    if (new_allocation->allocation_type != HWFQ_ALLOCATION_RATE) {
        return false;
    }
    uint64_t current_allocated = scheduler->allocated_rate_capacity;
    if (old_allocation != NULL && old_allocation->allocation_type == HWFQ_ALLOCATION_RATE) {
        current_allocated -= old_allocation->rate;
    }
    uint64_t new_total = current_allocated + new_allocation->rate;
    return (new_total > scheduler->config.total_capacity);
}

static flow_config_t *find_flow(tenant_config_t *tenant, hwfq_flow_id_t flow_id)
{
    if (tenant == NULL) {
        return NULL;
    }
    flow_config_t *flow = tenant->flows;
    while (flow != NULL) {
        if (flow->flow_id == flow_id) {
            return flow;
        }
        flow = flow->next;
    }
    return NULL;
}

static flow_config_t *create_flow(hwfq_scheduler_t *scheduler, tenant_config_t *tenant,
                                  hwfq_flow_id_t flow_id)
{
    if (scheduler == NULL || tenant == NULL) {
        return NULL;
    }
    flow_config_t *existing = find_flow(tenant, flow_id);
    if (existing != NULL) {
        return existing;
    }
    flow_config_t *flow = (flow_config_t *)hwfq_alloc(scheduler, sizeof(flow_config_t));
    if (flow == NULL) {
        return NULL;
    }
    memset(flow, 0, sizeof(flow_config_t));
    flow->flow_id = flow_id;
    flow->configured = false;
    flow->next = tenant->flows;
    tenant->flows = flow;
    tenant->num_flows++;
    return flow;
}

static void remove_flow(hwfq_scheduler_t *scheduler, tenant_config_t *tenant,
                        hwfq_flow_id_t flow_id)
{
    if (scheduler == NULL || tenant == NULL) {
        return;
    }
    flow_config_t *prev = NULL;
    flow_config_t *curr = tenant->flows;
    while (curr != NULL) {
        if (curr->flow_id == flow_id) {
            if (prev == NULL) {
                tenant->flows = curr->next;
            } else {
                prev->next = curr->next;
            }
            hwfq_free(scheduler, curr);
            tenant->num_flows--;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

static void remove_tenant(hwfq_scheduler_t *scheduler, tenant_config_t *tenant)
{
    if (scheduler == NULL || tenant == NULL) {
        return;
    }

    // Destroy tenant's flow scheduler first
    if (tenant->flow_scheduler != NULL) {
        group_scheduler_destroy(scheduler, tenant->flow_scheduler);
        tenant->flow_scheduler = NULL;
    }

    // Destroy per-tenant lock
    pthread_mutex_destroy(&tenant->lock);

    flow_config_t *flow = tenant->flows;
    while (flow != NULL) {
        flow_config_t *next = flow->next;
        hwfq_free(scheduler, flow);
        flow = next;
    }
    if (tenant->configured && tenant->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
        scheduler->allocated_rate_capacity -= tenant->allocation.rate;
    }
    hwfq_free(scheduler, tenant);
}

// ============================================================================
// Public API Functions
// ============================================================================

int hwfq_init(const hwfq_config_t *config, hwfq_scheduler_t **scheduler_out)
{
    if (config == NULL || scheduler_out == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (config->max_tenants == 0 || config->max_flows_per_tenant == 0) {
        return HWFQ_ERR_INVALID_ARG;
    }
    void *(*alloc_fn)(size_t) = (config->alloc_fn != NULL) ? config->alloc_fn : malloc;
    hwfq_scheduler_t *scheduler = (hwfq_scheduler_t *)alloc_fn(sizeof(hwfq_scheduler_t));
    if (scheduler == NULL) {
        return HWFQ_ERR_NO_MEMORY;
    }
    memset(scheduler, 0, sizeof(hwfq_scheduler_t));
    scheduler->config = *config;
    if (scheduler->config.num_groups == 0) {
        scheduler->config.num_groups = HWFQ_DEFAULT_NUM_GROUPS;
    }
    if (scheduler->config.bins_per_group == 0) {
        scheduler->config.bins_per_group = HWFQ_DEFAULT_BINS_PER_GROUP;
    }
    scheduler->alloc_fn = config->alloc_fn;
    scheduler->free_fn = config->free_fn;
    size_t tenants_size = sizeof(tenant_config_t *) * config->max_tenants;
    scheduler->tenants = (tenant_config_t **)hwfq_alloc(scheduler, tenants_size);
    if (scheduler->tenants == NULL) {
        hwfq_free(scheduler, scheduler);
        return HWFQ_ERR_NO_MEMORY;
    }
    memset(scheduler->tenants, 0, tenants_size);
    scheduler->num_configured_tenants = 0;
    scheduler->allocated_rate_capacity = 0;

    // Initialize memory pool for session states
    uint32_t max_sessions = config->max_total_flows;
    if (max_sessions == 0) {
        // Default to max_tenants * max_flows_per_tenant if not specified
        max_sessions = config->max_tenants * config->max_flows_per_tenant;
        if (max_sessions == 0) {
            max_sessions = 10000;  // Fallback default
        }
    }
    scheduler->session_pool = (hwfq_memory_pool_t *)hwfq_alloc(scheduler, sizeof(hwfq_memory_pool_t));
    if (scheduler->session_pool == NULL) {
        hwfq_free(scheduler, scheduler->tenants);
        hwfq_free(scheduler, scheduler);
        return HWFQ_ERR_NO_MEMORY;
    }
    int pool_ret = hwfq_memory_pool_init(scheduler->session_pool, max_sessions,
                                         scheduler->alloc_fn, scheduler->free_fn);
    if (pool_ret != HWFQ_SUCCESS) {
        hwfq_free(scheduler, scheduler->session_pool);
        hwfq_free(scheduler, scheduler->tenants);
        hwfq_free(scheduler, scheduler);
        return pool_ret;
    }

    // Initialize shared entry pool for flow configurations
    // Sized to max_total_flows (shared across all tenants)
    scheduler->entry_pool = (hwfq_entry_pool_t *)hwfq_alloc(scheduler, sizeof(hwfq_entry_pool_t));
    if (scheduler->entry_pool == NULL) {
        hwfq_memory_pool_destroy(scheduler->session_pool);
        hwfq_free(scheduler, scheduler->session_pool);
        hwfq_free(scheduler, scheduler->tenants);
        hwfq_free(scheduler, scheduler);
        return HWFQ_ERR_NO_MEMORY;
    }
    int entry_pool_ret = hwfq_entry_pool_init(scheduler->entry_pool, max_sessions,
                                               scheduler->alloc_fn, scheduler->free_fn);
    if (entry_pool_ret != HWFQ_SUCCESS) {
        hwfq_free(scheduler, scheduler->entry_pool);
        hwfq_memory_pool_destroy(scheduler->session_pool);
        hwfq_free(scheduler, scheduler->session_pool);
        hwfq_free(scheduler, scheduler->tenants);
        hwfq_free(scheduler, scheduler);
        return entry_pool_ret;
    }

    // Initialize system-level scheduler (entries = tenants)
    scheduler->system_scheduler = group_scheduler_init(
        scheduler,
        scheduler->config.num_groups,
        scheduler->config.bins_per_group,
        scheduler->config.total_capacity,
        scheduler->config.max_tenants);
    if (scheduler->system_scheduler == NULL) {
        hwfq_memory_pool_destroy(scheduler->session_pool);
        hwfq_free(scheduler, scheduler->session_pool);
        hwfq_free(scheduler, scheduler->tenants);
        hwfq_free(scheduler, scheduler);
        return HWFQ_ERR_NO_MEMORY;
    }

    // Allocate in-flight hash table
    size_t hash_size = sizeof(in_flight_entry_t *) * HWFQ_IN_FLIGHT_HASH_BUCKETS;
    scheduler->in_flight_hash = (in_flight_entry_t **)hwfq_alloc(scheduler, hash_size);
    if (scheduler->in_flight_hash == NULL) {
        group_scheduler_destroy(scheduler, scheduler->system_scheduler);
        hwfq_memory_pool_destroy(scheduler->session_pool);
        hwfq_free(scheduler, scheduler->session_pool);
        hwfq_free(scheduler, scheduler->tenants);
        hwfq_free(scheduler, scheduler);
        return HWFQ_ERR_NO_MEMORY;
    }
    memset(scheduler->in_flight_hash, 0, hash_size);
    scheduler->in_flight_head = NULL;
    scheduler->in_flight_tail = NULL;

    pthread_mutex_init(&scheduler->lock, NULL);
    *scheduler_out = scheduler;
    return HWFQ_SUCCESS;
}

void hwfq_destroy(hwfq_scheduler_t *scheduler)
{
    if (scheduler == NULL) {
        return;
    }

    // Destroy system-level scheduler first
    if (scheduler->system_scheduler != NULL) {
        group_scheduler_destroy(scheduler, scheduler->system_scheduler);
        scheduler->system_scheduler = NULL;
    }

    if (scheduler->tenants != NULL) {
        for (uint32_t i = 0; i < scheduler->config.max_tenants; i++) {
            if (scheduler->tenants[i] != NULL) {
                remove_tenant(scheduler, scheduler->tenants[i]);
                scheduler->tenants[i] = NULL;
            }
        }
        hwfq_free(scheduler, scheduler->tenants);
    }

    // Destroy memory pool
    if (scheduler->session_pool != NULL) {
        hwfq_memory_pool_destroy(scheduler->session_pool);
        hwfq_free(scheduler, scheduler->session_pool);
        scheduler->session_pool = NULL;
    }

    // Destroy entry pool
    if (scheduler->entry_pool != NULL) {
        hwfq_entry_pool_destroy(scheduler->entry_pool);
        hwfq_free(scheduler, scheduler->entry_pool);
        scheduler->entry_pool = NULL;
    }

    // Free any remaining in-flight entries and the hash table
    if (scheduler->in_flight_hash != NULL) {
        in_flight_entry_t *entry = scheduler->in_flight_head;
        while (entry != NULL) {
            in_flight_entry_t *next = entry->list_next;
            hwfq_free(scheduler, entry);
            entry = next;
        }
        hwfq_free(scheduler, scheduler->in_flight_hash);
        scheduler->in_flight_hash = NULL;
    }

    pthread_mutex_destroy(&scheduler->lock);
    hwfq_free(scheduler, scheduler);
}

int hwfq_add_tenant(hwfq_scheduler_t *scheduler, const hwfq_allocation_t *allocation,
                    hwfq_tenant_id_t *tenant_id_out)
{
    if (scheduler == NULL || allocation == NULL || tenant_id_out == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (!validate_allocation(allocation)) {
        return HWFQ_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&scheduler->lock);
    hwfq_tenant_id_t tenant_id = 0;
    bool found = false;
    for (tenant_id = 0; tenant_id < scheduler->config.max_tenants; tenant_id++) {
        if (scheduler->tenants[tenant_id] == NULL) {
            found = true;
            break;
        }
    }
    if (!found) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_MEMORY;
    }
    if (would_exceed_capacity(scheduler, allocation, NULL)) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_OVERBOOKED;
    }
    tenant_config_t *tenant = (tenant_config_t *)hwfq_alloc(scheduler, sizeof(tenant_config_t));
    if (tenant == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_MEMORY;
    }
    memset(tenant, 0, sizeof(tenant_config_t));
    tenant->tenant_id = tenant_id;
    tenant->allocation = *allocation;
    tenant->configured = true;
    tenant->flows = NULL;
    tenant->num_flows = 0;
    tenant->has_backlog = false;
    tenant->tenant_session = NULL;

    // Initialize per-tenant lock
    pthread_mutex_init(&tenant->lock, NULL);

    // Create tenant-level flow scheduler with shared entry pool
    // Use tenant's allocation as capacity for the flow scheduler
    uint64_t tenant_capacity = (allocation->allocation_type == HWFQ_ALLOCATION_RATE)
        ? allocation->rate
        : scheduler->config.total_capacity;  // Weight-based gets full capacity (scaled by weight)

    tenant->flow_scheduler = group_scheduler_init_with_pool(
        scheduler,
        scheduler->config.num_groups,
        scheduler->config.bins_per_group,
        tenant_capacity,
        scheduler->config.max_flows_per_tenant,
        scheduler->entry_pool);  // Use shared entry pool
    if (tenant->flow_scheduler == NULL) {
        pthread_mutex_destroy(&tenant->lock);
        hwfq_free(scheduler, tenant);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_MEMORY;
    }

    // Configure tenant as entry in system scheduler
    group_entry_config_t tenant_entry = {
        .entry_id = tenant_id,
        .allocation = *allocation
    };
    int ret = group_scheduler_configure_entry(scheduler->system_scheduler, &tenant_entry);
    if (ret != HWFQ_SUCCESS) {
        group_scheduler_destroy(scheduler, tenant->flow_scheduler);
        hwfq_free(scheduler, tenant);
        pthread_mutex_unlock(&scheduler->lock);
        return ret;
    }

    scheduler->tenants[tenant_id] = tenant;
    scheduler->num_configured_tenants++;
    if (allocation->allocation_type == HWFQ_ALLOCATION_RATE) {
        scheduler->allocated_rate_capacity += allocation->rate;
    }
    *tenant_id_out = tenant_id;
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_configure_tenant(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                          const hwfq_allocation_t *allocation)
{
    if (scheduler == NULL || allocation == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (tenant_id >= scheduler->config.max_tenants) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (!validate_allocation(allocation)) {
        return HWFQ_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&scheduler->lock);
    tenant_config_t *tenant = scheduler->tenants[tenant_id];
    if (tenant == NULL || !tenant->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }
    if (would_exceed_capacity(scheduler, allocation, &tenant->allocation)) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_OVERBOOKED;
    }
    if (allocation->allocation_type == HWFQ_ALLOCATION_RATE) {
        if (tenant->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
            scheduler->allocated_rate_capacity -= tenant->allocation.rate;
        }
        scheduler->allocated_rate_capacity += allocation->rate;
    } else if (tenant->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
        scheduler->allocated_rate_capacity -= tenant->allocation.rate;
    }
    tenant->allocation = *allocation;

    // Update tenant entry in system scheduler
    group_entry_config_t tenant_entry = {
        .entry_id = tenant_id,
        .allocation = *allocation
    };
    group_scheduler_configure_entry(scheduler->system_scheduler, &tenant_entry);

    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_remove_tenant(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id)
{
    if (scheduler == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (tenant_id >= scheduler->config.max_tenants) {
        return HWFQ_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&scheduler->lock);
    tenant_config_t *tenant = scheduler->tenants[tenant_id];
    if (tenant == NULL || !tenant->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    // Check if tenant has pending work - must drain first
    if (tenant->has_backlog) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_TENANT_HAS_BACKLOG;
    }

    // Remove tenant from system scheduler
    group_scheduler_remove_entry(scheduler->system_scheduler, tenant_id);

    remove_tenant(scheduler, tenant);
    scheduler->tenants[tenant_id] = NULL;
    scheduler->num_configured_tenants--;
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_configure_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                        hwfq_flow_id_t flow_id, const hwfq_allocation_t *allocation)
{
    if (scheduler == NULL || allocation == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (tenant_id >= scheduler->config.max_tenants) {
        return HWFQ_ERR_INVALID_ARG;
    }
    // Flow ID 0 is reserved
    if (flow_id == HWFQ_FLOW_ID_RESERVED) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (!validate_allocation(allocation)) {
        return HWFQ_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&scheduler->lock);
    tenant_config_t *tenant = scheduler->tenants[tenant_id];
    if (tenant == NULL || !tenant->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }
    flow_config_t *flow = create_flow(scheduler, tenant, flow_id);
    if (flow == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_MEMORY;
    }
    flow->allocation = *allocation;
    flow->configured = true;

    // Configure flow as entry in tenant's flow scheduler
    group_entry_config_t flow_entry = {
        .entry_id = flow_id,
        .allocation = *allocation
    };
    int ret = group_scheduler_configure_entry(tenant->flow_scheduler, &flow_entry);
    if (ret != HWFQ_SUCCESS) {
        remove_flow(scheduler, tenant, flow_id);
        pthread_mutex_unlock(&scheduler->lock);
        return ret;
    }

    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_remove_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                     hwfq_flow_id_t flow_id)
{
    if (scheduler == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (tenant_id >= scheduler->config.max_tenants) {
        return HWFQ_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&scheduler->lock);
    tenant_config_t *tenant = scheduler->tenants[tenant_id];
    if (tenant == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }
    flow_config_t *flow = find_flow(tenant, flow_id);
    if (flow == NULL || !flow->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    // Remove flow from tenant's flow scheduler
    group_scheduler_remove_entry(tenant->flow_scheduler, flow_id);

    remove_flow(scheduler, tenant, flow_id);
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_reconfigure_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                          hwfq_flow_id_t flow_id, const hwfq_allocation_t *allocation)
{
    if (scheduler == NULL || allocation == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (tenant_id >= scheduler->config.max_tenants) {
        return HWFQ_ERR_INVALID_ARG;
    }
    // Flow ID 0 is reserved
    if (flow_id == HWFQ_FLOW_ID_RESERVED) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (!validate_allocation(allocation)) {
        return HWFQ_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&scheduler->lock);
    tenant_config_t *tenant = scheduler->tenants[tenant_id];
    if (tenant == NULL || !tenant->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    flow_config_t *flow = find_flow(tenant, flow_id);
    if (flow == NULL || !flow->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    // For rate-based allocations, check overbooking
    // Note: This is a simplified check at the flow level
    // In a full implementation, would need to track per-tenant rate capacity

    flow->allocation = *allocation;
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_get_capacity_info(hwfq_scheduler_t *scheduler, hwfq_capacity_info_t *capacity_info_out)
{
    if (scheduler == NULL || capacity_info_out == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&scheduler->lock);
    capacity_info_out->total_capacity = scheduler->config.total_capacity;
    capacity_info_out->allocated_capacity = scheduler->allocated_rate_capacity;
    capacity_info_out->available_capacity =
        scheduler->config.total_capacity - scheduler->allocated_rate_capacity;
    capacity_info_out->num_configured_tenants = scheduler->num_configured_tenants;
    capacity_info_out->num_active_flows = 0;
    if (scheduler->config.total_capacity > 0) {
        capacity_info_out->utilization_percent = (double)scheduler->allocated_rate_capacity /
                                                 (double)scheduler->config.total_capacity * 100.0;
    } else {
        capacity_info_out->utilization_percent = 0.0;
    }
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_get_tenant_stats(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                          hwfq_tenant_stats_t *stats_out)
{
    if (scheduler == NULL || stats_out == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (tenant_id >= scheduler->config.max_tenants) {
        return HWFQ_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&scheduler->lock);
    tenant_config_t *tenant = scheduler->tenants[tenant_id];
    if (tenant == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }
    // Copy all stats - current_backlog and effective_rate are maintained
    // during enqueue/dequeue when statistics are enabled
    *stats_out = tenant->stats;
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_get_flow_stats(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                        hwfq_flow_id_t flow_id, hwfq_flow_stats_t *stats_out)
{
    if (scheduler == NULL || stats_out == NULL) {
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
    if (tenant == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    // Find the flow
    flow_config_t *flow = find_flow(tenant, flow_id);
    if (flow == NULL) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    // Copy stats
    *stats_out = flow->stats;
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

// Helper to reset a single flow's stats (preserves current_backlog)
static void reset_flow_stats(flow_config_t *flow) {
    uint32_t current_backlog = flow->stats.current_backlog;
    memset(&flow->stats, 0, sizeof(hwfq_flow_stats_t));
    flow->stats.current_backlog = current_backlog;  // Preserve backlog
    memset(&flow->rate_tracking, 0, sizeof(rate_tracking_t));
}

// Helper to reset a single tenant's stats (preserves current_backlog)
static void reset_tenant_stats(tenant_config_t *tenant) {
    uint32_t current_backlog = tenant->stats.current_backlog;
    memset(&tenant->stats, 0, sizeof(hwfq_tenant_stats_t));
    tenant->stats.current_backlog = current_backlog;  // Preserve backlog
    memset(&tenant->rate_tracking, 0, sizeof(rate_tracking_t));
}

void hwfq_reset_stats(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                      hwfq_flow_id_t flow_id)
{
    if (scheduler == NULL) {
        return;
    }

    pthread_mutex_lock(&scheduler->lock);

    if (tenant_id == HWFQ_ALL_TENANTS) {
        // Reset all tenants and all flows
        for (uint32_t i = 0; i < scheduler->config.max_tenants; i++) {
            tenant_config_t *tenant = scheduler->tenants[i];
            if (tenant != NULL && tenant->configured) {
                reset_tenant_stats(tenant);
                // Reset all flows for this tenant
                flow_config_t *flow = tenant->flows;
                while (flow != NULL) {
                    reset_flow_stats(flow);
                    flow = flow->next;
                }
            }
        }
    } else if (tenant_id < scheduler->config.max_tenants) {
        tenant_config_t *tenant = scheduler->tenants[tenant_id];
        if (tenant != NULL && tenant->configured) {
            if (flow_id == HWFQ_ALL_FLOWS) {
                // Reset this tenant and all its flows
                reset_tenant_stats(tenant);
                flow_config_t *flow = tenant->flows;
                while (flow != NULL) {
                    reset_flow_stats(flow);
                    flow = flow->next;
                }
            } else if (flow_id != HWFQ_FLOW_ID_RESERVED) {
                // Reset specific flow only
                flow_config_t *flow = find_flow(tenant, flow_id);
                if (flow != NULL) {
                    reset_flow_stats(flow);
                }
            }
        }
    }

    pthread_mutex_unlock(&scheduler->lock);
}
