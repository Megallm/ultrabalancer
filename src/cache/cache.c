#include "cache/cache.h"
#include "core/proxy.h"
#include "http/http.h"
#include "utils/log.h"
#include "utils/buffer.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <brotli/encode.h>
#include <brotli/decode.h>


// claude help meeeeeee :3 

/* Global cache list */
static cache_t *caches = NULL;
static pthread_rwlock_t caches_lock = PTHREAD_RWLOCK_INITIALIZER;

cache_t* cache_create(const char *name, uint32_t max_size, uint32_t max_object_size) {
    cache_t *cache = calloc(1, sizeof(*cache));
    if (!cache) return NULL;

    cache->name = strdup(name);
    cache->max_size = max_size;
    cache->max_object_size = max_object_size;
    cache->max_age = 3600;  /* Default 1 hour */
    cache->max_entries = max_size / 1024;  /* Rough estimate */

    /* Initialize hash table - use prime number for better distribution */
    cache->hash.size = 16381;  /* Prime number */
    cache->hash.mask = cache->hash.size - 1;
    cache->hash.table = calloc(cache->hash.size, sizeof(cache_entry_t*));
    cache->hash.locks = calloc(cache->hash.size, sizeof(pthread_spinlock_t));

    if (!cache->hash.table || !cache->hash.locks) {
        free(cache->hash.table);
        free((void*)cache->hash.locks);
        free(cache->name);
        free(cache);
        return NULL;
    }

    /* Initialize per-bucket locks for fine-grained concurrency */
    for (uint32_t i = 0; i < cache->hash.size; i++) {
        pthread_spin_init(&cache->hash.locks[i], PTHREAD_PROCESS_PRIVATE);
    }

    /* Initialize LRU list */
    cache->lru.head = cache->lru.tail = NULL;
    pthread_spin_init(&cache->lru.lock, PTHREAD_PROCESS_PRIVATE);

    pthread_rwlock_init(&cache->lock, NULL);

    /* Add to global list */
    pthread_rwlock_wrlock(&caches_lock);
    cache->next = caches;
    caches = cache;
    pthread_rwlock_unlock(&caches_lock);

    log_info("Cache '%s' created: max_size=%uMB, max_object=%uKB",
             name, max_size / (1024*1024), max_object_size / 1024);

    return cache;
}

/* Jenkins hash function for cache keys */
uint32_t cache_hash_key(const char *key) {
    uint32_t hash = 0;

    while (*key) {
        hash += *key++;
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }

    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);

    return hash;
}

char* cache_build_key(struct http_txn *txn) {
    if (!txn || !txn->uri) return NULL;

    /* Build cache key from method + host + uri + vary headers */
    char key[2048];
    int len = 0;

    /* Add method */
    len += snprintf(key + len, sizeof(key) - len, "%u:", txn->meth);

    /* Add host header if present */
    char *host = http_header_get(&txn->req, "Host");
    if (host) {
        len += snprintf(key + len, sizeof(key) - len, "%s:", host);
    }

    /* Add URI */
    len += snprintf(key + len, sizeof(key) - len, "%s", txn->uri);

    /* TODO: Add vary header values */

    return strdup(key);
}

cache_entry_t* cache_lookup(cache_t *cache, const char *key) {
    uint32_t hash = cache_hash_key(key);
    uint32_t idx = hash & cache->hash.mask;

    /* Per-bucket locking for better concurrency */
    pthread_spin_lock(&cache->hash.locks[idx]);

    cache_entry_t *entry = cache->hash.table[idx];
    while (entry) {
        if (entry->key_hash == hash && strcmp(entry->key, key) == 0) {
            /* Check if entry is still valid */
            time_t now = time(NULL);
            if (entry->expires > now) {
                entry->last_access = now;
                entry->access_count++;

                atomic_fetch_add(&cache->stats.hits, 1);
                pthread_spin_unlock(&cache->hash.locks[idx]);

                /* Update LRU position */
                cache_update_lru(cache, entry);

                return entry;
            } else {
                /* Entry expired */
                atomic_fetch_add(&cache->stats.misses, 1);
                pthread_spin_unlock(&cache->hash.locks[idx]);
                return NULL;
            }
        }
        entry = entry->hash_next;
    }

    atomic_fetch_add(&cache->stats.misses, 1);
    pthread_spin_unlock(&cache->hash.locks[idx]);
    return NULL;
}

// ok 

int cache_insert(cache_t *cache, const char *key, cache_entry_t *entry) {
    /* Check size limits */
    if (entry->size > cache->max_object_size) {
        log_debug("Object too large for cache: %u > %u", entry->size, cache->max_object_size);
        return -1;
    }

    /* Evict entries if cache is full */
    while (cache->current_size + entry->size > cache->max_size) {
        cache_evict_lru(cache);
    }

    uint32_t hash = cache_hash_key(key);
    uint32_t idx = hash & cache->hash.mask;

    entry->key = strdup(key);
    entry->key_hash = hash;
    entry->created = time(NULL);
    entry->last_access = entry->created;

    /* Set expiration based on Cache-Control headers */
    if (entry->flags & CACHE_F_MAX_AGE) {
        /* Use max-age from response */
    } else {
        entry->expires = entry->created + cache->max_age;
    }

    pthread_rwlock_init(&entry->lock, NULL);

    /* Insert into hash table */
    pthread_spin_lock(&cache->hash.locks[idx]);
    entry->hash_next = cache->hash.table[idx];
    cache->hash.table[idx] = entry;
    pthread_spin_unlock(&cache->hash.locks[idx]);

    /* Add to LRU list head (most recently used) */
    pthread_spin_lock(&cache->lru.lock);
    entry->lru_next = cache->lru.head;
    entry->lru_prev = NULL;
    if (cache->lru.head) {
        cache->lru.head->lru_prev = entry;
    }
    cache->lru.head = entry;
    if (!cache->lru.tail) {
        cache->lru.tail = entry;
    }
    pthread_spin_unlock(&cache->lru.lock);

    /* Update stats */
    cache->current_size += entry->size;
    cache->entry_count++;
    atomic_fetch_add(&cache->stats.inserts, 1);
    atomic_fetch_add(&cache->stats.bytes_in, entry->size);

    log_debug("Cached object: key=%s, size=%u, expires=%ld",
              key, entry->size, entry->expires);

    return 0;
}

void cache_update_lru(cache_t *cache, cache_entry_t *entry) {
    pthread_spin_lock(&cache->lru.lock);

    /* Move entry to head of LRU list (most recently used) */
    if (entry != cache->lru.head) {
        /* Remove from current position */
        if (entry->lru_prev) {
            entry->lru_prev->lru_next = entry->lru_next;
        }
        if (entry->lru_next) {
            entry->lru_next->lru_prev = entry->lru_prev;
        } else {
            /* Entry was tail */
            cache->lru.tail = entry->lru_prev;
        }

        /* Insert at head */
        entry->lru_prev = NULL;
        entry->lru_next = cache->lru.head;
        cache->lru.head->lru_prev = entry;
        cache->lru.head = entry;
    }

    pthread_spin_unlock(&cache->lru.lock);
}

void cache_evict_lru(cache_t *cache) {
    pthread_spin_lock(&cache->lru.lock);

    cache_entry_t *victim = cache->lru.tail;
    if (!victim) {
        pthread_spin_unlock(&cache->lru.lock);
        return;
    }

    /* Remove from LRU list */
    cache->lru.tail = victim->lru_prev;
    if (cache->lru.tail) {
        cache->lru.tail->lru_next = NULL;
    } else {
        cache->lru.head = NULL;
    }

    pthread_spin_unlock(&cache->lru.lock);

    /* Remove from hash table */
    uint32_t idx = victim->key_hash & cache->hash.mask;

    pthread_spin_lock(&cache->hash.locks[idx]);
    cache_entry_t **p = &cache->hash.table[idx];
    while (*p) {
        if (*p == victim) {
            *p = victim->hash_next;
            break;
        }
        p = &(*p)->hash_next;
    }
    pthread_spin_unlock(&cache->hash.locks[idx]);

    /* Update stats */
    cache->current_size -= victim->size;
    cache->entry_count--;
    atomic_fetch_add(&cache->stats.evictions, 1);

    log_debug("Evicted cache entry: key=%s, size=%u", victim->key, victim->size);

    /* Free entry */
    free(victim->key);
    free(victim->data.ptr);
    free(victim->etag);
    free(victim->vary);
    pthread_rwlock_destroy(&victim->lock);
    free(victim);
}

/* Check if request can be served from cache */
int cache_check_request(struct stream *s, struct channel *req, struct channel *res) {
    struct http_txn *txn = s->txn;
    struct proxy *px = s->fe;

    /* Only cache GET and HEAD requests */
    if (!(txn->meth & (HTTP_METH_GET | HTTP_METH_HEAD))) {
        return 0;
    }

    /* Check for no-cache directives */
    char *cache_control = http_header_get(&txn->req, "Cache-Control");
    if (cache_control) {
        if (strstr(cache_control, "no-cache") || strstr(cache_control, "no-store")) {
            return 0;
        }
    }

    /* Build cache key and lookup */
    char *key = cache_build_key(txn);
    if (!key) return 0;

    cache_t *cache = px->cache;
    if (!cache) {
        free(key);
        return 0;
    }

    cache_entry_t *entry = cache_lookup(cache, key);
    free(key);

    if (!entry) {
        return 0;  /* Cache miss - proceed to backend */
    }

    /* Check if entry needs revalidation */
    if (entry->etag || entry->last_modified) {
        /* Add conditional headers for revalidation */
        if (entry->etag) {
            http_header_add(&txn->req, "If-None-Match", entry->etag);
        }
        if (entry->last_modified) {
            char date[64];
            strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT",
                    gmtime(&entry->last_modified));
            http_header_add(&txn->req, "If-Modified-Since", date);
        }
    }

    /* Serve from cache */
    pthread_rwlock_rdlock(&entry->lock);

    /* Copy cached response to channel buffer */
    buffer_put(&res->buf, entry->data.ptr, entry->data.len);

    /* Update stats */
    atomic_fetch_add(&cache->stats.bytes_out, entry->data.len);

    pthread_rwlock_unlock(&entry->lock);

    return 1;  /* Request served from cache */
}

/* Store response in cache if cacheable */
int cache_store_response(struct stream *s, struct channel *res) {
    struct http_txn *txn = s->txn;
    struct proxy *px = s->be ? s->be : s->fe;
    cache_t *cache = px->cache;

    if (!cache) return 0;

    /* Check if response is cacheable */
    if (txn->status < 200 || txn->status >= 300) {
        return 0;  /* Only cache 2xx responses */
    }

    /* Check Cache-Control headers */
    char *cache_control = http_header_get(&txn->rsp, "Cache-Control");
    if (cache_control) {
        if (strstr(cache_control, "no-cache") ||
            strstr(cache_control, "no-store") ||
            strstr(cache_control, "private")) {
            return 0;  /* Not cacheable */
        }
    }

    /* Build cache key */
    char *key = cache_build_key(txn);
    if (!key) return 0;

    /* Create cache entry */
    cache_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        free(key);
        return 0;
    }

    /* Copy response data */
    entry->data.len = res->buf.data;
    entry->data.alloc = entry->data.len;
    entry->data.ptr = malloc(entry->data.len);
    if (!entry->data.ptr) {
        free(entry);
        free(key);
        return 0;
    }

    memcpy(entry->data.ptr, res->buf.area + res->buf.head, entry->data.len);
    entry->size = entry->data.len;

    /* Store response metadata */
    entry->response.status = txn->status;

    /* Parse cache-related headers */
    char *etag = http_header_get(&txn->rsp, "ETag");
    if (etag) {
        entry->etag = strdup(etag);
    }

    char *last_modified = http_header_get(&txn->rsp, "Last-Modified");
    if (last_modified) {
        struct tm tm;
        if (strptime(last_modified, "%a, %d %b %Y %H:%M:%S GMT", &tm)) {
            entry->last_modified = mktime(&tm);
        }
    }

    char *vary = http_header_get(&txn->rsp, "Vary");
    if (vary) {
        entry->vary = strdup(vary);
    }

    /* Set cache flags based on headers */
    if (cache_control) {
        if (strstr(cache_control, "must-revalidate")) {
            entry->flags |= CACHE_F_MUST_REVALIDATE;
        }

        char *max_age = strstr(cache_control, "max-age=");
        if (max_age) {
            entry->flags |= CACHE_F_MAX_AGE;
            int age = atoi(max_age + 8);
            entry->expires = time(NULL) + age;
        }
    }

    /* Insert into cache */
    int ret = cache_insert(cache, key, entry);
    free(key);

    if (ret < 0) {
        /* Failed to cache - cleanup */
        free(entry->data.ptr);
        free(entry->etag);
        free(entry->vary);
        free(entry);
        return 0;
    }

    return 1;
}

/* Initialize compression context */
int compression_init(compression_ctx_t *ctx, int type, int level) {
    ctx->type = type;
    ctx->level = level;

    switch (type) {
        case COMP_TYPE_GZIP:
        case COMP_TYPE_DEFLATE:
            memset(&ctx->zstrm, 0, sizeof(ctx->zstrm));

            int wbits = (type == COMP_TYPE_GZIP) ? 15 + 16 : 15;

            if (deflateInit2(&ctx->zstrm, level, Z_DEFLATED, wbits,
                            8, Z_DEFAULT_STRATEGY) != Z_OK) {
                return -1;
            }
            break;

        case COMP_TYPE_BROTLI:
            ctx->brotli_state = BrotliEncoderCreateInstance(NULL, NULL, NULL);
            if (!ctx->brotli_state) {
                return -1;
            }

            BrotliEncoderSetParameter(ctx->brotli_state,
                                     BROTLI_PARAM_QUALITY, level);
            break;

        default:
            return -1;
    }

    return 0;
}

/* Compress data */
int compression_process(compression_ctx_t *ctx, struct buffer *in, struct buffer *out, int flags) {
    int ret = 0;

    switch (ctx->type) {
        case COMP_TYPE_GZIP:
        case COMP_TYPE_DEFLATE:
            ctx->zstrm.next_in = (Bytef*)(in->area + in->head);
            ctx->zstrm.avail_in = in->data;
            ctx->zstrm.next_out = (Bytef*)(out->area + out->head + out->data);
            ctx->zstrm.avail_out = out->size - out->data;

            int flush = (flags & COMP_FINISH) ? Z_FINISH : Z_NO_FLUSH;
            ret = deflate(&ctx->zstrm, flush);

            if (ret == Z_STREAM_ERROR) {
                return -1;
            }

            ctx->consumed = in->data - ctx->zstrm.avail_in;
            ctx->produced = (out->size - out->data) - ctx->zstrm.avail_out;

            in->data -= ctx->consumed;
            in->head += ctx->consumed;
            out->data += ctx->produced;
            break;

        case COMP_TYPE_BROTLI:
            {
                size_t available_in = in->data;
                const uint8_t *next_in = (uint8_t*)(in->area + in->head);
                size_t available_out = out->size - out->data;
                uint8_t *next_out = (uint8_t*)(out->area + out->head + out->data);

                BrotliEncoderOperation op = (flags & COMP_FINISH) ?
                    BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;

                if (!BrotliEncoderCompressStream(ctx->brotli_state, op,
                                                &available_in, &next_in,
                                                &available_out, &next_out,
                                                NULL)) {
                    return -1;
                }

                ctx->consumed = in->data - available_in;
                ctx->produced = (out->size - out->data) - available_out;

                in->data = available_in;
                in->head = (char*)next_in - in->area;
                out->data += ctx->produced;
            }
            break;
    }

    return (flags & COMP_FINISH && ret == Z_STREAM_END) ? 1 : 0;
}

/* End compression */
void compression_end(compression_ctx_t *ctx) {
    switch (ctx->type) {
        case COMP_TYPE_GZIP:
        case COMP_TYPE_DEFLATE:
            deflateEnd(&ctx->zstrm);
            break;

        case COMP_TYPE_BROTLI:
            if (ctx->brotli_state) {
                BrotliEncoderDestroyInstance(ctx->brotli_state);
                ctx->brotli_state = NULL;
            }
            break;
    }
}