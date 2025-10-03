#include "core/loadbalancer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>

#define ALIGN_SIZE(size) (((size) + 7) & ~7)

void* memory_pool_alloc(memory_pool_t* pool, size_t size) {
    size = ALIGN_SIZE(size);

    pthread_spin_lock(&pool->lock);

    if (pool->free_list) {
        memory_chunk_t* prev = NULL;
        memory_chunk_t* chunk = pool->free_list;

        while (chunk) {
            if (chunk->size >= size) {
                if (prev) {
                    prev->next = chunk->next;
                } else {
                    pool->free_list = chunk->next;
                }

                pthread_spin_unlock(&pool->lock);
                return chunk;
            }
            prev = chunk;
            chunk = chunk->next;
        }
    }

    if (pool->used + size > pool->size) {
        pthread_spin_unlock(&pool->lock);
        return NULL;
    }

    void* ptr = (char*)pool->base + pool->used;
    pool->used += size;

    pthread_spin_unlock(&pool->lock);
    return ptr;
}

void memory_pool_free(memory_pool_t* pool, void* ptr, size_t size) {
    if (!ptr) return;

    size = ALIGN_SIZE(size);

    memory_chunk_t* chunk = (memory_chunk_t*)ptr;
    chunk->size = size;

    pthread_spin_lock(&pool->lock);
    chunk->next = pool->free_list;
    pool->free_list = chunk;
    pthread_spin_unlock(&pool->lock);
}

typedef struct hash_node {
    uint64_t hash;
    backend_t* backend;
    struct hash_node* next;
} hash_node_t;

typedef struct consistent_hash {
    hash_node_t** table;
    uint32_t size;
    uint32_t virtual_nodes;
    pthread_spinlock_t lock;
} consistent_hash_t;

uint64_t murmur3_64(const void* key, size_t len, uint64_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;

    uint64_t h = seed ^ (len * m);

    const uint64_t* data = (const uint64_t*)key;
    const uint64_t* end = data + (len / 8);

    while (data != end) {
        uint64_t k = *data++;

        k *= m;
        k ^= k >> r;
        k *= m;

        h ^= k;
        h *= m;
    }

    const unsigned char* data2 = (const unsigned char*)data;

    switch (len & 7) {
        case 7: h ^= ((uint64_t)data2[6]) << 48;
        case 6: h ^= ((uint64_t)data2[5]) << 40;
        case 5: h ^= ((uint64_t)data2[4]) << 32;
        case 4: h ^= ((uint64_t)data2[3]) << 24;
        case 3: h ^= ((uint64_t)data2[2]) << 16;
        case 2: h ^= ((uint64_t)data2[1]) << 8;
        case 1: h ^= ((uint64_t)data2[0]);
                h *= m;
    };

    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
}

consistent_hash_t* consistent_hash_create(uint32_t size, uint32_t virtual_nodes) {
    consistent_hash_t* ch = calloc(1, sizeof(consistent_hash_t));
    if (!ch) return NULL;

    ch->size = size;
    ch->virtual_nodes = virtual_nodes;
    ch->table = calloc(size, sizeof(hash_node_t*));
    if (!ch->table) {
        free(ch);
        return NULL;
    }

    pthread_spin_init(&ch->lock, PTHREAD_PROCESS_PRIVATE);

    return ch;
}

void consistent_hash_add(consistent_hash_t* ch, backend_t* backend) {
    char key[512];

    pthread_spin_lock(&ch->lock);

    for (uint32_t i = 0; i < ch->virtual_nodes; i++) {
        snprintf(key, sizeof(key), "%s:%u#%u", backend->host, backend->port, i);
        uint64_t hash = murmur3_64(key, strlen(key), 0);

        uint32_t idx = hash % ch->size;

        hash_node_t* node = malloc(sizeof(hash_node_t));
        node->hash = hash;
        node->backend = backend;
        node->next = ch->table[idx];
        ch->table[idx] = node;
    }

    pthread_spin_unlock(&ch->lock);
}

backend_t* consistent_hash_get(consistent_hash_t* ch, const char* key) {
    uint64_t hash = murmur3_64(key, strlen(key), 0);
    uint32_t idx = hash % ch->size;

    pthread_spin_lock(&ch->lock);

    hash_node_t* node = ch->table[idx];
    backend_t* result = NULL;
    uint64_t min_distance = UINT64_MAX;

    while (node) {
        if (atomic_load(&node->backend->state) == BACKEND_UP) {
            uint64_t distance = (node->hash > hash) ?
                (node->hash - hash) : (UINT64_MAX - hash + node->hash);

            if (distance < min_distance) {
                min_distance = distance;
                result = node->backend;
            }
        }
        node = node->next;
    }

    if (!result) {
        for (uint32_t i = 0; i < ch->size; i++) {
            node = ch->table[i];
            while (node) {
                if (atomic_load(&node->backend->state) == BACKEND_UP) {
                    result = node->backend;
                    break;
                }
                node = node->next;
            }
            if (result) break;
        }
    }

    pthread_spin_unlock(&ch->lock);

    return result;
}

void consistent_hash_destroy(consistent_hash_t* ch) {
    if (!ch) return;

    for (uint32_t i = 0; i < ch->size; i++) {
        hash_node_t* node = ch->table[i];
        while (node) {
            hash_node_t* next = node->next;
            free(node);
            node = next;
        }
    }

    free(ch->table);
    pthread_spin_destroy(&ch->lock);
    free(ch);
}