// ============================================================================
// Chunked Entries Unit Tests
// ============================================================================
//
// Tests for the chunked entry storage used for per-owner entry management.
// Each chunk holds 1024 entries, and chunks are allocated on demand.
//
// ============================================================================

#include "test_common.h"
#include "../src/hwfq_chunked_entries.h"
#include "../src/hwfq_internal.h"

// ============================================================================
// Test 1: Basic Lifecycle
// ============================================================================

static void test_chunked_init_destroy(void) {
    printf("  test_chunked_init_destroy...");

    hwfq_chunked_entries_t ce;

    hwfq_chunked_entries_init(&ce, 1000, NULL, NULL);

    assert(hwfq_chunked_entries_count(&ce) == 0);
    assert(hwfq_chunked_entries_has_capacity(&ce));

    hwfq_chunked_entries_destroy(&ce);

    printf(" PASSED\n");
}

// ============================================================================
// Test 2: Single Alloc/Free
// ============================================================================

static void test_chunked_alloc_free(void) {
    printf("  test_chunked_alloc_free...");

    hwfq_chunked_entries_t ce;
    hwfq_chunked_entries_init(&ce, 1000, NULL, NULL);

    uint32_t entry_id;
    int ret = hwfq_chunked_entries_alloc(&ce, &entry_id);
    (void)ret;
    assert(ret == 0);
    assert(entry_id != HWFQ_ENTRY_NONE);

    assert(hwfq_chunked_entries_count(&ce) == 1);

    entry_config_t *entry = hwfq_chunked_get(&ce, entry_id);
    (void)entry;
    assert(entry != NULL);

    hwfq_chunked_entries_free(&ce, entry_id);

    hwfq_chunked_entries_destroy(&ce);

    printf(" PASSED\n");
}

// ============================================================================
// Test 3: Allocate Many (Across Chunks)
// ============================================================================

static void test_chunked_alloc_many(void) {
    printf("  test_chunked_alloc_many...");

    hwfq_chunked_entries_t ce;
    hwfq_chunked_entries_init(&ce, 5000, NULL, NULL);

    uint32_t entry_ids[2500];
    for (int i = 0; i < 2500; i++) {
        int ret = hwfq_chunked_entries_alloc(&ce, &entry_ids[i]);
        (void)ret;
        assert(ret == 0);
        assert(entry_ids[i] != HWFQ_ENTRY_NONE);
    }

    assert(hwfq_chunked_entries_count(&ce) == 2500);

    for (int i = 0; i < 2500; i++) {
        entry_config_t *entry = hwfq_chunked_get(&ce, entry_ids[i]);
        (void)entry;
        assert(entry != NULL);
    }

    hwfq_chunked_entries_destroy(&ce);

    printf(" PASSED\n");
}

// ============================================================================
// Test 4: Free Reuse
// ============================================================================

static void test_chunked_free_reuse(void) {
    printf("  test_chunked_free_reuse...");

    hwfq_chunked_entries_t ce;
    hwfq_chunked_entries_init(&ce, 1000, NULL, NULL);

    uint32_t entry_ids[10];
    for (int i = 0; i < 10; i++) {
        int ret = hwfq_chunked_entries_alloc(&ce, &entry_ids[i]);
        (void)ret;
        assert(ret == 0);
    }

    for (int i = 0; i < 5; i++) {
        hwfq_chunked_entries_free(&ce, entry_ids[i]);
    }

    uint32_t new_ids[5];
    for (int i = 0; i < 5; i++) {
        int ret = hwfq_chunked_entries_alloc(&ce, &new_ids[i]);
        (void)ret;
        assert(ret == 0);
    }

    assert(hwfq_chunked_entries_count(&ce) == 10);

    hwfq_chunked_entries_destroy(&ce);

    printf(" PASSED\n");
}

// ============================================================================
// Test 5: Entry Lookup
// ============================================================================

static void test_chunked_entry_lookup(void) {
    printf("  test_chunked_entry_lookup...");

    hwfq_chunked_entries_t ce;
    hwfq_chunked_entries_init(&ce, 1000, NULL, NULL);

    uint32_t entry_id;
    int ret = hwfq_chunked_entries_alloc(&ce, &entry_id);
    (void)ret;
    assert(ret == 0);

    entry_config_t *entry = hwfq_chunked_get(&ce, entry_id);
    (void)entry;
    assert(entry != NULL);

    entry_config_t *invalid = hwfq_chunked_get(&ce, HWFQ_ENTRY_NONE);
    (void)invalid;
    assert(invalid == NULL);

    entry_config_t *out_of_range = hwfq_chunked_get(&ce, 9999);
    (void)out_of_range;
    assert(out_of_range == NULL);

    hwfq_chunked_entries_destroy(&ce);

    printf(" PASSED\n");
}

// ============================================================================
// Test 6: Count Accuracy
// ============================================================================

static void test_chunked_count(void) {
    printf("  test_chunked_count...");

    hwfq_chunked_entries_t ce;
    hwfq_chunked_entries_init(&ce, 1000, NULL, NULL);

    assert(hwfq_chunked_entries_count(&ce) == 0);

    uint32_t entry_ids[50];
    for (int i = 0; i < 50; i++) {
        int ret = hwfq_chunked_entries_alloc(&ce, &entry_ids[i]);
        (void)ret;
        assert(ret == 0);
        assert(hwfq_chunked_entries_count(&ce) == (uint32_t)(i + 1));
    }

    for (int i = 0; i < 25; i++) {
        hwfq_chunked_entries_free(&ce, entry_ids[i]);
    }

    assert(hwfq_chunked_entries_count(&ce) == 25);

    hwfq_chunked_entries_destroy(&ce);

    printf(" PASSED\n");
}

// ============================================================================
// Test 7: Capacity Limits
// ============================================================================

static void test_chunked_max_capacity(void) {
    printf("  test_chunked_max_capacity...");

    hwfq_chunked_entries_t ce;
    hwfq_chunked_entries_init(&ce, 50, NULL, NULL);

    uint32_t entry_id;
    for (int i = 0; i < 50; i++) {
        int ret = hwfq_chunked_entries_alloc(&ce, &entry_id);
        (void)ret;
        assert(ret == 0);
    }

    assert(hwfq_chunked_entries_count(&ce) == 50);
    assert(!hwfq_chunked_entries_has_capacity(&ce));

    int ret = hwfq_chunked_entries_alloc(&ce, &entry_id);
    (void)ret;
    assert(ret != 0);

    hwfq_chunked_entries_destroy(&ce);

    printf(" PASSED\n");
}

// ============================================================================
// Test 8: Encode/Decode
// ============================================================================

static void test_chunked_encode_decode(void) {
    printf("  test_chunked_encode_decode...");

    uint32_t chunk, slot;

    hwfq_chunked_decode(0, &chunk, &slot);
    assert(chunk == 0);
    assert(slot == 0);

    hwfq_chunked_decode(1023, &chunk, &slot);
    assert(chunk == 0);
    assert(slot == 1023);

    hwfq_chunked_decode(1024, &chunk, &slot);
    assert(chunk == 1);
    assert(slot == 0);

    hwfq_chunked_decode(2048, &chunk, &slot);
    assert(chunk == 2);
    assert(slot == 0);

    uint32_t entry_id = hwfq_chunked_encode(5, 512);
    hwfq_chunked_decode(entry_id, &chunk, &slot);
    assert(chunk == 5);
    assert(slot == 512);

    printf(" PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("Chunked Entries Tests\n");
    printf("=====================\n\n");

    test_chunked_init_destroy();
    test_chunked_alloc_free();
    test_chunked_alloc_many();
    test_chunked_free_reuse();
    test_chunked_entry_lookup();
    test_chunked_count();
    test_chunked_max_capacity();
    test_chunked_encode_decode();

    printf("\nAll chunked entries tests passed!\n");
    return 0;
}
