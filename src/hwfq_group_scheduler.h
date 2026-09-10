// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 AirMettle, Inc.

#ifndef HWFQ_GROUP_SCHEDULER_H
#define HWFQ_GROUP_SCHEDULER_H

#include "hwfq.h"
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// H-WFQ Group Scheduler - Single-Level Reusable WFQ Component
// ============================================================================
//
// This is a standalone, reusable WFQ scheduler component that implements the
// WF2Q+ algorithm with a calendar queue data structure. It is generic and
// can be instantiated multiple times to build hierarchical schedulers.
//
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Group scheduler instance (single-level WFQ scheduler)
typedef struct group_scheduler_t group_scheduler_t;

// Session state (represents a queued work item)
typedef struct session_state_t session_state_t;

// Generic entry ID (can represent tenant_id, flow_id, or any identifier)
typedef uint32_t group_entry_id_t;

// Entry configuration for group scheduler
typedef struct {
    group_entry_id_t entry_id;
    hwfq_allocation_t allocation; // Rate or weight-based allocation
} group_entry_config_t;

// Forward declaration for chunked entries
typedef struct hwfq_chunked_entries_t hwfq_chunked_entries_t;

// Initialize a group scheduler instance with chunked entries storage
//
// parent - Parent scheduler (for memory allocation context)
// num_groups - Number of service interval groups (typically 16)
// bins_per_group - Number of bins per group (typically 2048)
// total_capacity - Total capacity in work units/sec
// max_entries - Maximum entry ID + 1 (e.g., max_tenants for system scheduler,
//               max_flows_per_tenant for tenant scheduler)
// entries - Chunked entries storage (owned by caller, not freed on destroy)
//
// Returns pointer to group scheduler, or NULL on error
group_scheduler_t *group_scheduler_init_with_entries(hwfq_scheduler_t *parent, uint32_t num_groups,
                                                       uint32_t bins_per_group, uint64_t total_capacity,
                                                       uint32_t max_entries, hwfq_chunked_entries_t *entries);

// Destroy a group scheduler and free all resources
//
// parent - Parent scheduler (for memory deallocation)
// gs - Group scheduler to destroy
void group_scheduler_destroy(hwfq_scheduler_t *parent, group_scheduler_t *gs);

// Configure an entry in the group scheduler
//
// An entry represents a schedulable entity (could be a tenant, flow, etc.)
// Each entry has an allocation (rate or weight) that determines scheduling
//
// gs - Group scheduler instance
// config - Entry configuration
//
// Returns 0 on success, negative error code on failure
int group_scheduler_configure_entry(group_scheduler_t *gs, const group_entry_config_t *config);

// Remove an entry configuration
//
// gs - Group scheduler instance
// entry_id - Entry to remove
//
// Returns 0 on success, negative error code on failure
int group_scheduler_remove_entry(group_scheduler_t *gs, group_entry_id_t entry_id);

// Internal cleanup callback type for session user_data during destroy
// Called with parent scheduler for access to memory operations
typedef void (*group_session_cleanup_fn)(hwfq_scheduler_t *parent, void *user_data);

// Enqueue a session into the group scheduler
//
// Creates a new session for the specified entry and calculates its WF2Q+
// scheduling times (start time, service interval, finish time). The session
// is inserted into the calendar queue for scheduling.
//
// gs - Group scheduler instance
// entry_id - Which entry this session belongs to
// work_size - Size of work (in work units, e.g., bytes, operations)
// user_data - User data pointer (stored in session, returned on dequeue)
// cleanup_fn - Optional callback to cleanup user_data if session destroyed without dequeue
// session_out - Output parameter for created session (optional)
//
// Returns 0 on success, negative error code on failure
int group_scheduler_enqueue(group_scheduler_t *gs, group_entry_id_t entry_id, uint64_t work_size,
                            void *user_data, group_session_cleanup_fn cleanup_fn,
                            session_state_t **session_out);

// Dequeue the next session according to WF2Q+ policy
//
// Selects the session with the earliest finish time among eligible sessions
// (those with start time <= virtual time). Removes the session from the
// calendar queue.
//
// MEMORY OWNERSHIP: The caller owns the returned session and MUST free it
// using free() when done. The scheduler relinquishes ownership at dequeue.
//
// Complexity: Amortized O(1) for typical workloads. Worst-case O(n) when
// sessions span many calendar bins (requires scanning to find true minimum).
//
// gs - Group scheduler instance
//
// Returns pointer to session (caller must free), or NULL if no work available
session_state_t *group_scheduler_dequeue(group_scheduler_t *gs);

// Remove a specific session from the scheduler
//
// MEMORY OWNERSHIP: The scheduler frees the session internally. Do NOT access
// or free the session pointer after calling this function.
//
// This differs from group_scheduler_dequeue() which returns ownership to caller.
// Use this function when you want to cancel/abort a session without processing.
//
// gs - Group scheduler instance
// session - Session to remove (will be freed internally)
//
// Returns 0 on success, negative error code on failure
int group_scheduler_remove_session(group_scheduler_t *gs, session_state_t *session);

// Get current virtual time
//
// Virtual time represents the progress of fair service delivery according
// to the WF2Q+ algorithm. Note: Returned value is scaled internally for
// precision (scaled by 2^20). Use for comparison, not direct interpretation.
//
// gs - Group scheduler instance
//
// Returns current virtual time value
uint64_t group_scheduler_get_virtual_time(group_scheduler_t *gs);

// Update virtual time after work completion
//
// Updates virtual time according to the WF2Q+ formula:
// V_WF2Q+(t + Δt) = max(V_WF2Q+(t) + Δt, min{S_i})
//
// gs - Group scheduler instance
// work_completed - Amount of work completed (in work units)
void group_scheduler_update_virtual_time(group_scheduler_t *gs, uint64_t work_completed);

// Get entry ID for a session
group_entry_id_t session_get_entry_id(const session_state_t *session);

// Get user data from a session
void *session_get_user_data(const session_state_t *session);

// Get work size from a session
uint64_t session_get_work_size(const session_state_t *session);

// Get finish time from a session
uint64_t session_get_finish_time(const session_state_t *session);

// Get number of active sessions in the scheduler
uint32_t group_scheduler_get_session_count(group_scheduler_t *gs);

// Check if scheduler is empty (no sessions)
bool group_scheduler_is_empty(group_scheduler_t *gs);

// Get calculated effective rate for an entry
//
// For rate-based entries, returns the configured rate.
// For weight-based entries, returns the calculated proportional rate.
//
// gs - Group scheduler instance
// entry_id - Entry to query
// rate_out - Output parameter for rate
//
// Returns 0 on success, negative error code on failure
int group_scheduler_get_entry_rate(group_scheduler_t *gs, group_entry_id_t entry_id,
                                   uint64_t *rate_out);

#ifdef __cplusplus
}
#endif

#endif // HWFQ_GROUP_SCHEDULER_H
