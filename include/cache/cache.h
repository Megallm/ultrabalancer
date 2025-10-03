#ifndef CACHE_CACHE_H
#define CACHE_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <zlib.h>
#include "core/common.h"

struct stream;
struct channel;
struct http_txn;

#define CACHE_F_SHARED        0x00000001
#define CACHE_F_PRIVATE       0x00000002
#define CACHE_F_NO_CACHE      0x00000004
#define CACHE_F_NO_STORE      0x00000008
#define CACHE_F_NO_TRANSFORM  0x00000010
#define CACHE_F_MUST_REVALIDATE 0x00000020
#define CACHE_F_PROXY_REVALIDATE 0x00000040
#define CACHE_F_MAX_AGE       0x00000080
#define CACHE_F_S_MAXAGE      0x00000100
#define CACHE_F_COMPRESSED    0x00000200
#define CACHE_F_BROTLI        0x00000400

typedef struct cache_entry {
    char *key;
    uint32_t key_hash;

    struct {
        char *ptr;
        size_t len;
        size_t alloc;
    } data;

    struct {
        uint16_t status;
        char *reason;
        struct http_hdr *headers;
    } response;

    uint32_t flags;
    time_t created;
    time_t expires;
    time_t last_access;
    uint32_t access_count;
    uint32_t size;

    char *etag;
    time_t last_modified;
    char *vary;

    pthread_rwlock_t lock;

    struct cache_entry *hash_next;
    struct cache_entry *lru_prev;
    struct cache_entry *lru_next;
} cache_entry_t;

typedef struct cache {
    char *name;
    uint32_t max_size;
    uint32_t current_size;
    uint32_t max_object_size;
    uint32_t max_age;
    uint32_t entry_count;
    uint32_t max_entries;

    struct {
        cache_entry_t **table;
        uint32_t size;
        uint32_t mask;
        pthread_spinlock_t *locks;
    } hash;

    struct {
        cache_entry_t *head;
        cache_entry_t *tail;
        pthread_spinlock_t lock;
    } lru;

    struct {
        _Atomic uint64_t hits;
        _Atomic uint64_t misses;
        _Atomic uint64_t inserts;
        _Atomic uint64_t evictions;
        _Atomic uint64_t bytes_in;
        _Atomic uint64_t bytes_out;
    } stats;

    uint32_t flags;
    pthread_rwlock_t lock;

    struct cache *next;
} cache_t;

typedef struct compression_ctx {
    int type;
    int level;
    z_stream zstrm;
    void *brotli_state;

    struct buffer *in;
    struct buffer *out;

    uint32_t consumed;
    uint32_t produced;
} compression_ctx_t;

cache_t* cache_create(const char *name, uint32_t max_size, uint32_t max_object_size);
void cache_destroy(cache_t *cache);

cache_entry_t* cache_lookup(cache_t *cache, const char *key);
int cache_insert(cache_t *cache, const char *key, cache_entry_t *entry);
void cache_delete(cache_t *cache, const char *key);
void cache_purge(cache_t *cache);

int cache_check_request(struct stream *s, struct channel *req, struct channel *res);
int cache_check_response(struct stream *s, struct channel *res);
int cache_store_response(struct stream *s, struct channel *res);

bool cache_entry_is_valid(cache_entry_t *entry);
bool cache_entry_is_fresh(cache_entry_t *entry, time_t now);
int cache_entry_needs_revalidation(cache_entry_t *entry);

uint32_t cache_hash_key(const char *key);
char* cache_build_key(struct http_txn *txn);
char* cache_build_vary_key(struct http_txn *txn, const char *vary);

void cache_update_lru(cache_t *cache, cache_entry_t *entry);
void cache_evict_lru(cache_t *cache);

int compression_init(compression_ctx_t *ctx, int type, int level);
int compression_process(compression_ctx_t *ctx, struct buffer *in, struct buffer *out, int flags);
void compression_end(compression_ctx_t *ctx);

int compress_http_response(struct stream *s, struct channel *res);
int decompress_http_response(struct stream *s, struct channel *res);

const char* get_encoding_name(int type);
int parse_accept_encoding(const char *value);

#endif