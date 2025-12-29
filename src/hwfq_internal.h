#ifndef HWFQ_INTERNAL_H
#define HWFQ_INTERNAL_H

#include "hwfq.h"
#include "hwfq_chunked_entries.h"
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

typedef struct tenant_config_t tenant_config_t;
typedef struct hwfq_chunked_entries_t hwfq_chunked_entries_t;
typedef struct group_scheduler_t group_scheduler_t;
typedef struct session_state_t session_state_t;

typedef struct {
    uint64_t window_start_ns;
    uint64_t window_work_units;
} rate_tracking_t;

typedef struct flow_session_context flow_session_context_t;

typedef struct in_flight_entry {
    hwfq_tenant_id_t tenant_id;
    hwfq_flow_id_t flow_id;
    uint64_t work_size;
    uint64_t dequeue_time_ns;
    uint64_t timeout_ns;
    void *user_data;
    struct in_flight_entry *hash_next;
    struct in_flight_entry *list_next;
    struct in_flight_entry *list_prev;
} in_flight_entry_t;

struct tenant_config_t {
    hwfq_tenant_id_t tenant_id;
    hwfq_allocation_t allocation;
    bool configured;
    hwfq_chunked_entries_t flow_entries;
    group_scheduler_t *flow_scheduler;
    bool has_backlog;
    session_state_t *tenant_session;
    hwfq_tenant_stats_t stats;
    rate_tracking_t rate_tracking;
    pthread_mutex_t lock;
};

struct flow_session_context {
    hwfq_tenant_id_t tenant_id;
    hwfq_flow_id_t flow_id;
    void *user_data;
    uint64_t work_size;
    uint64_t enqueue_time_ns;
    uint64_t timeout_ns;
    hwfq_session_cleanup_fn cleanup_fn;
};

struct hwfq_scheduler_t {
    hwfq_config_t config;
    void *(*alloc_fn)(size_t);
    void (*free_fn)(void *);
    tenant_config_t **tenants;
    uint32_t num_configured_tenants;
    uint64_t allocated_rate_capacity;
    hwfq_chunked_entries_t tenant_entries;
    group_scheduler_t *system_scheduler;
    uint64_t in_flight_work_size;
    uint32_t in_flight_count;
    in_flight_entry_t **in_flight_hash;
    in_flight_entry_t *in_flight_head;
    in_flight_entry_t *in_flight_tail;
    pthread_mutex_t lock;
};

void *hwfq_alloc(hwfq_scheduler_t *scheduler, size_t size);
void hwfq_free(hwfq_scheduler_t *scheduler, void *ptr);

#endif // HWFQ_INTERNAL_H
