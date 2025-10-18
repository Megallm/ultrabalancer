#include "database/db_router.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>

db_router_t* db_router_create(db_pool_t* pool, uint32_t max_sessions) {
    if (!pool) return NULL;

    db_router_t* router = (db_router_t*)calloc(1, sizeof(db_router_t));
    if (!router) return NULL;

    router->pool = pool;
    router->max_sessions = max_sessions;
    router->sessions = (db_session_t*)calloc(max_sessions, sizeof(db_session_t));

    if (!router->sessions) {
        free(router);
        return NULL;
    }

    pthread_mutex_t* mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    if (!mutex) {
        free(router->sessions);
        free(router);
        return NULL;
    }
    pthread_mutex_init(mutex, NULL);
    router->mutex = mutex;

    return router;
}

void db_router_destroy(db_router_t* router) {
    if (!router) return;

    pthread_mutex_t* mutex = (pthread_mutex_t*)router->mutex;
    pthread_mutex_destroy(mutex);
    free(mutex);
    free(router->sessions);
    free(router);
}

static db_session_t* db_router_find_session(db_router_t* router, uint64_t session_id) {
    for (uint32_t i = 0; i < router->session_count; i++) {
        if (router->sessions[i].session_id == session_id) {
            return &router->sessions[i];
        }
    }
    return NULL;
}

static db_session_t* db_router_create_session(db_router_t* router, uint64_t session_id) {
    if (router->session_count >= router->max_sessions) {
        time_t oldest_time = 0;
        uint32_t oldest_idx = 0;
        bool found_non_transaction = false;

        // Find the first non-transaction session to initialize baseline
        for (uint32_t i = 0; i < router->session_count; i++) {
            if (!router->sessions[i].in_transaction) {
                oldest_time = router->sessions[i].last_activity;
                oldest_idx = i;
                found_non_transaction = true;
                break;
            }
        }

        // If we found a non-transaction session, continue to find the oldest one
        if (found_non_transaction) {
            for (uint32_t i = oldest_idx + 1; i < router->session_count; i++) {
                if (!router->sessions[i].in_transaction &&
                    router->sessions[i].last_activity < oldest_time) {
                    oldest_time = router->sessions[i].last_activity;
                    oldest_idx = i;
                }
            }

            // Always evict oldest session when at max capacity
            router->sessions[oldest_idx].session_id = session_id;
            router->sessions[oldest_idx].backend_id = 0;
            router->sessions[oldest_idx].in_transaction = false;
            router->sessions[oldest_idx].last_activity = time(NULL);
            return &router->sessions[oldest_idx];
        }

        return NULL;  // All sessions in transaction
    }

    db_session_t* session = &router->sessions[router->session_count++];
    session->session_id = session_id;
    session->backend_id = 0;
    session->in_transaction = false;
    session->last_activity = time(NULL);

    return session;
}

db_connection_t* db_router_route_query(db_router_t* router,
                                        const uint8_t* query_data,
                                        size_t query_length,
                                        uint64_t client_session_id) {
    if (!router || !query_data || query_length == 0) return NULL;

    db_query_info_t query_info = {0};

    db_protocol_type_t protocol = db_protocol_detect(query_data, query_length);

    switch (protocol) {
        case DB_PROTOCOL_POSTGRESQL:
            db_protocol_parse_postgresql(query_data, query_length, &query_info);
            break;
        case DB_PROTOCOL_MYSQL:
            db_protocol_parse_mysql(query_data, query_length, &query_info);
            break;
        case DB_PROTOCOL_REDIS:
            db_protocol_parse_redis(query_data, query_length, &query_info);
            break;
        default:
            return NULL;
    }

    pthread_mutex_t* mutex = (pthread_mutex_t*)router->mutex;
    pthread_mutex_lock(mutex);

    db_session_t* session = db_router_find_session(router, client_session_id);

    if (!session && (query_info.requires_sticky ||
                     query_info.query_type == DB_QUERY_TRANSACTION_BEGIN)) {
        session = db_router_create_session(router, client_session_id);
    }

    uint64_t backend_id = 0;
    bool in_transaction = false;

    if (session) {
        if (query_info.query_type == DB_QUERY_TRANSACTION_BEGIN) {
            session->in_transaction = true;
            in_transaction = true;
        } else if (query_info.query_type == DB_QUERY_TRANSACTION_END) {
            session->in_transaction = false;
            session->backend_id = 0;
        }

        if (session->in_transaction || query_info.requires_sticky) {
            backend_id = session->backend_id;
            in_transaction = session->in_transaction;
        }

        session->last_activity = time(NULL);
    }

    // Keep mutex locked while acquiring connection to prevent race condition
    db_connection_t* conn = db_pool_acquire(router->pool, query_info.query_type,
                                             in_transaction, backend_id);

    // Update session backend_id if it was just created
    if (conn && session && session->backend_id == 0) {
        session->backend_id = conn->backend_id;
    }

    pthread_mutex_unlock(mutex);

    return conn;
}

void db_router_end_session(db_router_t* router, uint64_t client_session_id) {
    if (!router) return;

    pthread_mutex_t* mutex = (pthread_mutex_t*)router->mutex;
    pthread_mutex_lock(mutex);

    for (uint32_t i = 0; i < router->session_count; i++) {
        if (router->sessions[i].session_id == client_session_id) {
            if (i < router->session_count - 1) {
                memmove(&router->sessions[i], &router->sessions[i + 1],
                        (router->session_count - i - 1) * sizeof(db_session_t));
            }
            router->session_count--;
            break;
        }
    }

    pthread_mutex_unlock(mutex);
}

int db_router_get_stats(db_router_t* router, char* buffer, size_t buffer_size) {
    if (!router || !buffer) return -1;

    pthread_mutex_t* mutex = (pthread_mutex_t*)router->mutex;
    pthread_mutex_lock(mutex);

    int written = snprintf(buffer, buffer_size,
        "{\"session_count\":%u,\"max_sessions\":%u,\"sessions\":[",
        router->session_count, router->max_sessions);

    for (uint32_t i = 0; i < router->session_count && written < (int)buffer_size; i++) {
        if (i > 0) {
            written += snprintf(buffer + written, buffer_size - written, ",");
        }
        written += snprintf(buffer + written, buffer_size - written,
            "{\"session_id\":%lu,\"backend_id\":%lu,\"in_transaction\":%s}",
            (unsigned long)router->sessions[i].session_id,
            (unsigned long)router->sessions[i].backend_id,
            router->sessions[i].in_transaction ? "true" : "false");
    }

    written += snprintf(buffer + written, buffer_size - written, "]}");

    pthread_mutex_unlock(mutex);
    return written;
}
