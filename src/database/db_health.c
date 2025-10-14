#include "database/db_health.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

static void* db_health_check_thread(void* arg);

db_health_checker_t* db_health_checker_create(db_pool_t* pool,
                                                uint32_t check_interval_ms,
                                                uint32_t timeout_ms,
                                                uint32_t max_lag_ms) {
    if (!pool) return NULL;

    db_health_checker_t* checker = (db_health_checker_t*)calloc(1, sizeof(db_health_checker_t));
    if (!checker) return NULL;

    checker->pool = pool;
    checker->check_interval_ms = check_interval_ms;
    checker->timeout_ms = timeout_ms;
    checker->max_lag_ms = max_lag_ms;
    atomic_store(&checker->running, false);

    return checker;
}

void db_health_checker_destroy(db_health_checker_t* checker) {
    if (!checker) return;

    if (atomic_load(&checker->running)) {
        db_health_checker_stop(checker);
    }

    free(checker);
}

int db_health_checker_start(db_health_checker_t* checker) {
    if (!checker || atomic_load(&checker->running)) return -1;

    atomic_store(&checker->running, true);

    if (pthread_create(&checker->thread, NULL, db_health_check_thread, checker) != 0) {
        atomic_store(&checker->running, false);
        return -1;
    }

    return 0;
}

void db_health_checker_stop(db_health_checker_t* checker) {
    if (!checker || !atomic_load(&checker->running)) return;

    atomic_store(&checker->running, false);

    pthread_join(checker->thread, NULL);
    checker->thread = 0;
}

static int db_health_check_tcp(const char* host, uint16_t port, uint32_t timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    int result = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    result = select(fd + 1, NULL, &write_fds, NULL, &timeout);
    if (result <= 0) {
        close(fd);
        return -1;
    }

    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static uint64_t db_health_check_postgresql_lag(const char* host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return UINT64_MAX;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd);
        return UINT64_MAX;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return UINT64_MAX;
    }

    const char* query = "SELECT EXTRACT(EPOCH FROM (now() - pg_last_xact_replay_timestamp())) * 1000;";
    ssize_t sent = send(fd, query, strlen(query), 0);

    if (sent <= 0) {
        close(fd);
        return 0;
    }

    close(fd);
    return 0;
}

static uint64_t db_health_check_mysql_lag(const char* host, uint16_t port) {
    (void)host;
    (void)port;
    return UINT64_MAX;
}

int db_health_check_backend(db_backend_t* backend) {
    if (!backend) return -1;

    int result = db_health_check_tcp(backend->host, backend->port, 5000);

    if (result == 0) {
        if (!backend->is_healthy) {
            backend->is_healthy = true;
        }
        backend->last_health_check = time(NULL);
        return 0;
    } else {
        backend->is_healthy = false;
        backend->last_health_check = time(NULL);
        return -1;
    }
}

uint64_t db_health_check_replication_lag(db_backend_t* backend) {
    if (!backend || backend->role != DB_BACKEND_REPLICA) {
        return 0;
    }

    uint64_t lag = 0;

    switch (backend->protocol) {
        case DB_PROTOCOL_POSTGRESQL:
            lag = db_health_check_postgresql_lag(backend->host, backend->port);
            break;
        case DB_PROTOCOL_MYSQL:
            lag = db_health_check_mysql_lag(backend->host, backend->port);
            break;
        case DB_PROTOCOL_REDIS:
            lag = 0;
            break;
        default:
            lag = 0;
            break;
    }

    backend->replication_lag_ms = lag;
    return lag;
}

static void* db_health_check_thread(void* arg) {
    db_health_checker_t* checker = (db_health_checker_t*)arg;

    while (atomic_load(&checker->running)) {
        pthread_mutex_t* mutex = (pthread_mutex_t*)checker->pool->mutex;
        pthread_mutex_lock(mutex);

        db_backend_t* backend = checker->pool->backends;
        while (backend) {
            db_backend_t* next = backend->next;
            pthread_mutex_unlock(mutex);

            int health_result = db_health_check_backend(backend);

            if (backend->role == DB_BACKEND_REPLICA && health_result == 0) {
                uint64_t lag = db_health_check_replication_lag(backend);

                pthread_mutex_lock(mutex);
                if (lag > checker->max_lag_ms && backend->is_healthy) {
                    backend->is_healthy = false;
                } else if (lag <= checker->max_lag_ms && !backend->is_healthy) {
                    backend->is_healthy = true;
                }
                pthread_mutex_unlock(mutex);
            }

            pthread_mutex_lock(mutex);
            backend = next;
        }

        pthread_mutex_unlock(mutex);

        usleep(checker->check_interval_ms * 1000);
    }

    return NULL;
}
