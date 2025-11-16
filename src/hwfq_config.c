#include "hwfq_internal.h"
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
    pthread_mutex_init(&scheduler->lock, NULL);
    *scheduler_out = scheduler;
    return HWFQ_SUCCESS;
}

void hwfq_destroy(hwfq_scheduler_t *scheduler)
{
    if (scheduler == NULL) {
        return;
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
    remove_flow(scheduler, tenant, flow_id);
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
    *stats_out = tenant->stats;
    stats_out->current_backlog = 0;
    stats_out->effective_rate = 0.0;
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_get_flow_stats(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                        hwfq_flow_id_t flow_id, hwfq_flow_stats_t *stats_out)
{
    return HWFQ_ERR_INTERNAL; // Not implemented yet
}

void hwfq_reset_stats(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                      hwfq_flow_id_t flow_id)
{
    // Not implemented yet
}
