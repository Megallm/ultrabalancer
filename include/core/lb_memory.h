#ifndef LB_MEMORY_H
#define LB_MEMORY_H

#include "lb_types.h"
#include <stddef.h>

typedef struct memory_chunk {
    size_t size;
    struct memory_chunk* next;
} memory_chunk_t;

typedef struct memory_pool {
    void* base;
    size_t size;
    size_t used;
    pthread_spinlock_t lock;
    struct memory_chunk* free_list;
} memory_pool_t;

memory_pool_t* memory_pool_create(size_t size);
void memory_pool_destroy(memory_pool_t* pool);
void* memory_pool_alloc(memory_pool_t* pool, size_t size);
void memory_pool_free(memory_pool_t* pool, void* ptr, size_t size);

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

consistent_hash_t* consistent_hash_create(uint32_t size, uint32_t virtual_nodes);
void consistent_hash_add(consistent_hash_t* ch, backend_t* backend);
backend_t* consistent_hash_get(consistent_hash_t* ch, const char* key);
void consistent_hash_destroy(consistent_hash_t* ch);

#endif