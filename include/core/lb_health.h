#ifndef LB_HEALTH_H
#define LB_HEALTH_H

#include "lb_types.h"

void* health_check_thread(void* arg);
void* stats_thread(void* arg);
void check_backend_health(loadbalancer_t* lb, backend_t* backend);

#endif