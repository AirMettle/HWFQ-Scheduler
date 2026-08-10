// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 AirMettle, Inc.

// ============================================================================
// Hierarchical Fairness at Scale Tests for H-WFQ Scheduler
// ============================================================================
//
// Validates weighted fairness through the full two-level hierarchical scheduler
// (public API only), addressing gaps in the existing test suite:
//
// 1. 128 tenants with skewed weights (8 weight tiers, geometric 2x spacing)
// 2. Intra-tenant flow fairness with 10 skewed flows through the public API
// 3. Cross-tenant fairness when tenants have different numbers of active flows
// 4. Full two-level skew: skewed tenant weights AND skewed flow weights
// 5. 128 tenants x 4 flows each with varied weights
//
// Scheduling pattern: pre-fill a backlog for all tenants/flows, then repeatedly
// dequeue one item and re-enqueue for the same tenant/flow to maintain constant
// backlog. This forces the scheduler to make weighted choices every dequeue.
//
// ============================================================================

#include "test_common.h"
#include "../include/hwfq.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Global test counters
static int g_tests_passed = 0;
static int g_tests_failed = 0;

// ============================================================================
// Constants
// ============================================================================

#define WORK_SIZE 4096
#define PREFILL 3  // Items per tenant/flow to pre-fill (keeps all backlogged)

// ============================================================================
// Helpers
// ============================================================================

static bool is_within_tolerance(double actual, double expected, double tol_pct) {
    if (expected == 0.0) return actual == 0.0;
    return fabs((actual - expected) / expected) * 100.0 <= tol_pct;
}

static double deviation_pct(double actual, double expected) {
    if (expected == 0.0) return (actual == 0.0) ? 0.0 : 100.0;
    return ((actual - expected) / expected) * 100.0;
}

static double adaptive_tolerance(uint32_t weight, uint32_t max_weight) {
    double ratio = (double)weight / (double)max_weight;
    if (ratio >= 0.25) return 10.0;
    if (ratio >= 0.0625) return 20.0;
    if (ratio >= 0.015) return 40.0;
    return 80.0;
}

// ============================================================================
// Test 1: 128 Tenants with Geometric Weight Tiers
// ============================================================================
//
// 8 weight tiers of 16 tenants each: 512, 256, 128, 64, 32, 16, 8, 4
// Each tenant has 1 flow. Pre-fill backlog, then dequeue+re-enqueue loop.

#define T1_TENANTS 128
#define T1_TIERS 8
#define T1_PER_TIER 16
#define T1_ROUNDS 20000

static const uint32_t TIER_WEIGHTS[T1_TIERS] = {512, 256, 128, 64, 32, 16, 8, 4};

void test_many_tenants_geometric_weights(void) {
    hwfq_config_t config = {
        .max_tenants = 256,
        .max_flows_per_tenant = 100,
        .max_total_flows = 50000,
        .total_capacity = 1000000000ULL,  // 1 GB/s
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_tenant_id_t tenant_ids[T1_TENANTS];
    hwfq_flow_id_t flow_ids[T1_TENANTS];

    uint32_t total_weight = 0;
    for (int i = 0; i < T1_TENANTS; i++) {
        uint32_t tier = i / T1_PER_TIER;
        uint32_t w = TIER_WEIGHTS[tier];
        total_weight += w;

        hwfq_allocation_t alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = w};
        ret = hwfq_add_tenant(scheduler, &alloc, &tenant_ids[i]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

        flow_ids[i] = test_add_flow(scheduler, tenant_ids[i]);
        TEST_ASSERT(flow_ids[i] != 0, "Failed to add flow");
    }

    // Pre-fill backlog
    for (int i = 0; i < T1_TENANTS; i++) {
        for (int p = 0; p < PREFILL; p++) {
            hwfq_session_t work = {.user_data = NULL, .work_size = WORK_SIZE};
            hwfq_enqueue(scheduler, tenant_ids[i], flow_ids[i], &work);
        }
    }

    printf("    128 tenants, 8 weight tiers (512..4), total_weight=%u\n", total_weight);

    uint64_t counts[T1_TENANTS] = {0};
    uint64_t total_dequeued = 0;

    for (int round = 0; round < T1_ROUNDS; round++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        if (ret != HWFQ_SUCCESS) break;

        // Count and re-enqueue for same tenant to maintain backlog
        for (int i = 0; i < T1_TENANTS; i++) {
            if (tenant_ids[i] == tid_out) {
                counts[i]++;
                hwfq_session_t refill = {.user_data = NULL, .work_size = WORK_SIZE};
                hwfq_enqueue(scheduler, tenant_ids[i], flow_ids[i], &refill);
                break;
            }
        }
        total_dequeued++;
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    printf("    Results by weight tier (%llu dequeued):\n", (unsigned long long)total_dequeued);

    bool all_ok = true;
    for (int tier = 0; tier < T1_TIERS; tier++) {
        uint64_t tier_count = 0;
        for (int j = 0; j < T1_PER_TIER; j++) {
            tier_count += counts[tier * T1_PER_TIER + j];
        }
        double expected = (double)(TIER_WEIGHTS[tier] * T1_PER_TIER) / total_weight;
        double actual = (double)tier_count / total_dequeued;
        double dev = deviation_pct(actual, expected);
        double tol = adaptive_tolerance(TIER_WEIGHTS[tier], TIER_WEIGHTS[0]);
        bool ok = is_within_tolerance(actual, expected, tol);
        if (!ok) all_ok = false;

        printf("      Tier %d (w=%3u, x%d): expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
               tier, TIER_WEIGHTS[tier], T1_PER_TIER, expected, actual, dev,
               ok ? "OK" : "FAIL");
    }

    printf("    Within-tier uniformity:\n");
    for (int tier = 0; tier < T1_TIERS; tier++) {
        uint64_t tier_min = UINT64_MAX, tier_max = 0;
        for (int j = 0; j < T1_PER_TIER; j++) {
            uint64_t c = counts[tier * T1_PER_TIER + j];
            if (c < tier_min) tier_min = c;
            if (c > tier_max) tier_max = c;
        }
        double spread = (tier_min > 0) ? (double)tier_max / tier_min : 0;
        printf("      Tier %d (w=%3u): min=%llu, max=%llu, spread=%.2fx\n",
               tier, TIER_WEIGHTS[tier],
               (unsigned long long)tier_min, (unsigned long long)tier_max, spread);
    }

    TEST_ASSERT(all_ok, "Per-tier allocation ratios exceed tolerance");
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test 2: Intra-Tenant Flow Fairness (10 Skewed Flows, Full Hierarchy)
// ============================================================================

#define T2_FLOWS 10
#define T2_ROUNDS 10000

static const uint32_t FLOW_WEIGHTS[T2_FLOWS] = {512, 256, 128, 64, 32, 16, 8, 4, 2, 1};

void test_intra_tenant_flow_fairness(void) {
    hwfq_config_t config = {
        .max_tenants = 10,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 1000000000ULL,  // 1 GB/s
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_allocation_t tenant_alloc = {
        .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100
    };
    hwfq_tenant_id_t tenant_id;
    ret = hwfq_add_tenant(scheduler, &tenant_alloc, &tenant_id);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

    hwfq_flow_id_t fids[T2_FLOWS];
    uint32_t total_flow_weight = 0;
    for (int i = 0; i < T2_FLOWS; i++) {
        fids[i] = test_add_flow_with_weight(scheduler, tenant_id, FLOW_WEIGHTS[i]);
        TEST_ASSERT(fids[i] != 0, "Failed to add flow");
        total_flow_weight += FLOW_WEIGHTS[i];
    }

    // Pre-fill
    for (int i = 0; i < T2_FLOWS; i++) {
        for (int p = 0; p < PREFILL; p++) {
            hwfq_session_t work = {.user_data = NULL, .work_size = WORK_SIZE};
            hwfq_enqueue(scheduler, tenant_id, fids[i], &work);
        }
    }

    printf("    1 tenant, 10 flows (weights 512..1), total_flow_weight=%u\n", total_flow_weight);

    uint64_t flow_counts[T2_FLOWS] = {0};
    uint64_t total_dequeued = 0;

    for (int round = 0; round < T2_ROUNDS; round++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        if (ret != HWFQ_SUCCESS) break;

        for (int i = 0; i < T2_FLOWS; i++) {
            if (fids[i] == fid_out) {
                flow_counts[i]++;
                hwfq_session_t refill = {.user_data = NULL, .work_size = WORK_SIZE};
                hwfq_enqueue(scheduler, tenant_id, fids[i], &refill);
                break;
            }
        }
        total_dequeued++;
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    printf("    Results (%llu dequeued):\n", (unsigned long long)total_dequeued);
    bool all_ok = true;
    for (int i = 0; i < T2_FLOWS; i++) {
        double expected = (double)FLOW_WEIGHTS[i] / total_flow_weight;
        double actual = (double)flow_counts[i] / total_dequeued;
        double dev = deviation_pct(actual, expected);
        double tol = adaptive_tolerance(FLOW_WEIGHTS[i], FLOW_WEIGHTS[0]);
        bool ok = is_within_tolerance(actual, expected, tol);
        if (!ok) all_ok = false;

        printf("      Flow %d (w=%3u): expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
               i, FLOW_WEIGHTS[i], expected, actual, dev, ok ? "OK" : "FAIL");
    }

    TEST_ASSERT(all_ok, "Flow allocation ratios exceed tolerance");
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test 3: Cross-Tenant Fairness with Unequal Flow Counts
// ============================================================================
//
// 5 tenants with EQUAL weights but different numbers of active flows (1..16).
// Tenant-level fairness should hold regardless of flow count.

#define T3_TENANTS 5
#define T3_ROUNDS 10000

static const int T3_FLOW_COUNTS[T3_TENANTS] = {1, 2, 4, 8, 16};

void test_cross_tenant_fairness_unequal_flows(void) {
    hwfq_config_t config = {
        .max_tenants = 10,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 1000000000ULL,  // 1 GB/s
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_tenant_id_t tids[T3_TENANTS];
    hwfq_flow_id_t fids[T3_TENANTS][16];

    for (int t = 0; t < T3_TENANTS; t++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = 100
        };
        ret = hwfq_add_tenant(scheduler, &alloc, &tids[t]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

        for (int f = 0; f < T3_FLOW_COUNTS[t]; f++) {
            fids[t][f] = test_add_flow(scheduler, tids[t]);
            TEST_ASSERT(fids[t][f] != 0, "Failed to add flow");
        }
    }

    // Pre-fill: PREFILL items per flow per tenant
    for (int t = 0; t < T3_TENANTS; t++) {
        for (int f = 0; f < T3_FLOW_COUNTS[t]; f++) {
            for (int p = 0; p < PREFILL; p++) {
                hwfq_session_t work = {.user_data = NULL, .work_size = WORK_SIZE};
                hwfq_enqueue(scheduler, tids[t], fids[t][f], &work);
            }
        }
    }

    printf("    5 tenants (equal weight=100), flow counts: 1, 2, 4, 8, 16\n");

    uint64_t tenant_counts[T3_TENANTS] = {0};
    uint64_t total_dequeued = 0;

    for (int round = 0; round < T3_ROUNDS; round++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        if (ret != HWFQ_SUCCESS) break;

        for (int t = 0; t < T3_TENANTS; t++) {
            if (tids[t] == tid_out) {
                tenant_counts[t]++;
                // Re-enqueue for same tenant+flow
                hwfq_session_t refill = {.user_data = NULL, .work_size = WORK_SIZE};
                hwfq_enqueue(scheduler, tids[t], fid_out, &refill);
                break;
            }
        }
        total_dequeued++;
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    printf("    Results (%llu dequeued):\n", (unsigned long long)total_dequeued);
    bool all_ok = true;
    double expected = 1.0 / T3_TENANTS;
    for (int t = 0; t < T3_TENANTS; t++) {
        double actual = (double)tenant_counts[t] / total_dequeued;
        double dev = deviation_pct(actual, expected);
        bool ok = is_within_tolerance(actual, expected, 15.0);
        if (!ok) all_ok = false;

        printf("      Tenant %d (%2d flows): expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
               t, T3_FLOW_COUNTS[t], expected, actual, dev, ok ? "OK" : "FAIL");
    }

    TEST_ASSERT(all_ok, "Cross-tenant fairness violated by flow count differences");
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test 4: Full Two-Level Skew (Skewed Tenants AND Skewed Flows)
// ============================================================================
//
// 4 tenants (weights 800, 400, 200, 100)
// Each tenant has 4 flows (weights 400, 200, 100, 50)

#define T4_TENANTS 4
#define T4_FLOWS 4
#define T4_ROUNDS 15000

static const uint32_t T4_TENANT_WEIGHTS[T4_TENANTS] = {800, 400, 200, 100};
static const uint32_t T4_FLOW_WEIGHTS_ARR[T4_FLOWS] = {400, 200, 100, 50};

void test_hierarchical_two_level_skew(void) {
    hwfq_config_t config = {
        .max_tenants = 10,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 10000,
        .total_capacity = 1000000000ULL,  // 1 GB/s
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_tenant_id_t tids[T4_TENANTS];
    hwfq_flow_id_t fids[T4_TENANTS][T4_FLOWS];

    uint32_t total_tenant_weight = 0;
    uint32_t total_flow_weight = 0;
    for (int i = 0; i < T4_TENANTS; i++) total_tenant_weight += T4_TENANT_WEIGHTS[i];
    for (int i = 0; i < T4_FLOWS; i++) total_flow_weight += T4_FLOW_WEIGHTS_ARR[i];

    for (int t = 0; t < T4_TENANTS; t++) {
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = T4_TENANT_WEIGHTS[t]
        };
        ret = hwfq_add_tenant(scheduler, &alloc, &tids[t]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

        for (int f = 0; f < T4_FLOWS; f++) {
            fids[t][f] = test_add_flow_with_weight(scheduler, tids[t], T4_FLOW_WEIGHTS_ARR[f]);
            TEST_ASSERT(fids[t][f] != 0, "Failed to add flow");
        }
    }

    // Pre-fill
    for (int t = 0; t < T4_TENANTS; t++) {
        for (int f = 0; f < T4_FLOWS; f++) {
            for (int p = 0; p < PREFILL; p++) {
                hwfq_session_t work = {.user_data = NULL, .work_size = WORK_SIZE};
                hwfq_enqueue(scheduler, tids[t], fids[t][f], &work);
            }
        }
    }

    printf("    4 tenants (800,400,200,100) x 4 flows (400,200,100,50)\n");

    uint64_t tenant_counts[T4_TENANTS] = {0};
    uint64_t flow_counts[T4_TENANTS][T4_FLOWS] = {{0}};
    uint64_t total_dequeued = 0;

    for (int round = 0; round < T4_ROUNDS; round++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        if (ret != HWFQ_SUCCESS) break;

        for (int t = 0; t < T4_TENANTS; t++) {
            if (tids[t] == tid_out) {
                tenant_counts[t]++;
                for (int f = 0; f < T4_FLOWS; f++) {
                    if (fids[t][f] == fid_out) {
                        flow_counts[t][f]++;
                        break;
                    }
                }
                // Re-enqueue
                hwfq_session_t refill = {.user_data = NULL, .work_size = WORK_SIZE};
                hwfq_enqueue(scheduler, tids[t], fid_out, &refill);
                break;
            }
        }
        total_dequeued++;
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    // Validate tenant-level
    printf("    Tenant-level results (%llu dequeued):\n", (unsigned long long)total_dequeued);
    bool tenant_ok = true;
    for (int t = 0; t < T4_TENANTS; t++) {
        double expected = (double)T4_TENANT_WEIGHTS[t] / total_tenant_weight;
        double actual = (double)tenant_counts[t] / total_dequeued;
        double dev = deviation_pct(actual, expected);
        double tol = adaptive_tolerance(T4_TENANT_WEIGHTS[t], T4_TENANT_WEIGHTS[0]);
        bool ok = is_within_tolerance(actual, expected, tol);
        if (!ok) tenant_ok = false;

        printf("      Tenant %d (w=%3u): expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
               t, T4_TENANT_WEIGHTS[t], expected, actual, dev, ok ? "OK" : "FAIL");
    }

    // Validate per-flow within each tenant
    printf("    Flow-level results (within each tenant):\n");
    bool flow_ok = true;
    for (int t = 0; t < T4_TENANTS; t++) {
        if (tenant_counts[t] == 0) continue;
        for (int f = 0; f < T4_FLOWS; f++) {
            double expected = (double)T4_FLOW_WEIGHTS_ARR[f] / total_flow_weight;
            double actual = (double)flow_counts[t][f] / tenant_counts[t];
            double dev = deviation_pct(actual, expected);
            double tol = adaptive_tolerance(T4_FLOW_WEIGHTS_ARR[f], T4_FLOW_WEIGHTS_ARR[0]);
            bool ok = is_within_tolerance(actual, expected, tol);
            if (!ok) flow_ok = false;

            printf("      T%d F%d (w=%3u): expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
                   t, f, T4_FLOW_WEIGHTS_ARR[f], expected, actual, dev, ok ? "OK" : "FAIL");
        }
    }

    // Validate combined effective share
    printf("    Effective per-flow share (product of both levels):\n");
    bool combined_ok = true;
    for (int t = 0; t < T4_TENANTS; t++) {
        for (int f = 0; f < T4_FLOWS; f++) {
            double expected = ((double)T4_TENANT_WEIGHTS[t] / total_tenant_weight) *
                              ((double)T4_FLOW_WEIGHTS_ARR[f] / total_flow_weight);
            double actual = (double)flow_counts[t][f] / total_dequeued;
            double dev = deviation_pct(actual, expected);
            double tol = adaptive_tolerance(T4_TENANT_WEIGHTS[t], T4_TENANT_WEIGHTS[0]) +
                         adaptive_tolerance(T4_FLOW_WEIGHTS_ARR[f], T4_FLOW_WEIGHTS_ARR[0]);
            bool ok = is_within_tolerance(actual, expected, tol);
            if (!ok) combined_ok = false;

            printf("      T%d F%d: expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
                   t, f, expected, actual, dev, ok ? "OK" : "FAIL");
        }
    }

    TEST_ASSERT(tenant_ok, "Tenant-level allocation exceeded tolerance");
    TEST_ASSERT(flow_ok, "Flow-level allocation exceeded tolerance");
    TEST_ASSERT(combined_ok, "Combined two-level allocation exceeded tolerance");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test 5: 128 Tenants x 4 Flows Each with Varied Weights
// ============================================================================

#define T5_TENANTS 128
#define T5_FLOWS_PER 4
#define T5_ROUNDS 20000
#define T5_WEIGHT_CLASSES 8

static const uint32_t T5_CLASS_WEIGHTS[T5_WEIGHT_CLASSES] = {
    100, 150, 200, 250, 300, 350, 400, 450
};

void test_scale_128_tenants_4_flows(void) {
    hwfq_config_t config = {
        .max_tenants = 256,
        .max_flows_per_tenant = 100,
        .max_total_flows = 100000,
        .total_capacity = 1000000000ULL,  // 1 GB/s
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_tenant_id_t tids[T5_TENANTS];
    hwfq_flow_id_t fids[T5_TENANTS][T5_FLOWS_PER];
    uint32_t tenant_weights[T5_TENANTS];

    uint32_t total_weight = 0;
    for (int i = 0; i < T5_TENANTS; i++) {
        tenant_weights[i] = T5_CLASS_WEIGHTS[i % T5_WEIGHT_CLASSES];
        total_weight += tenant_weights[i];

        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = tenant_weights[i]
        };
        ret = hwfq_add_tenant(scheduler, &alloc, &tids[i]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

        for (int f = 0; f < T5_FLOWS_PER; f++) {
            fids[i][f] = test_add_flow(scheduler, tids[i]);
            TEST_ASSERT(fids[i][f] != 0, "Failed to add flow");
        }
    }

    // Pre-fill: 1 item per flow per tenant (128 * 4 = 512 items)
    for (int i = 0; i < T5_TENANTS; i++) {
        for (int f = 0; f < T5_FLOWS_PER; f++) {
            hwfq_session_t work = {.user_data = NULL, .work_size = WORK_SIZE};
            hwfq_enqueue(scheduler, tids[i], fids[i][f], &work);
        }
    }

    printf("    128 tenants x 4 flows, 8 weight classes (100..450), total_weight=%u\n",
           total_weight);

    uint64_t tenant_counts[T5_TENANTS] = {0};
    uint64_t total_dequeued = 0;

    for (int round = 0; round < T5_ROUNDS; round++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        if (ret != HWFQ_SUCCESS) break;

        for (int i = 0; i < T5_TENANTS; i++) {
            if (tids[i] == tid_out) {
                tenant_counts[i]++;
                hwfq_session_t refill = {.user_data = NULL, .work_size = WORK_SIZE};
                hwfq_enqueue(scheduler, tids[i], fid_out, &refill);
                break;
            }
        }
        total_dequeued++;
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    printf("    Results by weight class (%llu dequeued):\n", (unsigned long long)total_dequeued);
    bool all_ok = true;

    int tenants_per_class = T5_TENANTS / T5_WEIGHT_CLASSES;
    for (int cls = 0; cls < T5_WEIGHT_CLASSES; cls++) {
        uint64_t class_count = 0;
        uint64_t class_min = UINT64_MAX, class_max = 0;

        for (int j = 0; j < T5_TENANTS; j++) {
            if ((j % T5_WEIGHT_CLASSES) == cls) {
                class_count += tenant_counts[j];
                if (tenant_counts[j] < class_min) class_min = tenant_counts[j];
                if (tenant_counts[j] > class_max) class_max = tenant_counts[j];
            }
        }

        double expected = (double)(T5_CLASS_WEIGHTS[cls] * tenants_per_class) / total_weight;
        double actual = (double)class_count / total_dequeued;
        double dev = deviation_pct(actual, expected);
        double tol = 15.0;
        bool ok = is_within_tolerance(actual, expected, tol);
        if (!ok) all_ok = false;

        double spread = (class_min > 0) ? (double)class_max / class_min : 0;
        printf("      Class w=%3u (x%d): expected %.4f, actual %.4f (%+.2f%%), "
               "spread=%.2fx [%s]\n",
               T5_CLASS_WEIGHTS[cls], tenants_per_class, expected, actual, dev,
               spread, ok ? "OK" : "FAIL");
    }

    TEST_ASSERT(all_ok, "Weight class allocation exceeded tolerance");
    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test 6: Combined Scale + Two-Level Hierarchy
// ============================================================================
//
// 32 tenants across 4 geometric weight tiers (8 tenants per tier).
// Each tenant has 4 flows with geometric weights.
// Validates tenant-level, flow-level, AND combined effective share at scale.
// This is the single definitive test for the full H-WFQ hierarchy.

#define T6_TENANTS 256
#define T6_TIERS 4
#define T6_PER_TIER 64
#define T6_FLOWS 4
#define T6_ROUNDS 50000

static const uint32_t T6_TENANT_WEIGHTS[T6_TIERS] = {800, 400, 200, 100};
static const uint32_t T6_FLOW_WEIGHTS[T6_FLOWS] = {400, 200, 100, 50};

void test_combined_scale_and_hierarchy(void) {
    hwfq_config_t config = {
        .max_tenants = 512,
        .max_flows_per_tenant = 1000,
        .max_total_flows = 50000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_tenant_id_t tids[T6_TENANTS];
    hwfq_flow_id_t fids[T6_TENANTS][T6_FLOWS];

    uint32_t total_tenant_weight = 0;
    uint32_t total_flow_weight = 0;
    for (int i = 0; i < T6_TIERS; i++) total_tenant_weight += T6_TENANT_WEIGHTS[i] * T6_PER_TIER;
    for (int i = 0; i < T6_FLOWS; i++) total_flow_weight += T6_FLOW_WEIGHTS[i];

    for (int i = 0; i < T6_TENANTS; i++) {
        uint32_t tier = i / T6_PER_TIER;
        hwfq_allocation_t alloc = {
            .allocation_type = HWFQ_ALLOCATION_WEIGHT,
            .weight = T6_TENANT_WEIGHTS[tier]
        };
        ret = hwfq_add_tenant(scheduler, &alloc, &tids[i]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

        for (int f = 0; f < T6_FLOWS; f++) {
            fids[i][f] = test_add_flow_with_weight(scheduler, tids[i], T6_FLOW_WEIGHTS[f]);
            TEST_ASSERT(fids[i][f] != 0, "Failed to add flow");
        }
    }

    // Pre-fill backlog
    for (int i = 0; i < T6_TENANTS; i++) {
        for (int f = 0; f < T6_FLOWS; f++) {
            for (int p = 0; p < PREFILL; p++) {
                hwfq_session_t work = {.user_data = NULL, .work_size = WORK_SIZE};
                hwfq_enqueue(scheduler, tids[i], fids[i][f], &work);
            }
        }
    }

    printf("    %d tenants (%d tiers x %d each), %d flows/tenant (weights %u:%u:%u:%u)\n",
           T6_TENANTS, T6_TIERS, T6_PER_TIER, T6_FLOWS,
           T6_FLOW_WEIGHTS[0], T6_FLOW_WEIGHTS[1], T6_FLOW_WEIGHTS[2], T6_FLOW_WEIGHTS[3]);
    printf("    Tenant weights: %u, %u, %u, %u (total=%u)\n",
           T6_TENANT_WEIGHTS[0], T6_TENANT_WEIGHTS[1],
           T6_TENANT_WEIGHTS[2], T6_TENANT_WEIGHTS[3], total_tenant_weight);

    // Tracking: per (tenant, flow) counts
    uint64_t flow_counts[T6_TENANTS][T6_FLOWS] = {{0}};
    uint64_t total_dequeued = 0;

    for (int round = 0; round < T6_ROUNDS; round++) {
        hwfq_session_t work_out;
        hwfq_tenant_id_t tid_out;
        hwfq_flow_id_t fid_out;
        ret = hwfq_dequeue(scheduler, &work_out, &tid_out, &fid_out);
        if (ret != HWFQ_SUCCESS) break;

        for (int i = 0; i < T6_TENANTS; i++) {
            if (tids[i] == tid_out) {
                for (int f = 0; f < T6_FLOWS; f++) {
                    if (fids[i][f] == fid_out) {
                        flow_counts[i][f]++;
                        break;
                    }
                }
                hwfq_session_t refill = {.user_data = NULL, .work_size = WORK_SIZE};
                hwfq_enqueue(scheduler, tids[i], fid_out, &refill);
                break;
            }
        }
        total_dequeued++;
        hwfq_complete(scheduler, &work_out, tid_out, fid_out, get_time_ns_bench());
    }

    // === Tenant-level validation (aggregate per tier) ===
    printf("\n    TENANT-LEVEL (per tier, %llu total dequeued):\n", (unsigned long long)total_dequeued);
    printf("    %-6s  %-8s  %-8s  %-10s  %-10s  %-10s  %s\n",
           "Tier", "Weight", "Tenants", "Expected", "Actual", "Deviation", "Status");

    bool tenant_ok = true;
    double max_tenant_dev = 0;
    for (int tier = 0; tier < T6_TIERS; tier++) {
        uint64_t tier_count = 0;
        for (int j = 0; j < T6_PER_TIER; j++) {
            int idx = tier * T6_PER_TIER + j;
            for (int f = 0; f < T6_FLOWS; f++) tier_count += flow_counts[idx][f];
        }
        double expected = (double)(T6_TENANT_WEIGHTS[tier] * T6_PER_TIER) / total_tenant_weight;
        double actual = (double)tier_count / total_dequeued;
        double dev = deviation_pct(actual, expected);
        double abs_dev = fabs(dev);
        if (abs_dev > max_tenant_dev) max_tenant_dev = abs_dev;
        double tol = adaptive_tolerance(T6_TENANT_WEIGHTS[tier], T6_TENANT_WEIGHTS[0]);
        bool ok = is_within_tolerance(actual, expected, tol);
        if (!ok) tenant_ok = false;

        printf("    %-6d  %-8u  %-8d  %-10.4f  %-10.4f  %+.2f%%      [%s]\n",
               tier, T6_TENANT_WEIGHTS[tier], T6_PER_TIER, expected, actual, dev,
               ok ? "OK" : "FAIL");
    }
    printf("    Max tenant-level deviation: %.2f%%\n", max_tenant_dev);

    // === Flow-level validation (within-tenant, averaged per tier) ===
    printf("\n    FLOW-LEVEL (within-tenant, averaged per tier):\n");
    printf("    %-6s  %-8s  %-10s  %-10s  %-10s  %s\n",
           "Tier", "FlowW", "Expected", "AvgActual", "MaxDev", "Status");

    bool flow_ok = true;
    double max_flow_dev = 0;
    for (int tier = 0; tier < T6_TIERS; tier++) {
        for (int f = 0; f < T6_FLOWS; f++) {
            double expected = (double)T6_FLOW_WEIGHTS[f] / total_flow_weight;
            double sum_actual = 0;
            double worst_dev = 0;

            for (int j = 0; j < T6_PER_TIER; j++) {
                int idx = tier * T6_PER_TIER + j;
                uint64_t tenant_total = 0;
                for (int ff = 0; ff < T6_FLOWS; ff++) tenant_total += flow_counts[idx][ff];
                if (tenant_total == 0) continue;

                double actual = (double)flow_counts[idx][f] / tenant_total;
                sum_actual += actual;
                double dev = fabs(deviation_pct(actual, expected));
                if (dev > worst_dev) worst_dev = dev;
            }

            double avg_actual = sum_actual / T6_PER_TIER;
            if (worst_dev > max_flow_dev) max_flow_dev = worst_dev;
            double tol = adaptive_tolerance(T6_FLOW_WEIGHTS[f], T6_FLOW_WEIGHTS[0]);
            bool ok = (worst_dev <= tol);
            if (!ok) flow_ok = false;

            printf("    %-6d  %-8u  %-10.4f  %-10.4f  %+.2f%%      [%s]\n",
                   tier, T6_FLOW_WEIGHTS[f], expected, avg_actual, worst_dev,
                   ok ? "OK" : "FAIL");
        }
    }
    printf("    Max flow-level deviation: %.2f%%\n", max_flow_dev);

    // === Combined effective share (per tier x flow) ===
    printf("\n    EFFECTIVE SHARE (tenant_tier x flow, product of both levels):\n");
    printf("    %-12s  %-10s  %-10s  %-10s  %s\n",
           "Tier:Flow", "Expected", "Actual", "Deviation", "Status");

    bool combined_ok = true;
    double max_combined_dev = 0;
    for (int tier = 0; tier < T6_TIERS; tier++) {
        for (int f = 0; f < T6_FLOWS; f++) {
            double expected = ((double)(T6_TENANT_WEIGHTS[tier] * T6_PER_TIER) / total_tenant_weight) *
                              ((double)T6_FLOW_WEIGHTS[f] / total_flow_weight);

            uint64_t combo_count = 0;
            for (int j = 0; j < T6_PER_TIER; j++) {
                combo_count += flow_counts[tier * T6_PER_TIER + j][f];
            }
            double actual = (double)combo_count / total_dequeued;
            double dev = deviation_pct(actual, expected);
            double abs_dev = fabs(dev);
            if (abs_dev > max_combined_dev) max_combined_dev = abs_dev;

            double tol = adaptive_tolerance(T6_TENANT_WEIGHTS[tier], T6_TENANT_WEIGHTS[0]) +
                         adaptive_tolerance(T6_FLOW_WEIGHTS[f], T6_FLOW_WEIGHTS[0]);
            bool ok = is_within_tolerance(actual, expected, tol);
            if (!ok) combined_ok = false;

            printf("    T%d:F%-7d  %-10.4f  %-10.4f  %+.2f%%      [%s]\n",
                   tier, f, expected, actual, dev, ok ? "OK" : "FAIL");
        }
    }
    printf("    Max combined deviation: %.2f%%\n", max_combined_dev);

    printf("\n    SUMMARY: tenant_max=%.2f%%, flow_max=%.2f%%, combined_max=%.2f%%\n",
           max_tenant_dev, max_flow_dev, max_combined_dev);

    TEST_ASSERT(tenant_ok, "Tenant-level allocation exceeded tolerance");
    TEST_ASSERT(flow_ok, "Flow-level allocation exceeded tolerance");
    TEST_ASSERT(combined_ok, "Combined allocation exceeded tolerance");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Test 7: Work-Conserving Redistribution
// ============================================================================
//
// Validates tech.pdf p.4: "unused resources can be temporarily shared across
// tenants or redistributed within a tenant's own user processes."
//
// Phase 1: All 256 tenants active — measure baseline allocation per tier.
// Phase 2: Tier 0 + Tier 1 go idle (192 of 256 tenants stop sending work).
//          Remaining 128 tenants (Tiers 2+3) should absorb 100% of capacity,
//          split proportionally by their weights (200:100 = 2:1).
// Phase 3: Tier 0 reactivates — capacity must redistribute back.
//
// The key assertion: NO capacity is wasted when tenants are idle.

#define T7_TENANTS    T6_TENANTS   // reuse 256
#define T7_TIERS      T6_TIERS
#define T7_PER_TIER   T6_PER_TIER
#define T7_FLOWS      1            // 1 flow per tenant (simplify — this tests tenant-level)
#define T7_PHASE_ROUNDS 15000

void test_work_conserving(void) {
    hwfq_config_t config = {
        .max_tenants = 512,
        .max_flows_per_tenant = 100,
        .max_total_flows = 50000,
        .total_capacity = 1000000000ULL,
        .num_groups = 16,
        .bins_per_group = 2048,
        .enable_statistics = false,
    };

    hwfq_scheduler_t *scheduler = NULL;
    int ret = hwfq_init(&config, &scheduler);
    TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to create scheduler");

    hwfq_tenant_id_t tids[T7_TENANTS];
    hwfq_flow_id_t fids[T7_TENANTS];

    uint32_t total_weight = 0;
    for (int i = 0; i < T7_TENANTS; i++) {
        uint32_t tier = i / T7_PER_TIER;
        uint32_t w = T6_TENANT_WEIGHTS[tier];
        total_weight += w;

        hwfq_allocation_t alloc = {.allocation_type = HWFQ_ALLOCATION_WEIGHT, .weight = w};
        ret = hwfq_add_tenant(scheduler, &alloc, &tids[i]);
        TEST_ASSERT(ret == HWFQ_SUCCESS, "Failed to add tenant");

        fids[i] = test_add_flow(scheduler, tids[i]);
        TEST_ASSERT(fids[i] != 0, "Failed to add flow");
    }

    printf("    256 tenants (4 tiers: 800,400,200,100 x 64 each), 1 flow per tenant\n");

    // Helper: run a phase with a subset of active tiers
    // active_tiers is a bitmask (bit 0 = tier 0, etc.)
    // Returns per-tier dequeue counts
    #define RUN_PHASE(phase_name, active_mask, rounds, tier_counts_out, total_out) do { \
        memset(tier_counts_out, 0, sizeof(uint64_t) * T7_TIERS); \
        *(total_out) = 0; \
        /* Pre-fill active tenants */ \
        for (int _i = 0; _i < T7_TENANTS; _i++) { \
            int _tier = _i / T7_PER_TIER; \
            if (!((active_mask) & (1 << _tier))) continue; \
            for (int _p = 0; _p < PREFILL; _p++) { \
                hwfq_session_t _w = {.user_data = NULL, .work_size = WORK_SIZE}; \
                hwfq_enqueue(scheduler, tids[_i], fids[_i], &_w); \
            } \
        } \
        for (int _r = 0; _r < (rounds); _r++) { \
            hwfq_session_t _wo; \
            hwfq_tenant_id_t _to; \
            hwfq_flow_id_t _fo; \
            int _ret = hwfq_dequeue(scheduler, &_wo, &_to, &_fo); \
            if (_ret != HWFQ_SUCCESS) break; \
            for (int _i = 0; _i < T7_TENANTS; _i++) { \
                if (tids[_i] == _to) { \
                    tier_counts_out[_i / T7_PER_TIER]++; \
                    hwfq_session_t _rf = {.user_data = NULL, .work_size = WORK_SIZE}; \
                    hwfq_enqueue(scheduler, tids[_i], fids[_i], &_rf); \
                    break; \
                } \
            } \
            (*(total_out))++; \
            hwfq_complete(scheduler, &_wo, _to, _fo, get_time_ns_bench()); \
        } \
        /* Drain remaining to clean state */ \
        { hwfq_session_t _dw; hwfq_tenant_id_t _dt; hwfq_flow_id_t _df; \
          while (hwfq_dequeue(scheduler, &_dw, &_dt, &_df) == HWFQ_SUCCESS) \
              hwfq_complete(scheduler, &_dw, _dt, _df, get_time_ns_bench()); } \
    } while(0)

    uint64_t tier_counts[T7_TIERS];
    uint64_t phase_total;

    // === Phase 1: All tiers active ===
    RUN_PHASE("all_active", 0xF, T7_PHASE_ROUNDS, tier_counts, &phase_total);

    printf("\n    PHASE 1 — All 256 tenants active (%llu dequeued):\n",
           (unsigned long long)phase_total);

    bool phase1_ok = true;
    for (int t = 0; t < T7_TIERS; t++) {
        double expected = (double)(T6_TENANT_WEIGHTS[t] * T7_PER_TIER) / total_weight;
        double actual = (double)tier_counts[t] / phase_total;
        double dev = deviation_pct(actual, expected);
        bool ok = is_within_tolerance(actual, expected, 10.0);
        if (!ok) phase1_ok = false;
        printf("      Tier %d (w=%u): expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
               t, T6_TENANT_WEIGHTS[t], expected, actual, dev, ok ? "OK" : "FAIL");
    }

    // === Phase 2: Tiers 0+1 go idle (only tiers 2+3 active) ===
    // Active weight: 200*64 + 100*64 = 19200
    // Tier 2 should get 200/300 = 66.7%, Tier 3 should get 100/300 = 33.3%
    RUN_PHASE("half_idle", 0xC, T7_PHASE_ROUNDS, tier_counts, &phase_total);

    uint32_t active_weight = (T6_TENANT_WEIGHTS[2] + T6_TENANT_WEIGHTS[3]) * T7_PER_TIER;
    printf("\n    PHASE 2 — Tiers 0+1 idle, Tiers 2+3 active (%llu dequeued):\n",
           (unsigned long long)phase_total);
    printf("      (Work-conserving: %u of %u weight active, 100%% of capacity should be used)\n",
           active_weight, total_weight);

    bool phase2_ok = true;
    // Tiers 0+1 should get 0
    for (int t = 0; t < 2; t++) {
        bool ok = (tier_counts[t] == 0);
        if (!ok) phase2_ok = false;
        printf("      Tier %d (w=%u): IDLE — got %llu [%s]\n",
               t, T6_TENANT_WEIGHTS[t], (unsigned long long)tier_counts[t],
               ok ? "OK" : "FAIL");
    }
    // Tiers 2+3 should split 100% in 2:1 ratio
    for (int t = 2; t < T7_TIERS; t++) {
        double expected = (double)(T6_TENANT_WEIGHTS[t] * T7_PER_TIER) / active_weight;
        double actual = (double)tier_counts[t] / phase_total;
        double dev = deviation_pct(actual, expected);
        bool ok = is_within_tolerance(actual, expected, 10.0);
        if (!ok) phase2_ok = false;
        printf("      Tier %d (w=%u): expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
               t, T6_TENANT_WEIGHTS[t], expected, actual, dev, ok ? "OK" : "FAIL");
    }

    // === Phase 3: All tiers reactivate ===
    RUN_PHASE("reactivate", 0xF, T7_PHASE_ROUNDS, tier_counts, &phase_total);

    printf("\n    PHASE 3 — All 256 tenants reactivated (%llu dequeued):\n",
           (unsigned long long)phase_total);

    bool phase3_ok = true;
    for (int t = 0; t < T7_TIERS; t++) {
        double expected = (double)(T6_TENANT_WEIGHTS[t] * T7_PER_TIER) / total_weight;
        double actual = (double)tier_counts[t] / phase_total;
        double dev = deviation_pct(actual, expected);
        bool ok = is_within_tolerance(actual, expected, 10.0);
        if (!ok) phase3_ok = false;
        printf("      Tier %d (w=%u): expected %.4f, actual %.4f (%+.2f%%) [%s]\n",
               t, T6_TENANT_WEIGHTS[t], expected, actual, dev, ok ? "OK" : "FAIL");
    }

    printf("\n    WORK-CONSERVING SUMMARY:\n");
    printf("      Phase 1 (all active):     %s\n", phase1_ok ? "PASS" : "FAIL");
    printf("      Phase 2 (half idle):      %s — idle tenants got 0, "
           "active tenants absorbed 100%%\n", phase2_ok ? "PASS" : "FAIL");
    printf("      Phase 3 (reactivated):    %s — capacity redistributed back\n",
           phase3_ok ? "PASS" : "FAIL");

    TEST_ASSERT(phase1_ok, "Phase 1 failed");
    TEST_ASSERT(phase2_ok, "Phase 2 (work-conserving redistribution) failed");
    TEST_ASSERT(phase3_ok, "Phase 3 (reactivation) failed");

    hwfq_destroy(scheduler);
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== H-WFQ Hierarchical Fairness at Scale ===\n\n");

    printf("Running test_many_tenants_geometric_weights (128 tenants, 8 tiers)...\n");
    test_many_tenants_geometric_weights();

    printf("\nRunning test_intra_tenant_flow_fairness (1 tenant, 10 skewed flows)...\n");
    test_intra_tenant_flow_fairness();

    printf("\nRunning test_cross_tenant_fairness_unequal_flows (5 tenants, 1-16 flows)...\n");
    test_cross_tenant_fairness_unequal_flows();

    printf("\nRunning test_hierarchical_two_level_skew (4 tenants x 4 flows, both skewed)...\n");
    test_hierarchical_two_level_skew();

    printf("\nRunning test_scale_128_tenants_4_flows (128 tenants x 4 flows)...\n");
    test_scale_128_tenants_4_flows();

    printf("\nRunning test_combined_scale_and_hierarchy (256 tenants x 4 flows, both skewed)...\n");
    test_combined_scale_and_hierarchy();

    printf("\nRunning test_work_conserving (256 tenants, idle/reactivate phases)...\n");
    test_work_conserving();

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
