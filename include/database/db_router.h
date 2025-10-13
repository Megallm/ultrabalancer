#ifndef DB_ROUTER_H
#define DB_ROUTER_H

#include "db_protocol.h"
#include "db_pool.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint64_t session_id;
    uint64_t backend_id;
    bool in_transaction;
    time_t last_activity;
} db_session_t;

typedef struct db_router_t {
    db_pool_t* pool;
    db_session_t* sessions;
    uint32_t session_count;
    uint32_t max_sessions;
    void* mutex;
} db_router_t;

db_router_t* db_router_create(db_pool_t* pool, uint32_t max_sessions);
void db_router_destroy(db_router_t* router);

db_connection_t* db_router_route_query(db_router_t* router,
                                        const uint8_t* query_data,
                                        size_t query_length,
                                        uint64_t client_session_id);

void db_router_end_session(db_router_t* router, uint64_t client_session_id);

int db_router_get_stats(db_router_t* router, char* buffer, size_t buffer_size);

#endif
