#ifndef LOADBALANCER_H
#define LOADBALANCER_H

#include "lb_types.h"
#include "lb_memory.h"
#include "lb_network.h"
#include "lb_health.h"
#include "lb_utils.h"

loadbalancer_t* lb_create(uint16_t port, lb_algorithm_t algorithm);
void lb_destroy(loadbalancer_t* lb);
int lb_add_backend(loadbalancer_t* lb, const char* host, uint16_t port, uint32_t weight);
int lb_remove_backend(loadbalancer_t* lb, const char* host, uint16_t port);
int lb_start(loadbalancer_t* lb);
void lb_stop(loadbalancer_t* lb);

backend_t* lb_select_backend(loadbalancer_t* lb, struct sockaddr_in* client_addr);

#endif