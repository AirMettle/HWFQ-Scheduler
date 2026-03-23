#include "hwfq_internal.h"
#include "hwfq_chunked_entries.h"
#include "hwfq_group_scheduler.h"
#include "hwfq_group_scheduler_internal.h"
#include <stdlib.h>
#include <string.h>

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

static void remove_tenant(hwfq_scheduler_t *scheduler, tenant_config_t *tenant)
{
    if (scheduler == NULL || tenant == NULL) {
        return;
    }

    if (tenant->flow_scheduler != NULL) {
        group_scheduler_destroy(scheduler, tenant->flow_scheduler);
        tenant->flow_scheduler = NULL;
    }

    pthread_mutex_destroy(&tenant->lock);

    hwfq_chunked_entries_destroy(&tenant->flow_entries);

    if (tenant->configured && tenant->allocation.allocation_type == HWFQ_ALLOCATION_RATE) {
        scheduler->allocated_rate_capacity -= tenant->allocation.rate;
    }
    hwfq_free(scheduler, tenant);
}

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

    hwfq_chunked_entries_init(&scheduler->tenant_entries, scheduler->config.max_tenants,
                               scheduler->alloc_fn, scheduler->free_fn);

    scheduler->system_scheduler = group_scheduler_init_with_entries(
        scheduler,
        scheduler->config.num_groups,
        scheduler->config.bins_per_group,
        scheduler->config.total_capacity,
        scheduler->config.max_tenants,
        &scheduler->tenant_entries);
    if (scheduler->system_scheduler == NULL) {
        hwfq_chunked_entries_destroy(&scheduler->tenant_entries);
        hwfq_free(scheduler, scheduler->tenants);
        hwfq_free(scheduler, scheduler);
        return HWFQ_ERR_NO_MEMORY;
    }

    size_t hash_size = sizeof(in_flight_entry_t *) * HWFQ_IN_FLIGHT_HASH_BUCKETS;
    scheduler->in_flight_hash = (in_flight_entry_t **)hwfq_alloc(scheduler, hash_size);
    if (scheduler->in_flight_hash == NULL) {
        group_scheduler_destroy(scheduler, scheduler->system_scheduler);
        hwfq_chunked_entries_destroy(&scheduler->tenant_entries);
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

    hwfq_chunked_entries_destroy(&scheduler->tenant_entries);

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
    tenant->has_backlog = false;
    tenant->tenant_session = NULL;

    // Add 1 to max_flows_per_tenant to account for the reserved flow_id 0 slot
    hwfq_chunked_entries_init(&tenant->flow_entries, scheduler->config.max_flows_per_tenant + 1,
                               scheduler->alloc_fn, scheduler->free_fn);

    // Pre-allocate flow_id 0 to reserve it (HWFQ_FLOW_ID_RESERVED = 0)
    // This ensures hwfq_add_flow never returns 0 as a flow_id
    uint32_t reserved_id;
    if (hwfq_chunked_entries_alloc(&tenant->flow_entries, &reserved_id) == HWFQ_SUCCESS) {
        entry_config_t *reserved_entry = hwfq_chunked_get(&tenant->flow_entries, reserved_id);
        if (reserved_entry != NULL) {
            reserved_entry->configured = false;
        }
    }

    pthread_mutex_init(&tenant->lock, NULL);

    uint64_t tenant_capacity = (allocation->allocation_type == HWFQ_ALLOCATION_RATE)
        ? allocation->rate
        : scheduler->config.total_capacity;  // Weight-based gets full capacity (scaled by weight)

    tenant->flow_scheduler = group_scheduler_init_with_entries(
        scheduler,
        scheduler->config.num_groups,
        scheduler->config.bins_per_group,
        tenant_capacity,
        scheduler->config.max_flows_per_tenant + 1,  // +1 to account for reserved flow_id 0
        &tenant->flow_entries);  // Use per-tenant chunked entries
    if (tenant->flow_scheduler == NULL) {
        pthread_mutex_destroy(&tenant->lock);
        hwfq_chunked_entries_destroy(&tenant->flow_entries);
        hwfq_free(scheduler, tenant);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_MEMORY;
    }

    uint32_t allocated_tenant_id;
    int alloc_ret = hwfq_chunked_entries_alloc(&scheduler->tenant_entries, &allocated_tenant_id);
    if (alloc_ret != HWFQ_SUCCESS) {
        group_scheduler_destroy(scheduler, tenant->flow_scheduler);
        pthread_mutex_destroy(&tenant->lock);
        hwfq_chunked_entries_destroy(&tenant->flow_entries);
        hwfq_free(scheduler, tenant);
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_MEMORY;
    }

    group_entry_config_t tenant_entry = {
        .entry_id = allocated_tenant_id,
        .allocation = *allocation
    };
    int ret = group_scheduler_configure_entry(scheduler->system_scheduler, &tenant_entry);
    if (ret != HWFQ_SUCCESS) {
        hwfq_chunked_entries_free(&scheduler->tenant_entries, allocated_tenant_id);
        group_scheduler_destroy(scheduler, tenant->flow_scheduler);
        pthread_mutex_destroy(&tenant->lock);
        hwfq_chunked_entries_destroy(&tenant->flow_entries);
        hwfq_free(scheduler, tenant);
        pthread_mutex_unlock(&scheduler->lock);
        return ret;
    }

    tenant_id = allocated_tenant_id;
    tenant->tenant_id = tenant_id;

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

    group_scheduler_remove_entry(scheduler->system_scheduler, tenant_id);

    hwfq_chunked_entries_free(&scheduler->tenant_entries, tenant_id);

    remove_tenant(scheduler, tenant);
    scheduler->tenants[tenant_id] = NULL;
    scheduler->num_configured_tenants--;
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_add_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                   const hwfq_allocation_t *allocation, hwfq_flow_id_t *flow_id_out)
{
    if (scheduler == NULL || allocation == NULL || flow_id_out == NULL) {
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

    uint32_t flow_id;
    int alloc_ret = hwfq_chunked_entries_alloc(&tenant->flow_entries, &flow_id);
    if (alloc_ret != HWFQ_SUCCESS) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NO_MEMORY;
    }

    group_entry_config_t flow_entry = {
        .entry_id = flow_id,
        .allocation = *allocation
    };
    int ret = group_scheduler_configure_entry(tenant->flow_scheduler, &flow_entry);
    if (ret != HWFQ_SUCCESS) {
        hwfq_chunked_entries_free(&tenant->flow_entries, flow_id);
        pthread_mutex_unlock(&scheduler->lock);
        return ret;
    }

    *flow_id_out = flow_id;
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
    if (tenant == NULL || !tenant->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    entry_config_t *entry = hwfq_chunked_get(&tenant->flow_entries, flow_id);
    if (entry == NULL || !entry->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    // Check if flow has pending work - must drain first
    if (entry->stats.current_backlog > 0) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_TENANT_HAS_BACKLOG;
    }

    group_scheduler_remove_entry(tenant->flow_scheduler, flow_id);
    hwfq_chunked_entries_free(&tenant->flow_entries, flow_id);
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

int hwfq_reconfigure_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                          hwfq_flow_id_t flow_id, const hwfq_allocation_t *allocation)
{
    if (scheduler == NULL || allocation == NULL) {
        return HWFQ_ERR_INVALID_ARG;
    }
    if (flow_id == HWFQ_FLOW_ID_RESERVED) {
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

    entry_config_t *entry = hwfq_chunked_get(&tenant->flow_entries, flow_id);
    if (entry == NULL || !entry->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    group_entry_config_t flow_entry = {
        .entry_id = flow_id,
        .allocation = *allocation
    };
    int ret = group_scheduler_configure_entry(tenant->flow_scheduler, &flow_entry);

    pthread_mutex_unlock(&scheduler->lock);
    return ret;
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

    pthread_mutex_lock(&scheduler->lock);

    tenant_config_t *tenant = scheduler->tenants[tenant_id];
    if (tenant == NULL || !tenant->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    entry_config_t *entry = hwfq_chunked_get(&tenant->flow_entries, flow_id);
    if (entry == NULL || !entry->configured) {
        pthread_mutex_unlock(&scheduler->lock);
        return HWFQ_ERR_NOT_FOUND;
    }

    *stats_out = entry->stats;
    pthread_mutex_unlock(&scheduler->lock);
    return HWFQ_SUCCESS;
}

static void reset_tenant_stats(tenant_config_t *tenant) {
    uint32_t current_backlog = tenant->stats.current_backlog;
    memset(&tenant->stats, 0, sizeof(hwfq_tenant_stats_t));
    tenant->stats.current_backlog = current_backlog;  // Preserve backlog
    memset(&tenant->rate_tracking, 0, sizeof(rate_tracking_t));
}

static void reset_flow_stats(entry_config_t *entry) {
    if (entry == NULL || !entry->configured) return;
    uint64_t current_backlog = entry->stats.current_backlog;
    memset(&entry->stats, 0, sizeof(hwfq_flow_stats_t));
    entry->stats.current_backlog = current_backlog;  // Preserve backlog
    memset(&entry->rate_tracking, 0, sizeof(entry_rate_tracking_t));
}

static void reset_all_flow_stats(tenant_config_t *tenant) {
    for (uint32_t c = 0; c < tenant->flow_entries.num_chunks; c++) {
        if (tenant->flow_entries.chunks[c] == NULL) continue;
        for (uint32_t s = 0; s < HWFQ_CHUNK_SIZE; s++) {
            entry_config_t *entry = &tenant->flow_entries.chunks[c][s];
            if (entry->configured) {
                reset_flow_stats(entry);
            }
        }
    }
}

void hwfq_reset_stats(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                      hwfq_flow_id_t flow_id)
{
    if (scheduler == NULL) {
        return;
    }

    pthread_mutex_lock(&scheduler->lock);

    if (tenant_id == HWFQ_ALL_TENANTS) {
        for (uint32_t i = 0; i < scheduler->config.max_tenants; i++) {
            tenant_config_t *tenant = scheduler->tenants[i];
            if (tenant != NULL && tenant->configured) {
                reset_tenant_stats(tenant);
                reset_all_flow_stats(tenant);
            }
        }
    } else if (tenant_id < scheduler->config.max_tenants) {
        tenant_config_t *tenant = scheduler->tenants[tenant_id];
        if (tenant != NULL && tenant->configured) {
            if (flow_id == HWFQ_ALL_FLOWS) {
                reset_tenant_stats(tenant);
                reset_all_flow_stats(tenant);
            } else {
                entry_config_t *entry = hwfq_chunked_get(&tenant->flow_entries, flow_id);
                reset_flow_stats(entry);
            }
        }
    }

    pthread_mutex_unlock(&scheduler->lock);
}
