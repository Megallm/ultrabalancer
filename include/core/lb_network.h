#ifndef LB_NETWORK_H
#define LB_NETWORK_H

#include "lb_types.h"

int create_listen_socket(uint16_t port, bool reuseport);
int connect_to_backend(backend_t* backend);
int set_nonblocking(int fd);

lb_connection_t* conn_create(loadbalancer_t* lb);
void conn_destroy(loadbalancer_t* lb, lb_connection_t* conn);

int handle_client_data(loadbalancer_t* lb, lb_connection_t* conn);
int handle_backend_data(loadbalancer_t* lb, lb_connection_t* conn);

void* worker_thread(void* arg);
void* worker_thread_v2(void* arg);

#endif