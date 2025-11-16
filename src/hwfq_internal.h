#ifndef HWFQ_INTERNAL_H
#define HWFQ_INTERNAL_H

#include "hwfq.h"
#include <pthread.h>

// ============================================================================
// Internal Constants
// ============================================================================

#define HWFQ_DEFAULT_NUM_GROUPS 16
#define HWFQ_DEFAULT_BINS_PER_GROUP 2048
#define HWFQ_TOTAL_BINS (HWFQ_DEFAULT_NUM_GROUPS * HWFQ_DEFAULT_BINS_PER_GROUP) // 32768

// Special tenant/flow IDs for operations
#define HWFQ_ALL_TENANTS 0xFFFFFFFF
#define HWFQ_ALL_FLOWS 0xFFFFFFFF

// ============================================================================
// Internal Data Structures
// ============================================================================

// Forward declarations
typedef struct tenant_config_t tenant_config_t;
typedef struct flow_config_t flow_config_t;

// Flow configuration state (within a tenant)
struct flow_config_t {
    hwfq_flow_id_t flow_id;
    hwfq_allocation_t allocation;
    bool configured;            // Whether this flow has explicit configuration
    struct flow_config_t *next; // For hash table chaining
};

// Tenant configuration state
struct tenant_config_t {
    hwfq_tenant_id_t tenant_id;
    hwfq_allocation_t allocation;
    bool configured; // Whether this tenant is configured

    // Flow configurations (simple linked list for now, hash table in future stories)
    flow_config_t *flows;
    uint32_t num_flows;

    // Statistics (if enabled)
    hwfq_tenant_stats_t stats;
};

// Main scheduler structure
struct hwfq_scheduler_t {
    // Configuration
    hwfq_config_t config;

    // Memory allocators
    void *(*alloc_fn)(size_t);
    void (*free_fn)(void *);

    // Tenant configurations
    tenant_config_t **tenants; // Array of tenant pointers (size = max_tenants)
    uint32_t num_configured_tenants;

    // Capacity tracking (for rate-based allocations)
    uint64_t allocated_rate_capacity; // Sum of all rate-based allocations

    // Thread safety (future implementation)
    pthread_mutex_t lock;
};

// ============================================================================
// Internal Memory Management Functions
// ============================================================================

// Allocate memory using scheduler's configured allocator
void *hwfq_alloc(hwfq_scheduler_t *scheduler, size_t size);

// Free memory using scheduler's configured deallocator
void hwfq_free(hwfq_scheduler_t *scheduler, void *ptr);

// ============================================================================
// Internal Helper Functions
// ============================================================================

// ============================================================================
// Note: Scheduling functions will be added in Story 3 (Hierarchical Scheduler)
// ============================================================================

#endif // HWFQ_INTERNAL_H
