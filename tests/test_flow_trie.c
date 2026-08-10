// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 AirMettle, Inc.

// ============================================================================
// Flow Trie Unit Tests
// ============================================================================
//
// Tests for the hierarchical trie structure used for O(1) flow ID allocation.
//
// ============================================================================

#include "test_common.h"
#include "../src/hwfq_flow_trie.h"
#include "../src/hwfq_internal.h"

// ============================================================================
// Test 1: Basic Lifecycle
// ============================================================================

static void test_trie_init_destroy(void) {
    printf("  test_trie_init_destroy...");

    hwfq_flow_trie_t trie;

    int ret = hwfq_flow_trie_init(&trie, 1000, NULL, NULL);
    (void)ret;
    assert(ret == 0);

    assert(hwfq_flow_trie_capacity(&trie) == 1000);
    assert(hwfq_flow_trie_allocated_count(&trie) == 1);
    assert(hwfq_flow_trie_free_count(&trie) == 999);

    hwfq_flow_trie_destroy(&trie, NULL);

    printf(" PASSED\n");
}

// ============================================================================
// Test 2: Sequential Allocation
// ============================================================================

static void test_trie_alloc_sequential(void) {
    printf("  test_trie_alloc_sequential...");

    hwfq_flow_trie_t trie;
    int ret = hwfq_flow_trie_init(&trie, 1000, NULL, NULL);
    (void)ret;
    assert(ret == 0);

    uint32_t flow_ids[100];
    for (int i = 0; i < 100; i++) {
        ret = hwfq_flow_trie_alloc(&trie, &flow_ids[i]);
        assert(ret == 0);
        assert(flow_ids[i] > 0);
    }

    assert(hwfq_flow_trie_allocated_count(&trie) == 101);

    for (int i = 0; i < 100; i++) {
        assert(hwfq_flow_trie_is_allocated(&trie, flow_ids[i]));
    }

    hwfq_flow_trie_destroy(&trie, NULL);

    printf(" PASSED\n");
}

// ============================================================================
// Test 3: Alloc/Free Cycle
// ============================================================================

static void test_trie_alloc_free_cycle(void) {
    printf("  test_trie_alloc_free_cycle...");

    hwfq_flow_trie_t trie;
    int ret = hwfq_flow_trie_init(&trie, 1000, NULL, NULL);
    (void)ret;
    assert(ret == 0);

    uint32_t flow_id1, flow_id2;

    ret = hwfq_flow_trie_alloc(&trie, &flow_id1);
    assert(ret == 0);
    assert(hwfq_flow_trie_is_allocated(&trie, flow_id1));

    hwfq_flow_trie_free(&trie, flow_id1);
    assert(!hwfq_flow_trie_is_allocated(&trie, flow_id1));

    ret = hwfq_flow_trie_alloc(&trie, &flow_id2);
    assert(ret == 0);

    uint32_t alloc_count = hwfq_flow_trie_allocated_count(&trie);
    (void)alloc_count;
    assert(alloc_count == 2);

    hwfq_flow_trie_destroy(&trie, NULL);

    printf(" PASSED\n");
}

// ============================================================================
// Test 4: Exhaustion
// ============================================================================

static void test_trie_exhaustion(void) {
    printf("  test_trie_exhaustion...");

    hwfq_flow_trie_t trie;
    uint32_t max_flows = 100;
    int ret = hwfq_flow_trie_init(&trie, max_flows, NULL, NULL);
    (void)ret;
    assert(ret == 0);

    uint32_t flow_id;
    for (uint32_t i = 0; i < max_flows - 1; i++) {
        ret = hwfq_flow_trie_alloc(&trie, &flow_id);
        assert(ret == 0);
    }

    assert(hwfq_flow_trie_allocated_count(&trie) == max_flows);
    assert(hwfq_flow_trie_free_count(&trie) == 0);

    ret = hwfq_flow_trie_alloc(&trie, &flow_id);
    assert(ret != 0);

    hwfq_flow_trie_destroy(&trie, NULL);

    printf(" PASSED\n");
}

// ============================================================================
// Test 5: Reserved ID Zero
// ============================================================================

static void test_trie_reserved_id_zero(void) {
    printf("  test_trie_reserved_id_zero...");

    hwfq_flow_trie_t trie;
    int ret = hwfq_flow_trie_init(&trie, 1000, NULL, NULL);
    (void)ret;
    assert(ret == 0);

    assert(hwfq_flow_trie_is_allocated(&trie, 0));

    uint32_t flow_id;
    for (int i = 0; i < 100; i++) {
        ret = hwfq_flow_trie_alloc(&trie, &flow_id);
        assert(ret == 0);
        assert(flow_id != 0);
    }

    hwfq_flow_trie_free(&trie, 0);
    assert(hwfq_flow_trie_is_allocated(&trie, 0));

    hwfq_flow_trie_destroy(&trie, NULL);

    printf(" PASSED\n");
}

// ============================================================================
// Test 6: Count Accuracy
// ============================================================================

static void test_trie_count_accuracy(void) {
    printf("  test_trie_count_accuracy...");

    hwfq_flow_trie_t trie;
    int ret = hwfq_flow_trie_init(&trie, 500, NULL, NULL);
    (void)ret;
    assert(ret == 0);

    assert(hwfq_flow_trie_allocated_count(&trie) == 1);
    assert(hwfq_flow_trie_free_count(&trie) == 499);

    uint32_t flow_ids[50];
    for (int i = 0; i < 50; i++) {
        ret = hwfq_flow_trie_alloc(&trie, &flow_ids[i]);
        assert(ret == 0);
    }

    assert(hwfq_flow_trie_allocated_count(&trie) == 51);
    assert(hwfq_flow_trie_free_count(&trie) == 449);

    for (int i = 0; i < 25; i++) {
        hwfq_flow_trie_free(&trie, flow_ids[i]);
    }

    assert(hwfq_flow_trie_allocated_count(&trie) == 26);
    assert(hwfq_flow_trie_free_count(&trie) == 474);

    hwfq_flow_trie_destroy(&trie, NULL);

    printf(" PASSED\n");
}

// ============================================================================
// Test 7: Is Allocated Check
// ============================================================================

static void test_trie_is_allocated(void) {
    printf("  test_trie_is_allocated...");

    hwfq_flow_trie_t trie;
    int ret = hwfq_flow_trie_init(&trie, 1000, NULL, NULL);
    (void)ret;
    assert(ret == 0);

    assert(!hwfq_flow_trie_is_allocated(&trie, 1));
    assert(!hwfq_flow_trie_is_allocated(&trie, 999));

    assert(!hwfq_flow_trie_is_allocated(&trie, 1000));
    assert(!hwfq_flow_trie_is_allocated(&trie, 5000));

    uint32_t flow_id;
    ret = hwfq_flow_trie_alloc(&trie, &flow_id);
    assert(ret == 0);
    assert(hwfq_flow_trie_is_allocated(&trie, flow_id));

    hwfq_flow_trie_free(&trie, flow_id);
    assert(!hwfq_flow_trie_is_allocated(&trie, flow_id));

    hwfq_flow_trie_destroy(&trie, NULL);

    printf(" PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("Flow Trie Tests\n");
    printf("===============\n\n");

    test_trie_init_destroy();
    test_trie_alloc_sequential();
    test_trie_alloc_free_cycle();
    test_trie_exhaustion();
    test_trie_reserved_id_zero();
    test_trie_count_accuracy();
    test_trie_is_allocated();

    printf("\nAll flow trie tests passed!\n");
    return 0;
}
