#include "hwfq.h"
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// ============================================================================
// Test Helper Functions
// ============================================================================

// Custom allocator for testing memory management
typedef struct {
    size_t alloc_count;
    size_t free_count;
    size_t bytes_allocated;
} allocator_stats_t;

static allocator_stats_t g_allocator_stats = {0, 0, 0};

static void *test_alloc(size_t size)
{
    g_allocator_stats.alloc_count++;
    g_allocator_stats.bytes_allocated += size;

    // Allocate extra space to store size for accounting
    size_t *ptr = (size_t *)malloc(sizeof(size_t) + size);
    if (ptr != NULL) {
        *ptr = size;
        return (void *)(ptr + 1);
    }
    return NULL;
}

static void test_free(void *ptr)
{
    if (ptr != NULL) {
        g_allocator_stats.free_count++;

        size_t *size_ptr = ((size_t *)ptr) - 1;
        g_allocator_stats.bytes_allocated -= *size_ptr;

        free(size_ptr);
    }
}

static void reset_allocator_stats(void)
{
    g_allocator_stats.alloc_count = 0;
    g_allocator_stats.free_count = 0;
    g_allocator_stats.bytes_allocated = 0;
}

// Test result tracking
static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: Basic scheduler initialization and destruction
void test_init_destroy(void)
{
    hwfq_scheduler_t *sched = NULL;

    hwfq_config_t config = {.max_tenants = 100,
                            .max_flows_per_tenant = 1000,
                            .num_groups = 16,
                            .bins_per_group = 2048,
                            .total_capacity = 1000000000, // 1 GB/sec
                            .enable_statistics = false,
                            .alloc_fn = NULL,
                            .free_fn = NULL,
                            .session_available_fn = NULL};

    int ret = hwfq_init(&config, &sched);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "hwfq_init should succeed");
    TEST_ASSERT(sched != NULL, "Scheduler should not be NULL");

    hwfq_destroy(sched);

    TEST_PASS();
}

// Test 2: Initialize with NULL parameters (error case)
void test_init_null_params(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100, .max_flows_per_tenant = 1000, .total_capacity = 1000000000};

    // NULL config
    int ret = hwfq_init(NULL, &sched);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Should reject NULL config");

    // NULL output parameter
    ret = hwfq_init(&config, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Should reject NULL output");

    TEST_PASS();
}

// Test 3: Custom allocator usage
void test_custom_allocator(void)
{
    reset_allocator_stats();

    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {.max_tenants = 10,
                            .max_flows_per_tenant = 100,
                            .total_capacity = 1000000000,
                            .enable_statistics = false,
                            .alloc_fn = test_alloc,
                            .free_fn = test_free,
                            .session_available_fn = NULL};

    int ret = hwfq_init(&config, &sched);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Init should succeed with custom allocator");
    TEST_ASSERT(g_allocator_stats.alloc_count > 0, "Custom allocator should be called");

    size_t allocs_before_destroy = g_allocator_stats.alloc_count;

    hwfq_destroy(sched);

    TEST_ASSERT(g_allocator_stats.free_count == allocs_before_destroy,
                "All allocations should be freed");
    TEST_ASSERT(g_allocator_stats.bytes_allocated == 0, "No memory leaks");

    TEST_PASS();
}

// Test 4: Configure tenant with weight-based allocation
void test_configure_tenant_weight(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100, .max_flows_per_tenant = 1000, .total_capacity = 1000000000};

    hwfq_init(&config, &sched);

    hwfq_allocation_t alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100};

    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(sched, &alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should add tenant with weight");
    TEST_ASSERT(tenant_id == 0, "First tenant should have ID 0");

    hwfq_capacity_info_t info;
    hwfq_get_capacity_info(sched, &info);
    TEST_ASSERT(info.num_configured_tenants == 1, "Should have 1 configured tenant");
    TEST_ASSERT(info.allocated_capacity == 0, "Weight-based shouldn't allocate capacity");

    hwfq_destroy(sched);
    TEST_PASS();
}

// Test 5: Configure tenant with rate-based allocation
void test_configure_tenant_rate(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .total_capacity = 1000000000 // 1 GB/sec
    };

    hwfq_init(&config, &sched);

    hwfq_allocation_t alloc = {
        .allocation_type = HWFQ_ALLOCATION_RATE,
        .rate = 500000000 // 500 MB/sec
    };

    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(sched, &alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should add tenant with rate");

    hwfq_capacity_info_t info;
    hwfq_get_capacity_info(sched, &info);
    TEST_ASSERT(info.num_configured_tenants == 1, "Should have 1 configured tenant");
    TEST_ASSERT(info.allocated_capacity == 500000000, "Should track allocated rate");
    TEST_ASSERT(info.available_capacity == 500000000, "Should have 500 MB/sec available");
    TEST_ASSERT(info.utilization_percent == 50.0, "Should be 50% utilized");

    hwfq_destroy(sched);
    TEST_PASS();
}

// Test 6: Overbooking prevention
void test_overbooking_prevention(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100,
        .max_flows_per_tenant = 1000,
        .total_capacity = 1000000000 // 1 GB/sec
    };

    hwfq_init(&config, &sched);

    // Add first tenant with 700 MB/sec
    hwfq_allocation_t alloc1 = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 700000000};
    hwfq_tenant_id_t tenant_id1;
    int ret = hwfq_add_tenant(sched, &alloc1, &tenant_id1);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "First allocation should succeed");

    // Try to add second tenant with 400 MB/sec (would exceed 1 GB/sec)
    hwfq_allocation_t alloc2 = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 400000000};
    hwfq_tenant_id_t tenant_id2;
    ret = hwfq_add_tenant(sched, &alloc2, &tenant_id2);
    TEST_ASSERT(ret == HWFQ_ERR_OVERBOOKED, "Should reject overbooking");

    // Add second tenant with 300 MB/sec (exactly fits)
    alloc2.rate = 300000000;
    ret = hwfq_add_tenant(sched, &alloc2, &tenant_id2);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should accept allocation that fits");

    hwfq_capacity_info_t info;
    hwfq_get_capacity_info(sched, &info);
    TEST_ASSERT(info.allocated_capacity == 1000000000, "Should be fully allocated");
    TEST_ASSERT(info.available_capacity == 0, "Should have no capacity remaining");

    hwfq_destroy(sched);
    TEST_PASS();
}

// Test 7: Reconfigure tenant (change allocation)
void test_reconfigure_tenant(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100, .max_flows_per_tenant = 1000, .total_capacity = 1000000000};

    hwfq_init(&config, &sched);

    // Add tenant with 500 MB/sec
    hwfq_allocation_t alloc = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 500000000};
    hwfq_tenant_id_t tenant_id;
    hwfq_add_tenant(sched, &alloc, &tenant_id);

    // Reconfigure with 300 MB/sec
    alloc.rate = 300000000;
    int ret = hwfq_configure_tenant(sched, tenant_id, &alloc);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should allow reconfiguration");

    hwfq_capacity_info_t info;
    hwfq_get_capacity_info(sched, &info);
    TEST_ASSERT(info.allocated_capacity == 300000000, "Should reflect new allocation");
    TEST_ASSERT(info.num_configured_tenants == 1, "Should still have 1 tenant");

    hwfq_destroy(sched);
    TEST_PASS();
}

// Test 8: Remove tenant
void test_remove_tenant(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100, .max_flows_per_tenant = 1000, .total_capacity = 1000000000};

    hwfq_init(&config, &sched);

    // Add tenant
    hwfq_allocation_t alloc = {.allocation_type = HWFQ_ALLOCATION_RATE, .rate = 500000000};
    hwfq_tenant_id_t tenant_id;
    hwfq_add_tenant(sched, &alloc, &tenant_id);

    // Remove tenant
    int ret = hwfq_remove_tenant(sched, tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should remove tenant successfully");

    hwfq_capacity_info_t info;
    hwfq_get_capacity_info(sched, &info);
    TEST_ASSERT(info.num_configured_tenants == 0, "Should have 0 tenants");
    TEST_ASSERT(info.allocated_capacity == 0, "Capacity should be freed");

    // Try to remove non-existent tenant
    ret = hwfq_remove_tenant(sched, tenant_id);
    TEST_ASSERT(ret == HWFQ_ERR_NOT_FOUND, "Should return NOT_FOUND for removed tenant");

    hwfq_destroy(sched);
    TEST_PASS();
}

// Test 9: Configure flow
void test_configure_flow(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100, .max_flows_per_tenant = 1000, .total_capacity = 1000000000};

    hwfq_init(&config, &sched);

    // Add tenant
    hwfq_allocation_t tenant_alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100};
    hwfq_tenant_id_t tenant_id;
    hwfq_add_tenant(sched, &tenant_alloc, &tenant_id);

    // Configure flow
    hwfq_allocation_t flow_alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 50};
    int ret = hwfq_configure_flow(sched, tenant_id, 1, &flow_alloc);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should configure flow successfully");

    hwfq_destroy(sched);
    TEST_PASS();
}

// Test 10: Remove flow
void test_remove_flow(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100, .max_flows_per_tenant = 1000, .total_capacity = 1000000000};

    hwfq_init(&config, &sched);

    // Add tenant
    hwfq_allocation_t tenant_alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100};
    hwfq_tenant_id_t tenant_id;
    hwfq_add_tenant(sched, &tenant_alloc, &tenant_id);

    // Configure flow
    hwfq_allocation_t flow_alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 50};
    hwfq_configure_flow(sched, tenant_id, 1, &flow_alloc);

    // Remove flow
    int ret = hwfq_remove_flow(sched, tenant_id, 1);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Should remove flow successfully");

    // Try to remove non-existent flow
    ret = hwfq_remove_flow(sched, tenant_id, 1);
    TEST_ASSERT(ret == HWFQ_ERR_NOT_FOUND, "Should return NOT_FOUND for removed flow");

    hwfq_destroy(sched);
    TEST_PASS();
}

// Test 11: Multiple tenants
void test_multiple_tenants(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100, .max_flows_per_tenant = 1000, .total_capacity = 1000000000};

    hwfq_init(&config, &sched);

    // Add 5 tenants with different allocations
    for (uint32_t i = 0; i < 5; i++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_RATE,
            .rate = 100000000 // 100 MB/sec each
        };
        hwfq_tenant_id_t tenant_id;
        int ret = hwfq_add_tenant(sched, &alloc, &tenant_id);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Should add all tenants");
        TEST_ASSERT(tenant_id == i, "Tenant IDs should be sequential");
    }

    hwfq_capacity_info_t info;
    hwfq_get_capacity_info(sched, &info);
    TEST_ASSERT(info.num_configured_tenants == 5, "Should have 5 tenants");
    TEST_ASSERT(info.allocated_capacity == 500000000, "Should have 500 MB/sec allocated");

    hwfq_destroy(sched);
    TEST_PASS();
}

// Test 12: Invalid parameters
void test_invalid_parameters(void)
{
    hwfq_scheduler_t *sched = NULL;
    hwfq_config_t config = {
        .max_tenants = 100, .max_flows_per_tenant = 1000, .total_capacity = 1000000000};

    hwfq_init(&config, &sched);

    // Invalid tenant ID for add_tenant
    hwfq_allocation_t alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100};

    // NULL allocation for add_tenant
    hwfq_tenant_id_t tenant_id;
    int ret = hwfq_add_tenant(sched, NULL, &tenant_id);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Should reject NULL allocation");

    // NULL tenant_id_out for add_tenant
    ret = hwfq_add_tenant(sched, &alloc, NULL);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Should reject NULL tenant_id_out");

    // Zero weight
    alloc.allocation_type = HWFQ_ALLOCATION_WEIGHT;
    alloc.weight = 0;
    ret = hwfq_add_tenant(sched, &alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Should reject zero weight");

    // Zero rate
    alloc.allocation_type = HWFQ_ALLOCATION_RATE;
    alloc.rate = 0;
    ret = hwfq_add_tenant(sched, &alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Should reject zero rate");

    // Configure non-existent tenant
    alloc.weight = 100;
    alloc.allocation_type = HWFQ_ALLOCATION_WEIGHT;
    ret = hwfq_configure_tenant(sched, 0, &alloc);
    TEST_ASSERT(ret == HWFQ_ERR_NOT_FOUND, "Should reject configuring non-existent tenant");

    // Invalid tenant ID for configure_tenant
    ret = hwfq_configure_tenant(sched, 999, &alloc);
    TEST_ASSERT(ret == HWFQ_ERR_INVALID_ARG, "Should reject invalid tenant ID");

    hwfq_destroy(sched);
    TEST_PASS();
}

// ============================================================================
// Test Runner
// ============================================================================

int main(void)
{
    printf("=== H-WFQ Configuration API Tests ===\n\n");

    // Run all tests
    test_init_destroy();
    test_init_null_params();
    test_custom_allocator();
    test_configure_tenant_weight();
    test_configure_tenant_rate();
    test_overbooking_prevention();
    test_reconfigure_tenant();
    test_remove_tenant();
    test_configure_flow();
    test_remove_flow();
    test_multiple_tenants();
    test_invalid_parameters();

    // Print summary
    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    if (g_tests_failed == 0) {
        printf("\nAll tests passed!\n");
        return 0;
    } else {
        printf("\nSome tests failed.\n");
        return 1;
    }
}
