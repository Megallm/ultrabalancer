#include "stick_tables.h"
#include "core/proxy.h"
#include "utils/log.h"
#include <stdlib.h>
#include <string.h>

/* Global list of stick tables */
static stick_table_t *stick_tables = NULL;
static pthread_rwlock_t stick_tables_lock = PTHREAD_RWLOCK_INITIALIZER;

stick_table_t* stktable_new(const char *id, int type, uint32_t size, uint32_t expire) {
    stick_table_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;

    t->id = strdup(id);
    t->type = type;
    t->size = size;
    t->expire = expire;
    t->current = 0;

    /* Initialize hash table with prime size for better distribution */
    t->hash.size = size | 1;  // Make it odd
    t->hash.entries = calloc(t->hash.size, sizeof(stick_entry_t*));
    t->hash.locks = calloc(t->hash.size, sizeof(pthread_spinlock_t));

    if (!t->hash.entries || !t->hash.locks) {
        free(t->hash.entries);
        free(t->hash.locks);
        free(t->id);
        free(t);
        return NULL;
    }

    /* Initialize per-bucket spinlocks for fine-grained locking */
    for (uint32_t i = 0; i < t->hash.size; i++) {
        pthread_spin_init(&t->hash.locks[i], PTHREAD_PROCESS_PRIVATE);
    }

    t->keys = EB_ROOT;
    LIST_INIT(&t->lru);
    pthread_rwlock_init(&t->lock, NULL);

    /* Add to global list */
    pthread_rwlock_wrlock(&stick_tables_lock);
    t->next = stick_tables;
    stick_tables = t;
    pthread_rwlock_unlock(&stick_tables_lock);

    return t;
}

/* Hash function for stick table keys - uses MurmurHash3 algorithm */
static uint32_t stktable_hash(stick_key_t *key) {
    uint32_t hash = 0x12345678;
    const uint8_t *data = NULL;
    size_t len = 0;

    switch (key->type) {
        case STKTABLE_TYPE_IP:
            data = (uint8_t*)&key->data.ipv4;
            len = sizeof(key->data.ipv4);
            break;
        case STKTABLE_TYPE_IPV6:
            data = (uint8_t*)&key->data.ipv6;
            len = sizeof(key->data.ipv6);
            break;
        case STKTABLE_TYPE_INTEGER:
            data = (uint8_t*)&key->data.integer;
            len = sizeof(key->data.integer);
            break;
        case STKTABLE_TYPE_STRING:
            data = (uint8_t*)key->data.str.ptr;
            len = key->data.str.len;
            break;
        case STKTABLE_TYPE_BINARY:
            data = (uint8_t*)key->data.bin.ptr;
            len = key->data.bin.len;
            break;
    }

    /* Simple hash for now - replace with MurmurHash3 for production */
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + data[i];
    }

    return hash;
}

stick_entry_t* stktable_lookup(stick_table_t *t, stick_key_t *key) {
    uint32_t hash = stktable_hash(key);
    uint32_t idx = hash % t->hash.size;

    /* Use per-bucket locking for better concurrency */
    pthread_spin_lock(&t->hash.locks[idx]);

    stick_entry_t *entry = t->hash.entries[idx];
    while (entry) {
        if (entry->key.type == key->type) {
            bool match = false;

            switch (key->type) {
                case STKTABLE_TYPE_IP:
                    match = memcmp(&entry->key.data.ipv4, &key->data.ipv4,
                                  sizeof(struct in_addr)) == 0;
                    break;
                case STKTABLE_TYPE_IPV6:
                    match = memcmp(&entry->key.data.ipv6, &key->data.ipv6,
                                  sizeof(struct in6_addr)) == 0;
                    break;
                case STKTABLE_TYPE_INTEGER:
                    match = entry->key.data.integer == key->data.integer;
                    break;
                case STKTABLE_TYPE_STRING:
                    match = entry->key.data.str.len == key->data.str.len &&
                           memcmp(entry->key.data.str.ptr, key->data.str.ptr,
                                 key->data.str.len) == 0;
                    break;
            }

            if (match) {
                /* Update LRU and access time */
                entry->last_access = time(NULL);
                atomic_fetch_add(&t->stats.hits, 1);
                pthread_spin_unlock(&t->hash.locks[idx]);
                return entry;
            }
        }
        entry = entry->node.node_p;
    }

    atomic_fetch_add(&t->stats.misses, 1);
    pthread_spin_unlock(&t->hash.locks[idx]);
    return NULL;
}

stick_entry_t* stktable_get(stick_table_t *t, stick_key_t *key) {
    stick_entry_t *entry = stktable_lookup(t, key);
    if (entry) return entry;

    /* Check if table is full - evict LRU entry if needed */
    if (t->current >= t->size) {
        stktable_expire(t);

        if (t->current >= t->size) {
            /* Still full - evict LRU entry */
            pthread_rwlock_wrlock(&t->lock);

            stick_entry_t *lru = LIST_ELEM(t->lru.p, stick_entry_t *, list);
            if (lru && lru->ref_cnt == 0) {
                uint32_t hash = stktable_hash(&lru->key);
                uint32_t idx = hash % t->hash.size;

                pthread_spin_lock(&t->hash.locks[idx]);
                /* Remove from hash table */
                stick_entry_t **p = &t->hash.entries[idx];
                while (*p) {
                    if (*p == lru) {
                        *p = lru->node.node_p;
                        break;
                    }
                    p = &(*p)->node.node_p;
                }
                pthread_spin_unlock(&t->hash.locks[idx]);

                LIST_DEL(&lru->list);
                pthread_rwlock_destroy(&lru->lock);

                /* Free key data if needed */
                if (lru->key.type == STKTABLE_TYPE_STRING && lru->key.data.str.ptr) {
                    free(lru->key.data.str.ptr);
                }

                free(lru);
                t->current--;
                atomic_fetch_add(&t->stats.expires, 1);
            }

            pthread_rwlock_unlock(&t->lock);
        }
    }

    /* Create new entry */
    entry = calloc(1, sizeof(*entry));
    if (!entry) return NULL;

    /* Copy key */
    entry->key = *key;
    if (key->type == STKTABLE_TYPE_STRING) {
        entry->key.data.str.ptr = malloc(key->data.str.len);
        if (!entry->key.data.str.ptr) {
            free(entry);
            return NULL;
        }
        memcpy(entry->key.data.str.ptr, key->data.str.ptr, key->data.str.len);
    }

    entry->expire = time(NULL) + t->expire;
    entry->last_access = time(NULL);
    pthread_rwlock_init(&entry->lock, NULL);

    /* Insert into hash table */
    uint32_t hash = stktable_hash(key);
    uint32_t idx = hash % t->hash.size;

    pthread_spin_lock(&t->hash.locks[idx]);
    entry->node.node_p = t->hash.entries[idx];
    t->hash.entries[idx] = entry;
    pthread_spin_unlock(&t->hash.locks[idx]);

    /* Add to LRU list */
    pthread_rwlock_wrlock(&t->lock);
    LIST_ADDQ(&t->lru, &entry->list);
    t->current++;
    pthread_rwlock_unlock(&t->lock);

    atomic_fetch_add(&t->stats.inserts, 1);

    return entry;
}

void stktable_touch(stick_table_t *t, stick_entry_t *entry) {
    /* Update access time and move to head of LRU */
    entry->last_access = time(NULL);

    pthread_rwlock_wrlock(&t->lock);
    LIST_DEL(&entry->list);
    LIST_ADD(&t->lru, &entry->list);
    pthread_rwlock_unlock(&t->lock);
}

void stktable_expire(stick_table_t *t) {
    time_t now = time(NULL);
    stick_entry_t *entry, *next;

    pthread_rwlock_wrlock(&t->lock);

    /* Walk LRU list from tail (oldest entries) */
    list_for_each_entry_safe_rev(entry, next, &t->lru, list) {
        if (entry->expire > now && entry->ref_cnt > 0)
            break;  /* Still valid or in use */

        if (entry->ref_cnt > 0)
            continue;  /* Skip if referenced */

        /* Remove from hash table */
        uint32_t hash = stktable_hash(&entry->key);
        uint32_t idx = hash % t->hash.size;

        pthread_spin_lock(&t->hash.locks[idx]);
        stick_entry_t **p = &t->hash.entries[idx];
        while (*p) {
            if (*p == entry) {
                *p = entry->node.node_p;
                break;
            }
            p = &(*p)->node.node_p;
        }
        pthread_spin_unlock(&t->hash.locks[idx]);

        /* Remove from LRU list */
        LIST_DEL(&entry->list);

        /* Free entry */
        if (entry->key.type == STKTABLE_TYPE_STRING && entry->key.data.str.ptr) {
            free(entry->key.data.str.ptr);
        }
        pthread_rwlock_destroy(&entry->lock);
        free(entry);

        t->current--;
        atomic_fetch_add(&t->stats.expires, 1);
    }

    pthread_rwlock_unlock(&t->lock);
}

int stktable_update_key(stick_table_t *t, stick_key_t *key, int data_type, void *value) {
    stick_entry_t *entry = stktable_get(t, key);
    if (!entry) return -1;

    pthread_rwlock_wrlock(&entry->lock);

    switch (data_type) {
        case STKTABLE_DATA_CONN_CNT:
            atomic_store(&entry->counters.conn_cnt, *(uint32_t*)value);
            break;
        case STKTABLE_DATA_CONN_CUR:
            atomic_store(&entry->counters.conn_cur, *(uint32_t*)value);
            break;
        case STKTABLE_DATA_SESS_CNT:
            atomic_store(&entry->counters.sess_cnt, *(uint32_t*)value);
            break;
        case STKTABLE_DATA_HTTP_REQ_CNT:
            atomic_store(&entry->counters.http_req_cnt, *(uint32_t*)value);
            break;
        case STKTABLE_DATA_BYTES_IN:
            atomic_store(&entry->counters.bytes_in, *(uint64_t*)value);
            break;
        case STKTABLE_DATA_BYTES_OUT:
            atomic_store(&entry->counters.bytes_out, *(uint64_t*)value);
            break;
        case STKTABLE_DATA_SERVER_ID:
            atomic_store(&entry->counters.server_id, *(uint32_t*)value);
            break;
    }

    pthread_rwlock_unlock(&entry->lock);
    atomic_fetch_add(&t->stats.updates, 1);

    return 0;
}

/* Track a session with stick table */
int stksess_track(struct session *sess, stick_table_t *t, stick_key_t *key) {
    stick_entry_t *entry = stktable_get(t, key);
    if (!entry) return -1;

    /* Increment reference count to prevent eviction */
    entry->ref_cnt++;

    /* Update counters */
    atomic_fetch_add(&entry->counters.conn_cnt, 1);
    atomic_fetch_add(&entry->counters.conn_cur, 1);
    atomic_fetch_add(&entry->counters.sess_cnt, 1);

    /* Store in session for later use */
    if (sess->stkctr) {
        sess->stkctr->entry = entry;
        sess->stkctr->table = t;
    }

    return 0;
}

/* Get stored server for sticky session */
struct server* stksess_get_server(struct session *sess, stick_table_t *t) {
    if (!sess->stkctr || sess->stkctr->table != t)
        return NULL;

    stick_entry_t *entry = sess->stkctr->entry;
    if (!entry) return NULL;

    uint32_t server_id = atomic_load(&entry->counters.server_id);
    if (server_id == 0) return NULL;

    /* Find server by ID - implement server lookup */
    return sess->stkctr->server;
}

void stick_tables_init() {
    /* Initialize global structures */
    pthread_rwlock_init(&stick_tables_lock, NULL);
    log_info("Stick tables initialized");
}

void stick_tables_deinit() {
    pthread_rwlock_wrlock(&stick_tables_lock);

    stick_table_t *t = stick_tables;
    while (t) {
        stick_table_t *next = t->next;
        stktable_free(t);
        t = next;
    }

    stick_tables = NULL;
    pthread_rwlock_unlock(&stick_tables_lock);
    pthread_rwlock_destroy(&stick_tables_lock);
}

void stktable_free(stick_table_t *t) {
    if (!t) return;

    /* Free all entries */
    for (uint32_t i = 0; i < t->hash.size; i++) {
        stick_entry_t *entry = t->hash.entries[i];
        while (entry) {
            stick_entry_t *next = entry->node.node_p;

            if (entry->key.type == STKTABLE_TYPE_STRING && entry->key.data.str.ptr) {
                free(entry->key.data.str.ptr);
            }

            pthread_rwlock_destroy(&entry->lock);
            free(entry);
            entry = next;
        }

        pthread_spin_destroy(&t->hash.locks[i]);
    }

    free(t->hash.entries);
    free(t->hash.locks);
    free(t->id);
    pthread_rwlock_destroy(&t->lock);
    free(t);
}