#include "hwfq.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * Basic H-WFQ Configuration Example
 *
 * This example demonstrates:
 * - Initializing a scheduler
 * - Configuring tenants with different allocation types
 * - Querying capacity information
 * - Cleaning up resources
 */

int main(void)
{
    printf("=== H-WFQ Basic Configuration Example ===\n\n");

    // Step 1: Initialize the scheduler
    printf("1. Initializing scheduler...\n");

    hwfq_config_t config = {.max_tenants = 1000,
                            .max_flows_per_tenant = 10000,
                            .num_groups = 16,
                            .bins_per_group = 2048,
                            .total_capacity = 10000000000ULL, // 10 GB/sec total capacity
                            .enable_statistics = true,
                            .alloc_fn = NULL, // Use default malloc
                            .free_fn = NULL,  // Use default free
                            .session_available_fn = NULL};

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);

    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to initialize scheduler: %d\n", ret);
        return 1;
    }

    printf("   Scheduler initialized successfully!\n");
    printf("   Max tenants: %u\n", config.max_tenants);
    printf("   Total capacity: %llu bytes/sec\n\n", (unsigned long long)config.total_capacity);

    // Step 2: Add tenants with different allocation types
    printf("2. Adding tenants...\n");

    // Tenant 0: Premium tier - guaranteed 5 GB/sec (rate-based)
    hwfq_allocation_t premium_alloc = {
        .allocation_type = HWFQ_ALLOCATION_RATE,
        .rate = 5000000000ULL // 5 GB/sec guaranteed
    };

    hwfq_tenant_id_t premium_id;
    ret = hwfq_add_tenant(scheduler, &premium_alloc, &premium_id);
    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to add premium tenant: %d\n", ret);
        hwfq_destroy(scheduler);
        return 1;
    }
    printf("   Tenant %u (Premium): 5 GB/sec guaranteed (rate-based)\n", premium_id);

    // Tenant 1: Standard tier - weight 70 (weight-based)
    hwfq_allocation_t standard_alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 70};

    hwfq_tenant_id_t standard_id;
    ret = hwfq_add_tenant(scheduler, &standard_alloc, &standard_id);
    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to add standard tenant: %d\n", ret);
        hwfq_destroy(scheduler);
        return 1;
    }
    printf("   Tenant %u (Standard): Weight 70 (proportional)\n", standard_id);

    // Tenant 2: Basic tier - weight 30 (weight-based)
    hwfq_allocation_t basic_alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 30};

    hwfq_tenant_id_t basic_id;
    ret = hwfq_add_tenant(scheduler, &basic_alloc, &basic_id);
    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to add basic tenant: %d\n", ret);
        hwfq_destroy(scheduler);
        return 1;
    }
    printf("   Tenant %u (Basic): Weight 30 (proportional)\n\n", basic_id);

    // Step 3: Query capacity information
    printf("3. Querying capacity information...\n");

    hwfq_capacity_info_t capacity_info;
    ret = hwfq_get_capacity_info(scheduler, &capacity_info);

    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to get capacity info: %d\n", ret);
        hwfq_destroy(scheduler);
        return 1;
    }

    printf("   Total capacity: %llu bytes/sec\n", (unsigned long long)capacity_info.total_capacity);
    printf("   Allocated capacity: %llu bytes/sec (rate-based only)\n",
           (unsigned long long)capacity_info.allocated_capacity);
    printf("   Available capacity: %llu bytes/sec\n",
           (unsigned long long)capacity_info.available_capacity);
    printf("   Configured tenants: %u\n", capacity_info.num_configured_tenants);
    printf("   Utilization: %.1f%%\n\n", capacity_info.utilization_percent);

    // Step 4: Configure flows within tenants
    printf("4. Configuring flows within tenants...\n");

    // Configure flow 100 for tenant 1 with weight 80
    hwfq_allocation_t flow_alloc_1 = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 80};

    ret = hwfq_configure_flow(scheduler, standard_id, 100, &flow_alloc_1);
    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to configure flow: %d\n", ret);
        hwfq_destroy(scheduler);
        return 1;
    }
    printf("   Tenant %u, Flow 100: Weight 80\n", standard_id);

    // Configure flow 200 for standard tenant with weight 20
    hwfq_allocation_t flow_alloc_2 = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 20};

    ret = hwfq_configure_flow(scheduler, standard_id, 200, &flow_alloc_2);
    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to configure flow: %d\n", ret);
        hwfq_destroy(scheduler);
        return 1;
    }
    printf("   Tenant %u, Flow 200: Weight 20\n\n", standard_id);

    // Step 5: Demonstrate overbooking prevention
    printf("5. Testing overbooking prevention...\n");

    // Try to allocate 6 GB/sec (would exceed available 5 GB/sec)
    hwfq_allocation_t overbook_alloc = {.allocation_type = HWFQ_ALLOCATION_RATE,
                                        .rate = 6000000000ULL};

    hwfq_tenant_id_t overbook_id;
    ret = hwfq_add_tenant(scheduler, &overbook_alloc, &overbook_id);
    if (ret == HWFQ_ERR_OVERBOOKED) {
        printf("   ✓ Correctly rejected overbooking (6 GB/sec > 5 GB/sec available)\n\n");
    } else {
        fprintf(stderr, "   ✗ Failed to detect overbooking!\n\n");
    }

    // Step 6: Reconfigure a tenant
    printf("6. Reconfiguring tenant...\n");

    // Reduce premium tenant from 5 GB/sec to 3 GB/sec
    premium_alloc.rate = 3000000000ULL;
    ret = hwfq_configure_tenant(scheduler, premium_id, &premium_alloc);

    if (ret != HWFQ_SUCCESS) {
        fprintf(stderr, "Failed to reconfigure tenant: %d\n", ret);
        hwfq_destroy(scheduler);
        return 1;
    }

    printf("   Tenant 0 (Premium): Updated to 3 GB/sec\n");

    // Check new capacity
    hwfq_get_capacity_info(scheduler, &capacity_info);
    printf("   New allocated capacity: %llu bytes/sec\n",
           (unsigned long long)capacity_info.allocated_capacity);
    printf("   New available capacity: %llu bytes/sec\n\n",
           (unsigned long long)capacity_info.available_capacity);

    // Step 7: Cleanup
    printf("7. Cleaning up...\n");
    hwfq_destroy(scheduler);
    printf("   Scheduler destroyed successfully!\n\n");

    printf("=== Example completed successfully ===\n");
    return 0;
}
