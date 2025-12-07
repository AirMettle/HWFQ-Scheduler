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

// In-flight hash table size (power of 2 for fast modulo)
#define HWFQ_IN_FLIGHT_HASH_BUCKETS 1024
#define HWFQ_IN_FLIGHT_HASH_MASK (HWFQ_IN_FLIGHT_HASH_BUCKETS - 1)

// ============================================================================
// Internal Data Structures
// ============================================================================

// Forward declarations
typedef struct tenant_config_t tenant_config_t;
typedef struct flow_config_t flow_config_t;
typedef struct hwfq_memory_pool_t hwfq_memory_pool_t;
typedef struct hwfq_entry_pool_t hwfq_entry_pool_t;
typedef struct group_scheduler_t group_scheduler_t;
typedef struct session_state_t session_state_t;

// Rate tracking for effective_rate calculation
typedef struct {
    uint64_t window_start_ns;   // Start of current measurement window
    uint64_t window_work_units; // Work units processed in current window
} rate_tracking_t;

// Flow session context - wraps user session with hierarchical metadata
// Forward declaration (full definition below)
typedef struct flow_session_context flow_session_context_t;

// Flow configuration state (within a tenant)
struct flow_config_t {
    hwfq_flow_id_t flow_id;
    hwfq_allocation_t allocation;
    bool configured;            // Whether this flow has explicit configuration
    struct flow_config_t *next; // For hash table chaining
    hwfq_flow_stats_t stats;    // Per-flow statistics (if enabled)
    rate_tracking_t rate_tracking;  // For effective_rate calculation
};

// In-flight session entry (for scheduler-level tracking)
// Uses hash table for O(1) lookup by user_data, plus doubly-linked list for timeout scan
typedef struct in_flight_entry {
    hwfq_tenant_id_t tenant_id;
    hwfq_flow_id_t flow_id;
    uint64_t work_size;
    uint64_t dequeue_time_ns;
    uint64_t timeout_ns;                // Per-session timeout (0 = no timeout)
    void *user_data;                    // Hash key for O(1) lookup on complete()
    struct in_flight_entry *hash_next;  // Hash chain (for collisions in same bucket)
    struct in_flight_entry *list_next;  // Doubly-linked list for timeout scan
    struct in_flight_entry *list_prev;  // For O(1) removal from list
} in_flight_entry_t;

// Tenant configuration state
struct tenant_config_t {
    hwfq_tenant_id_t tenant_id;
    hwfq_allocation_t allocation;
    bool configured; // Whether this tenant is configured

    // Flow configurations (simple linked list for now, hash table in future stories)
    flow_config_t *flows;
    uint32_t num_flows;

    // Tenant-level scheduler for flows (Story 3: Hierarchical Scheduler)
    group_scheduler_t *flow_scheduler;  // Entries = flows within this tenant

    // Backlog tracking for system-level scheduler
    bool has_backlog;                   // Is tenant registered in system scheduler?
    session_state_t *tenant_session;    // Tenant's session handle in system scheduler

    // Statistics (if enabled)
    hwfq_tenant_stats_t stats;
    rate_tracking_t rate_tracking;  // For effective_rate calculation

    // Per-tenant lock for flow scheduler operations
    pthread_mutex_t lock;
};

// Flow session context - wraps user session with hierarchical metadata
struct flow_session_context {
    hwfq_tenant_id_t tenant_id;
    hwfq_flow_id_t flow_id;
    void *user_data;
    uint64_t work_size;
    uint64_t enqueue_time_ns;  // Enqueue timestamp for wait time calculation
    uint64_t timeout_ns;       // Per-session timeout (0 = no timeout)
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

    // Pre-allocated memory pool for session states
    hwfq_memory_pool_t *session_pool;

    // Shared entry pool for flow configurations (all tenants share this)
    hwfq_entry_pool_t *entry_pool;

    // System-level scheduler (Story 3: Hierarchical Scheduler)
    group_scheduler_t *system_scheduler;  // Entries = tenants

    // In-flight work tracking (capacity-based) with O(1) hash table lookup
    uint64_t in_flight_work_size;       // Sum of work_size for all in-flight sessions
    uint32_t in_flight_count;           // Number of in-flight sessions
    in_flight_entry_t **in_flight_hash; // Hash buckets for O(1) lookup by user_data
    in_flight_entry_t *in_flight_head;  // Doubly-linked list head (for timeout scan)
    in_flight_entry_t *in_flight_tail;  // Doubly-linked list tail (for O(1) append)

    // Thread safety (global lock)
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
