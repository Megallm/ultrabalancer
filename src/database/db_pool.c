#include "database/db_pool.h"
#include "database/db_protocol.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>

db_pool_t* db_pool_create(uint32_t max_connections, uint32_t min_idle, uint32_t max_idle) {
    db_pool_t* pool = (db_pool_t*)calloc(1, sizeof(db_pool_t));
    if (!pool) return NULL;

    pool->max_connections = max_connections;
    pool->min_idle = min_idle;
    pool->max_idle = max_idle;
    pool->max_lifetime_seconds = 3600;
    pool->idle_timeout_seconds = 300;
    pool->initialized = true;

    pthread_mutex_t* mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    if (!mutex) {
        free(pool);
        return NULL;
    }
    pthread_mutex_init(mutex, NULL);
    pool->mutex = mutex;

    return pool;
}

void db_pool_destroy(db_pool_t* pool) {
    if (!pool) return;

    pthread_mutex_t* mutex = (pthread_mutex_t*)pool->mutex;
    pthread_mutex_lock(mutex);

    db_connection_t* conn = pool->idle_connections;
    while (conn) {
        db_connection_t* next = conn->next;
        if (conn->fd >= 0) close(conn->fd);
        free(conn);
        conn = next;
    }

    conn = pool->active_connections;
    while (conn) {
        db_connection_t* next = conn->next;
        if (conn->fd >= 0) close(conn->fd);
        free(conn);
        conn = next;
    }

    db_backend_t* backend = pool->backends;
    while (backend) {
        db_backend_t* next = backend->next;
        free(backend);
        backend = next;
    }

    pthread_mutex_unlock(mutex);
    pthread_mutex_destroy(mutex);
    free(mutex);
    free(pool);
}

int db_pool_add_backend(db_pool_t* pool, const char* host, uint16_t port,
                        db_backend_role_t role, db_protocol_type_t protocol) {
    if (!pool || !host) return -1;

    pthread_mutex_t* mutex = (pthread_mutex_t*)pool->mutex;
    pthread_mutex_lock(mutex);

    db_backend_t* backend = (db_backend_t*)calloc(1, sizeof(db_backend_t));
    if (!backend) {
        pthread_mutex_unlock(mutex);
        return -1;
    }

    static uint64_t next_id = 1;
    backend->id = next_id++;
    strncpy(backend->host, host, sizeof(backend->host) - 1);
    backend->port = port;
    backend->role = role;
    backend->protocol = protocol;
    backend->is_healthy = true;
    backend->max_connections = 100;

    backend->next = pool->backends;
    pool->backends = backend;

    pthread_mutex_unlock(mutex);
    return 0;
}

static db_backend_t* db_pool_find_backend(db_pool_t* pool, uint64_t id) {
    db_backend_t* backend = pool->backends;
    while (backend) {
        if (backend->id == id) return backend;
        backend = backend->next;
    }
    return NULL;
}

db_backend_t* db_pool_select_backend(db_pool_t* pool, db_query_type_t query_type) {
    if (!pool) return NULL;

    pthread_mutex_t* mutex = (pthread_mutex_t*)pool->mutex;
    pthread_mutex_lock(mutex);

    db_backend_t* selected = NULL;

    if (query_type == DB_QUERY_WRITE ||
        query_type == DB_QUERY_TRANSACTION_BEGIN ||
        query_type == DB_QUERY_SESSION_VAR) {

        db_backend_t* backend = pool->backends;
        while (backend) {
            if (backend->role == DB_BACKEND_PRIMARY && backend->is_healthy) {
                selected = backend;
                break;
            }
            backend = backend->next;
        }
    } else if (query_type == DB_QUERY_READ) {
        uint32_t min_connections = UINT32_MAX;
        uint64_t min_lag = UINT64_MAX;

        db_backend_t* backend = pool->backends;
        while (backend) {
            if (backend->role == DB_BACKEND_REPLICA &&
                backend->is_healthy &&
                backend->replication_lag_ms < 5000) {

                if (backend->active_connections < min_connections ||
                    (backend->active_connections == min_connections &&
                     backend->replication_lag_ms < min_lag)) {
                    selected = backend;
                    min_connections = backend->active_connections;
                    min_lag = backend->replication_lag_ms;
                }
            }
            backend = backend->next;
        }

        if (!selected) {
            backend = pool->backends;
            while (backend) {
                if (backend->role == DB_BACKEND_PRIMARY && backend->is_healthy) {
                    selected = backend;
                    break;
                }
                backend = backend->next;
            }
        }
    }

    pthread_mutex_unlock(mutex);
    return selected;
}

db_connection_t* db_pool_acquire(db_pool_t* pool, db_query_type_t query_type,
                                  bool in_transaction, uint64_t session_backend_id) {
    if (!pool) return NULL;

    pthread_mutex_t* mutex = (pthread_mutex_t*)pool->mutex;
    pthread_mutex_lock(mutex);

    db_backend_t* backend = NULL;

    if (session_backend_id > 0) {
        backend = db_pool_find_backend(pool, session_backend_id);
        if (!backend || !backend->is_healthy) {
            pthread_mutex_unlock(mutex);
            return NULL;
        }
    } else {
        pthread_mutex_unlock(mutex);
        backend = db_pool_select_backend(pool, query_type);
        pthread_mutex_lock(mutex);
        if (!backend) {
            pthread_mutex_unlock(mutex);
            return NULL;
        }
    }

    db_connection_t* conn = pool->idle_connections;
    db_connection_t* prev = NULL;

    while (conn) {
        if (conn->backend_id == backend->id) {
            if (prev) {
                prev->next = conn->next;
            } else {
                pool->idle_connections = conn->next;
            }

            conn->next = pool->active_connections;
            pool->active_connections = conn;
            conn->in_use = true;
            conn->in_transaction = in_transaction;
            conn->last_used = time(NULL);
            pool->total_idle--;
            pool->total_active++;
            backend->active_connections++;

            pthread_mutex_unlock(mutex);
            return conn;
        }
        prev = conn;
        conn = conn->next;
    }

    if (pool->total_active >= pool->max_connections) {
        pthread_mutex_unlock(mutex);
        return NULL;
    }

    pthread_mutex_unlock(mutex);
    return NULL;
}

void db_pool_release(db_pool_t* pool, db_connection_t* conn) {
    if (!pool || !conn) return;

    pthread_mutex_t* mutex = (pthread_mutex_t*)pool->mutex;
    pthread_mutex_lock(mutex);

    db_connection_t* curr = pool->active_connections;
    db_connection_t* prev = NULL;

    while (curr) {
        if (curr == conn) {
            if (prev) {
                prev->next = curr->next;
            } else {
                pool->active_connections = curr->next;
            }
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    db_backend_t* backend = db_pool_find_backend(pool, conn->backend_id);
    if (backend) {
        backend->active_connections--;
    }

    conn->in_use = false;
    conn->in_transaction = false;
    conn->last_used = time(NULL);

    if (pool->total_idle < pool->max_idle) {
        conn->next = pool->idle_connections;
        pool->idle_connections = conn;
        pool->total_active--;
        pool->total_idle++;
    } else {
        close(conn->fd);
        free(conn);
        pool->total_active--;
    }

    pthread_mutex_unlock(mutex);
}

int db_pool_validate_connection(db_connection_t* conn) {
    if (!conn || conn->fd < 0) return -1;

    char buf;
    int result = recv(conn->fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (result == 0) return -1;
    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;

    return 0;
}

void db_pool_cleanup_idle(db_pool_t* pool) {
    if (!pool) return;

    pthread_mutex_t* mutex = (pthread_mutex_t*)pool->mutex;
    pthread_mutex_lock(mutex);

    time_t now = time(NULL);
    db_connection_t* conn = pool->idle_connections;
    db_connection_t* prev = NULL;

    while (conn) {
        db_connection_t* next = conn->next;

        time_t age = now - conn->created_at;
        time_t idle = now - conn->last_used;

        if (db_pool_validate_connection(conn) != 0 ||
            age > (time_t)pool->max_lifetime_seconds ||
            idle > (time_t)pool->idle_timeout_seconds) {

            if (prev) {
                prev->next = next;
            } else {
                pool->idle_connections = next;
            }

            close(conn->fd);
            free(conn);
            pool->total_idle--;
        } else {
            prev = conn;
        }

        conn = next;
    }

    pthread_mutex_unlock(mutex);
}

int db_pool_get_stats(db_pool_t* pool, char* buffer, size_t buffer_size) {
    if (!pool || !buffer) return -1;

    pthread_mutex_t* mutex = (pthread_mutex_t*)pool->mutex;
    pthread_mutex_lock(mutex);

    int written = snprintf(buffer, buffer_size,
        "{\"total_idle\":%u,\"total_active\":%u,\"max_connections\":%u,\"backends\":[",
        pool->total_idle, pool->total_active, pool->max_connections);

    db_backend_t* backend = pool->backends;
    bool first = true;
    while (backend && written < (int)buffer_size) {
        if (!first) {
            written += snprintf(buffer + written, buffer_size - written, ",");
        }
        first = false;

        written += snprintf(buffer + written, buffer_size - written,
            "{\"id\":%lu,\"host\":\"%s\",\"port\":%u,\"role\":\"%s\",\"healthy\":%s,"
            "\"active_connections\":%u,\"replication_lag_ms\":%lu}",
            (unsigned long)backend->id, backend->host, backend->port,
            backend->role == DB_BACKEND_PRIMARY ? "primary" : "replica",
            backend->is_healthy ? "true" : "false",
            backend->active_connections,
            (unsigned long)backend->replication_lag_ms);

        backend = backend->next;
    }

    written += snprintf(buffer + written, buffer_size - written, "]}");

    pthread_mutex_unlock(mutex);
    return written;
}
