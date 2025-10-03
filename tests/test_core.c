#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../include/stick_tables.h"
#include "../include/cache/cache.h"
#include "../include/health/health.h"

void test_stick_tables() {
    printf("Testing stick tables...\n");

    stick_table_t *table = stktable_new("test", STKTABLE_TYPE_IP, 1000, 3600);
    assert(table != NULL);

    stick_key_t key = {
        .type = STKTABLE_TYPE_IP,
        .data.ipv4.s_addr = 0x0100007f  // 127.0.0.1
    };

    stick_entry_t *entry = stktable_get(table, &key);
    assert(entry != NULL);

    uint32_t val = 100;
    stktable_update_key(table, &key, STKTABLE_DATA_CONN_CNT, &val);

    entry = stktable_lookup(table, &key);
    assert(entry != NULL);
    assert(atomic_load(&entry->counters.conn_cnt) == 100);

    stktable_free(table);
    printf("Stick tables test passed\n");
}

void test_cache() {
    printf("Testing cache...\n");

    cache_t *cache = cache_create("test", 1024*1024, 100*1024);
    assert(cache != NULL);

    const char *key = "test_key";
    cache_entry_t *entry = calloc(1, sizeof(cache_entry_t));
    entry->data.ptr = strdup("test data");
    entry->data.len = 9;
    entry->size = 9;

    int ret = cache_insert(cache, key, entry);
    assert(ret == 0);

    cache_entry_t *found = cache_lookup(cache, key);
    assert(found != NULL);
    assert(strcmp(found->data.ptr, "test data") == 0);

    cache_destroy(cache);
    printf("Cache test passed\n");
}

void test_health_checks() {
    printf("Testing health checks...\n");

    check_t *check = check_new(HCHK_TYPE_TCP);
    assert(check != NULL);
    assert(check->type == HCHK_TYPE_TCP);
    assert(check->state == CHK_ST_INIT);

    check->interval.inter = 1000;
    check->interval.rise = 2;
    check->interval.fall = 3;

    check_free(check);
    printf("Health check test passed\n");
}

void test_compression() {
    printf("Testing compression...\n");

    compression_ctx_t ctx;
    int ret = compression_init(&ctx, COMP_TYPE_GZIP, 6);
    assert(ret == 0);

    const char *input = "This is a test string to compress. It should be compressed well.";
    struct buffer in_buf = { .p = (char*)input, .i = strlen(input), .size = 1024 };
    struct buffer out_buf = { .p = malloc(1024), .i = 0, .size = 1024 };

    ret = compression_process(&ctx, &in_buf, &out_buf, COMP_FINISH);
    assert(out_buf.i > 0);
    assert(out_buf.i < strlen(input));  // Should be compressed

    compression_end(&ctx);
    free(out_buf.p);
    printf("Compression test passed\n");
}

int main() {
    printf("Running UltraBalancer unit tests...\n\n");

    test_stick_tables();
    test_cache();
    test_health_checks();
    test_compression();

    printf("\nAll tests passed!\n");
    return 0;
}