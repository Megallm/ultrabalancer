#ifndef DB_HEALTH_H
#define DB_HEALTH_H

#include "db_pool.h"
#include "db_protocol.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    db_pool_t* pool;
    atomic_bool running;
    uint32_t check_interval_ms;
    uint32_t timeout_ms;
    uint32_t max_lag_ms;
    pthread_t thread;
} db_health_checker_t;

db_health_checker_t* db_health_checker_create(db_pool_t* pool,
                                                uint32_t check_interval_ms,
                                                uint32_t timeout_ms,
                                                uint32_t max_lag_ms);

void db_health_checker_destroy(db_health_checker_t* checker);

int db_health_checker_start(db_health_checker_t* checker);
void db_health_checker_stop(db_health_checker_t* checker);

int db_health_check_backend(db_backend_t* backend);
uint64_t db_health_check_replication_lag(db_backend_t* backend);

#ifdef __cplusplus
}
#endif

#endif
