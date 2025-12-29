#include "hwfq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Custom Allocator Example
 *
 * This example demonstrates:
 * - Using custom memory allocators
 * - Tracking memory usage
 * - Detecting memory leaks
 */

// ============================================================================
// Custom Allocator Implementation with Tracking
// ============================================================================

typedef struct {
    size_t total_allocated;
    size_t total_freed;
    size_t current_usage;
    size_t peak_usage;
    size_t alloc_count;
    size_t free_count;
} memory_tracker_t;

static memory_tracker_t g_memory_tracker = {0};

// Custom allocator that tracks memory usage
static void *tracked_alloc(size_t size)
{
    // Allocate extra space to store the size
    size_t *ptr = (size_t *)malloc(sizeof(size_t) + size);
    if (ptr == NULL) {
        return NULL;
    }

    // Store size at the beginning
    *ptr = size;

    // Update tracking statistics
    g_memory_tracker.total_allocated += size;
    g_memory_tracker.current_usage += size;
    g_memory_tracker.alloc_count++;

    if (g_memory_tracker.current_usage > g_memory_tracker.peak_usage) {
        g_memory_tracker.peak_usage = g_memory_tracker.current_usage;
    }

    printf("   [ALLOC] %zu bytes (total: %zu bytes)\n", size, g_memory_tracker.current_usage);

    // Return pointer after the size field
    return (void *)(ptr + 1);
}

// Custom deallocator that tracks memory usage
static void tracked_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    // Get the size from before the pointer
    size_t *size_ptr = ((size_t *)ptr) - 1;
    size_t size = *size_ptr;

    // Update tracking statistics
    g_memory_tracker.total_freed += size;
    g_memory_tracker.current_usage -= size;
    g_memory_tracker.free_count++;

    printf("   [FREE]  %zu bytes (total: %zu bytes)\n", size, g_memory_tracker.current_usage);

    // Free the actual allocation
    free(size_ptr);
}

// Reset tracking statistics
static void reset_memory_tracker(void)
{
    memset(&g_memory_tracker, 0, sizeof(memory_tracker_t));
}

// Print memory statistics
static void print_memory_stats(void)
{
    printf("\n=== Memory Statistics ===\n");
    printf("Total allocated:  %zu bytes\n", g_memory_tracker.total_allocated);
    printf("Total freed:      %zu bytes\n", g_memory_tracker.total_freed);
    printf("Current usage:    %zu bytes\n", g_memory_tracker.current_usage);
    printf("Peak usage:       %zu bytes\n", g_memory_tracker.peak_usage);
    printf("Alloc calls:      %zu\n", g_memory_tracker.alloc_count);
    printf("Free calls:       %zu\n", g_memory_tracker.free_count);

    if (g_memory_tracker.current_usage > 0) {
        printf("\n⚠️  WARNING: Memory leak detected! %zu bytes not freed\n",
               g_memory_tracker.current_usage);
    } else {
        printf("\n✓ No memory leaks detected\n");
    }
    printf("\n");
}

// ============================================================================
// Main Example
// ============================================================================

int main(void)
{
    printf("=== H-WFQ Custom Allocator Example ===\n\n");

    reset_memory_tracker();

    // Step 1: Initialize scheduler with custom allocator
    printf("1. Initializing scheduler with custom allocator...\n");

    hwfq_config_t config = {.max_tenants = 100,
                            .max_flows_per_tenant = 1000,
                            .num_groups = 16,
                            .bins_per_group = 2048,
                            .total_capacity = 10000000000ULL,
                            .enable_statistics = true,
                            .alloc_fn = tracked_alloc, // Use custom allocator
                            .free_fn = tracked_free,   // Use custom deallocator
                            .session_available_fn = NULL};

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);

    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to initialize scheduler: %d\n", ret);
        return 1;
    }

    printf("   Scheduler initialized!\n");
    printf("   Initial memory usage: %zu bytes\n\n", g_memory_tracker.current_usage);

    // Step 2: Add some tenants
    printf("2. Adding tenants...\n");

    hwfq_tenant_id_t tenant_ids[5];
    for (uint32_t i = 0; i < 5; i++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_RATE,
            .rate = 1000000000ULL // 1 GB/sec each
        };

        ret = hwfq_add_tenant(scheduler, &alloc, &tenant_ids[i]);
        if (ret != HWFQ_SUCCESS) {
            fprintf(stderr, "Failed to add tenant %u: %d\n", i, ret);
            hwfq_destroy(scheduler);
            return 1;
        }
    }

    printf("   Added 5 tenants\n");
    printf("   Current memory usage: %zu bytes\n\n", g_memory_tracker.current_usage);

    // Step 3: Add flows within tenants
    printf("3. Adding flows...\n");

    for (uint32_t tenant = 0; tenant < 5; tenant++) {
        for (uint32_t flow = 0; flow < 3; flow++) {
            hwfq_allocation_t alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100};
            hwfq_flow_id_t flow_id;

            ret = hwfq_add_flow(scheduler, tenant_ids[tenant], &alloc, &flow_id);
            if (ret != HWFQ_SUCCESS) {
                fprintf(stderr, "Failed to add flow: %d\n", ret);
                hwfq_destroy(scheduler);
                return 1;
            }
        }
    }

    printf("   Added 15 flows (3 per tenant)\n");
    printf("   Peak memory usage: %zu bytes\n\n", g_memory_tracker.peak_usage);

    // Step 4: Query capacity
    printf("4. Querying capacity...\n");

    hwfq_capacity_info_t capacity_info;
    ret = hwfq_get_capacity_info(scheduler, &capacity_info);

    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to get capacity info: %d\n", ret);
        hwfq_destroy(scheduler);
        return 1;
    }

    printf("   Configured tenants: %u\n", capacity_info.num_configured_tenants);
    printf("   Allocated capacity: %llu bytes/sec\n",
           (unsigned long long)capacity_info.allocated_capacity);
    printf("   Utilization: %.1f%%\n\n", capacity_info.utilization_percent);

    // Step 5: Remove some flows and tenants
    printf("5. Removing flows and tenants...\n");

    // Remove flows from first tenant
    for (uint32_t flow = 0; flow < 3; flow++) {
        hwfq_remove_flow(scheduler, tenant_ids[0], flow);
    }
    printf("   Removed 3 flows from tenant %u\n", tenant_ids[0]);

    // Remove first tenant
    hwfq_remove_tenant(scheduler, tenant_ids[0]);
    printf("   Removed tenant %u\n", tenant_ids[0]);

    printf("   Current memory usage: %zu bytes\n\n", g_memory_tracker.current_usage);

    // Step 6: Cleanup
    printf("6. Destroying scheduler...\n");
    hwfq_destroy(scheduler);
    printf("   Scheduler destroyed\n\n");

    // Step 7: Print final memory statistics
    print_memory_stats();

    printf("=== Example completed successfully ===\n");

    // Return error code if memory leak detected
    return (g_memory_tracker.current_usage > 0) ? 1 : 0;
}
