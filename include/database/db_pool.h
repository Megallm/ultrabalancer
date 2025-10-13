#ifndef DB_POOL_H
#define DB_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "db_protocol.h"

typedef enum {
    DB_BACKEND_UNKNOWN = 0,
    DB_BACKEND_PRIMARY,
    DB_BACKEND_REPLICA,
    DB_BACKEND_DOWN
} db_backend_role_t;

typedef struct db_connection_t {
    int fd;
    db_protocol_type_t protocol;
    db_backend_role_t backend_role;
    bool in_use;
    bool in_transaction;
    time_t created_at;
    time_t last_used;
    uint32_t query_count;
    uint64_t backend_id;
    struct db_connection_t* next;
} db_connection_t;

typedef struct db_backend_t {
    uint64_t id;
    char host[256];
    uint16_t port;
    db_backend_role_t role;
    db_protocol_type_t protocol;
    bool is_healthy;
    uint32_t active_connections;
    uint32_t total_connections;
    uint32_t max_connections;
    uint64_t replication_lag_ms;
    time_t last_health_check;
    struct db_backend_t* next;
} db_backend_t;

typedef struct {
    db_connection_t* idle_connections;
    db_connection_t* active_connections;
    db_backend_t* backends;
    uint32_t total_idle;
    uint32_t total_active;
    uint32_t max_connections;
    uint32_t min_idle;
    uint32_t max_idle;
    uint32_t max_lifetime_seconds;
    uint32_t idle_timeout_seconds;
    bool initialized;
    void* mutex;
} db_pool_t;

db_pool_t* db_pool_create(uint32_t max_connections, uint32_t min_idle, uint32_t max_idle);
void db_pool_destroy(db_pool_t* pool);

int db_pool_add_backend(db_pool_t* pool, const char* host, uint16_t port,
                        db_backend_role_t role, db_protocol_type_t protocol);

db_connection_t* db_pool_acquire(db_pool_t* pool, db_query_type_t query_type,
                                  bool in_transaction, uint64_t session_backend_id);

void db_pool_release(db_pool_t* pool, db_connection_t* conn);

db_backend_t* db_pool_select_backend(db_pool_t* pool, db_query_type_t query_type);

int db_pool_validate_connection(db_connection_t* conn);

void db_pool_cleanup_idle(db_pool_t* pool);

int db_pool_get_stats(db_pool_t* pool, char* buffer, size_t buffer_size);

#endif
