#ifndef HWFQ_H
#define HWFQ_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Type Definitions
// ============================================================================

// H-WFQ Scheduler handle (opaque)
typedef struct hwfq_scheduler_t hwfq_scheduler_t;

// Tenant identifier
typedef uint32_t hwfq_tenant_id_t;

// Flow identifier (unique within a tenant)
typedef uint32_t hwfq_flow_id_t;

// ============================================================================
// Error Codes
// ============================================================================

#define HWFQ_SUCCESS 0            // Operation completed successfully
#define HWFQ_ERR_INVALID_ARG -1   // Invalid argument provided
#define HWFQ_ERR_NO_MEMORY -2     // Memory allocation failed
#define HWFQ_ERR_NOT_FOUND -3     // Tenant or flow not found
#define HWFQ_ERR_ALREADY_EXIST -4 // Tenant or flow already exists
#define HWFQ_ERR_OVERBOOKED -5    // Allocation would exceed total system capacity
#define HWFQ_ERR_NO_WORK -6       // No work available (dequeue on empty scheduler)
#define HWFQ_ERR_INTERNAL -99     // Internal error (should never occur)

// ============================================================================
// Configuration Structures
// ============================================================================

// Allocation type for resource allocation
typedef enum {
    HWFQ_ALLOCATION_WEIGHT, // Relative weight (shares)
    HWFQ_ALLOCATION_RATE    // Absolute rate (work units/sec)
} hwfq_allocation_type_t;

// Weight/rate specification for resource allocation
typedef struct {
    hwfq_allocation_type_t allocation_type;
    union {
        uint32_t weight; // Relative weight (e.g., 100 for proportional share)
        uint64_t rate;   // Rate in work units per second
    };
} hwfq_allocation_t;

// Configuration for scheduler initialization
typedef struct {
    uint32_t max_tenants;          // Maximum number of tenants (e.g., 4000)
    uint32_t max_flows_per_tenant; // Maximum flows per tenant (e.g., 100000)
    uint32_t num_groups;           // Number of service interval groups (default: 16)
    uint32_t bins_per_group;       // Bins per group for finish times (default: 2048)

    // Total system capacity specification
    uint64_t total_capacity; // Total node capacity in work units/sec

    // Statistics and monitoring
    bool enable_statistics; // Enable per-flow statistics collection

    // Custom memory allocators (NULL = use malloc/free)
    void *(*alloc_fn)(size_t); // Custom allocator (NULL = use malloc)
    void (*free_fn)(void *);   // Custom deallocator (NULL = use free)

    // Callback function for when a session is ready to be dequeued
    void (*session_available_fn)(hwfq_scheduler_t *);
} hwfq_config_t;

// System capacity information
typedef struct {
    uint64_t total_capacity;         // Total system capacity (work units/sec)
    uint64_t allocated_capacity;     // Currently allocated to tenants (rate-based)
    uint64_t available_capacity;     // Remaining unallocated capacity
    uint32_t num_configured_tenants; // Number of configured tenants
    uint32_t num_active_flows;       // Number of flows with pending work
    double utilization_percent;      // Current system utilization (0-100%)
} hwfq_capacity_info_t;

// ============================================================================
// Global Configuration APIs
// ============================================================================

// Initialize a new H-WFQ scheduler instance
//
// config - Configuration parameters
// scheduler_out - Output parameter for scheduler handle
// returns - 0 on success, negative error code on failure
int hwfq_init(const hwfq_config_t *config, hwfq_scheduler_t **scheduler_out);

// Destroy scheduler and free all resources
//
// scheduler - Scheduler handle to destroy
void hwfq_destroy(hwfq_scheduler_t *scheduler);

// ============================================================================
// Tenant Configuration APIs
// ============================================================================

// Add a new tenant to the scheduler
//
// scheduler - Scheduler handle
// allocation - Resource allocation specification
// tenant_id_out - Output parameter for assigned tenant ID
// returns - 0 on success, negative error code on failure
//           -HWFQ_ERR_OVERBOOKED if allocation would exceed system capacity
//           -HWFQ_ERR_NO_MEMORY if all tenant slots are full or allocation fails
//
// Overbooking Prevention:
// - When using HWFQ_ALLOCATION_RATE: The function checks that the sum of all
//   tenant rates (including this new allocation) does not exceed the
//   total_capacity specified during initialization. Returns
//   HWFQ_ERR_OVERBOOKED if the allocation would violate this constraint.
//
// - When using HWFQ_ALLOCATION_WEIGHT: Weights are relative shares of
//   available capacity. No overbooking is possible as
//   weights represent proportions, not absolute guarantees.
//
// Tenant ID Assignment:
// - The function finds the first available tenant slot and assigns that ID
// - Tenant IDs can be recycled after hwfq_remove_tenant() is called
// - The assigned ID is returned via tenant_id_out
int hwfq_add_tenant(hwfq_scheduler_t *scheduler, const hwfq_allocation_t *allocation,
                    hwfq_tenant_id_t *tenant_id_out);

// Configure an existing tenant's resource allocation
//
// scheduler - Scheduler handle
// tenant_id - Tenant identifier (previously returned by hwfq_add_tenant)
// allocation - Resource allocation specification
// returns - 0 on success, negative error code on failure
//           -HWFQ_ERR_NOT_FOUND if tenant doesn't exist
//           -HWFQ_ERR_OVERBOOKED if allocation would exceed system capacity
//
// Overbooking Prevention:
// - When using HWFQ_ALLOCATION_RATE: The function checks that the sum of all
//   tenant rates (including this modified allocation) does not exceed the
//   total_capacity specified during initialization. Returns
//   HWFQ_ERR_OVERBOOKED if the allocation would violate this constraint.
//
// - When using HWFQ_ALLOCATION_WEIGHT: Weights are relative shares of
//   available capacity. No overbooking is possible as
//   weights represent proportions, not absolute guarantees.
//
// NOTE: Changes take effect immediately on the next scheduling decision.
//       No system restart required.
int hwfq_configure_tenant(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                          const hwfq_allocation_t *allocation);

// Remove a tenant configuration
//
// scheduler - Scheduler handle
// tenant_id - Tenant identifier
// returns - 0 on success, negative error code on failure
int hwfq_remove_tenant(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id);

// ============================================================================
// Flow Configuration APIs
// ============================================================================

// Configure a flow's resource allocation within its tenant
//
// scheduler - Scheduler handle
// tenant_id - Tenant owning this flow
// flow_id - Flow identifier (e.g., process ID, thread ID)
// allocation - Resource allocation within tenant's share
// returns - 0 on success, negative error code on failure
//
// NOTE: If flow is not configured, it gets equal share with other
//       unconfigured flows within the tenant.
int hwfq_configure_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                        hwfq_flow_id_t flow_id, const hwfq_allocation_t *allocation);

// Remove a flow configuration
//
// scheduler - Scheduler handle
// tenant_id - Tenant owning this flow
// flow_id - Flow identifier
// returns - 0 on success, negative error code on failure
int hwfq_remove_flow(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                     hwfq_flow_id_t flow_id);

// ============================================================================
// Capacity Management and Monitoring APIs
// ============================================================================

// Get system capacity and allocation information
//
// scheduler - Scheduler handle
// capacity_info_out - Output parameter for capacity information
// returns - 0 on success, negative error code on failure
//
// This function provides administrators visibility into:
// - How much total capacity is configured
// - How much capacity is currently allocated to tenants
// - How much capacity remains available for new allocations
// - Current system utilization
int hwfq_get_capacity_info(hwfq_scheduler_t *scheduler, hwfq_capacity_info_t *capacity_info_out);

// ============================================================================
// Scheduling APIs (Stubs for Future Implementation)
// ============================================================================

// Session to be scheduled
typedef struct {
    void *user_data;    // User-defined data pointer
    size_t work_size;   // Size of work (bytes, ops, etc.)
    uint64_t timestamp; // Enqueue timestamp (optional)
} hwfq_session_t;

// Enqueue work for scheduling
//
// scheduler - Scheduler handle
// tenant_id - Tenant submitting work
// flow_id - Flow submitting work (0 if not tracked)
// work - session to enqueue
// returns - 0 on success, negative error code on failure
int hwfq_enqueue(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id, hwfq_flow_id_t flow_id,
                 const hwfq_session_t *work);

// Dequeue next session according to H-WFQ policy
//
// scheduler - Scheduler handle
// work_out - Output parameter for next session
// tenant_id_out - Output parameter for tenant ID (optional, can be NULL)
// flow_id_out - Output parameter for flow ID (optional, can be NULL)
// returns - 0 on success, -EAGAIN if no work available, negative error code on failure
int hwfq_dequeue(hwfq_scheduler_t *scheduler, hwfq_session_t *work_out,
                 hwfq_tenant_id_t *tenant_id_out, hwfq_flow_id_t *flow_id_out);

// Notify scheduler that work has completed
// Used to update virtual time and statistics
//
// scheduler - Scheduler handle
// work - session that completed
// tenant_id - Tenant that owned the work
// flow_id - Flow that owned the work
// completion_time_ns - Time when work completed (nanoseconds)
void hwfq_complete(hwfq_scheduler_t *scheduler, const hwfq_session_t *work,
                   hwfq_tenant_id_t tenant_id, hwfq_flow_id_t flow_id, uint64_t completion_time_ns);

// ============================================================================
// Statistics and Monitoring APIs (Stubs for Future Implementation)
// ============================================================================

// Per-tenant statistics
typedef struct {
    uint64_t work_units_processed;  // Total work units processed
    uint64_t operations_completed;  // Total operations completed
    uint64_t total_wait_time_ns;    // Cumulative wait time
    uint64_t total_service_time_ns; // Cumulative service time
    uint32_t current_backlog;       // Current queued sessions
    double effective_rate;          // Current effective rate (work units/sec)
} hwfq_tenant_stats_t;

// Per-flow statistics
typedef struct {
    uint64_t work_units_processed;
    uint64_t operations_completed;
    uint64_t total_wait_time_ns;
    uint64_t total_service_time_ns;
    uint32_t current_backlog;
    double effective_rate;
} hwfq_flow_stats_t;

// Get statistics for a tenant
//
// scheduler - Scheduler handle
// tenant_id - Tenant to query
// stats_out - Output parameter for statistics
// returns - 0 on success, negative error code on failure
int hwfq_get_tenant_stats(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                          hwfq_tenant_stats_t *stats_out);

// Get statistics for a flow
//
// scheduler - Scheduler handle
// tenant_id - Tenant owning the flow
// flow_id - Flow to query
// stats_out - Output parameter for statistics
// returns - 0 on success, negative error code on failure
int hwfq_get_flow_stats(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                        hwfq_flow_id_t flow_id, hwfq_flow_stats_t *stats_out);

// Reset statistics counters
//
// scheduler - Scheduler handle
// tenant_id - Tenant to reset (or special value for all)
// flow_id - Flow to reset (or special value for all)
void hwfq_reset_stats(hwfq_scheduler_t *scheduler, hwfq_tenant_id_t tenant_id,
                      hwfq_flow_id_t flow_id);

#ifdef __cplusplus
}
#endif

#endif // HWFQ_H
