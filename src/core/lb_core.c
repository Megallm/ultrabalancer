#include "core/loadbalancer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

loadbalancer_t* lb_create(uint16_t port, lb_algorithm_t algorithm) {
    loadbalancer_t* lb = calloc(1, sizeof(loadbalancer_t));
    if (!lb) return NULL;

    lb->port = port;
    lb->algorithm = algorithm;
    lb->running = false;
    lb->worker_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;

    pthread_spin_init(&lb->conn_pool_lock, PTHREAD_PROCESS_PRIVATE);

    lb->config.connect_timeout_ms = 5000;
    lb->config.read_timeout_ms = 30000;
    lb->config.write_timeout_ms = 30000;
    lb->config.keepalive_timeout_ms = 60000;
    lb->config.health_check_interval_ms = 5000;
    lb->config.max_connections = MAX_CONNECTIONS;
    lb->config.tcp_nodelay = true;
    lb->config.so_reuseport = true;
    lb->config.defer_accept = true;

    lb->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (lb->epfd < 0) {
        free(lb);
        return NULL;
    }

    size_t pool_size = 1024 * 1024 * 1024;
    lb->memory_pool = mmap(NULL, pool_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (lb->memory_pool == MAP_FAILED) {
        close(lb->epfd);
        free(lb);
        return NULL;
    }

    if (madvise(lb->memory_pool, pool_size, MADV_HUGEPAGE) == -1) {
        madvise(lb->memory_pool, pool_size, MADV_SEQUENTIAL);
    }

    memory_pool_t* pool = (memory_pool_t*)lb->memory_pool;
    pool->base = (char*)lb->memory_pool + sizeof(memory_pool_t);
    pool->size = pool_size - sizeof(memory_pool_t);
    pool->used = 0;
    pthread_spin_init(&pool->lock, PTHREAD_PROCESS_PRIVATE);

    return lb;
}

void lb_destroy(loadbalancer_t* lb) {
    if (!lb) return;

    lb_stop(lb);

    for (uint32_t i = 0; i < lb->backend_count; i++) {
        if (lb->backends[i]) {
            pthread_spin_destroy(&lb->backends[i]->lock);
            free(lb->backends[i]);
        }
    }

    if (lb->memory_pool && lb->memory_pool != MAP_FAILED) {
        munmap(lb->memory_pool, 1024 * 1024 * 1024);
    }

    if (lb->epfd >= 0) close(lb->epfd);
    pthread_spin_destroy(&lb->conn_pool_lock);

    free(lb->workers);
    free(lb);
}

int lb_add_backend(loadbalancer_t* lb, const char* host, uint16_t port, uint32_t weight) {
    if (lb->backend_count >= MAX_BACKENDS) return -1;

    backend_t* backend = calloc(1, sizeof(backend_t));
    if (!backend) return -1;

    strncpy(backend->host, host, sizeof(backend->host) - 1);
    backend->port = port;
    backend->weight = weight ? weight : 1;
    backend->state = BACKEND_DOWN;
    backend->sockfd = -1;

    pthread_spin_init(&backend->lock, PTHREAD_PROCESS_PRIVATE);

    lb->backends[lb->backend_count++] = backend;

    return 0;
}

int create_listen_socket(uint16_t port, bool reuseport) {
    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0) return -1;

    int val = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    if (reuseport) {
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
    }

    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

    int defer = 3;
    setsockopt(sockfd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof(defer));

    val = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_QUICKACK, &val, sizeof(val));

    struct linger lng = {1, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &lng, sizeof(lng));

    int sndbuf = 2 * 1024 * 1024;
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

backend_t* lb_select_backend(loadbalancer_t* lb, struct sockaddr_in* client_addr) {
    backend_t* selected = NULL;
    uint32_t min_conns = UINT32_MAX;

    switch (lb->algorithm) {
        case LB_ALGO_ROUNDROBIN: {
            uint32_t attempts = 0;
            while (attempts < lb->backend_count) {
                uint32_t idx = atomic_fetch_add(&lb->round_robin_idx, 1) % lb->backend_count;
                backend_t* b = lb->backends[idx];
                if (b && atomic_load(&b->state) == BACKEND_UP) {
                    return b;
                }
                attempts++;
            }
            break;
        }

        case LB_ALGO_LEASTCONN: {
            for (uint32_t i = 0; i < lb->backend_count; i++) {
                backend_t* b = lb->backends[i];
                if (b && atomic_load(&b->state) == BACKEND_UP) {
                    uint32_t conns = atomic_load(&b->active_conns);
                    if (conns < min_conns) {
                        min_conns = conns;
                        selected = b;
                    }
                }
            }
            break;
        }

        case LB_ALGO_SOURCE: {
            uint32_t hash = client_addr->sin_addr.s_addr;
            hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
            hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
            hash = (hash >> 16) ^ hash;

            uint32_t idx = hash % lb->backend_count;
            for (uint32_t i = 0; i < lb->backend_count; i++) {
                uint32_t try_idx = (idx + i) % lb->backend_count;
                backend_t* b = lb->backends[try_idx];
                if (b && atomic_load(&b->state) == BACKEND_UP) {
                    return b;
                }
            }
            break;
        }

        case LB_ALGO_STICKY: {
            uint32_t total_weight = 0;
            for (uint32_t i = 0; i < lb->backend_count; i++) {
                backend_t* b = lb->backends[i];
                if (b && atomic_load(&b->state) == BACKEND_UP) {
                    total_weight += atomic_load(&b->weight);
                }
            }

            if (total_weight == 0) break;

            uint32_t random_weight = (rand() % total_weight) + 1;
            uint32_t current_weight = 0;

            for (uint32_t i = 0; i < lb->backend_count; i++) {
                backend_t* b = lb->backends[i];
                if (b && atomic_load(&b->state) == BACKEND_UP) {
                    current_weight += atomic_load(&b->weight);
                    if (random_weight <= current_weight) {
                        return b;
                    }
                }
            }
            break;
        }

        case LB_ALGO_RANDOM: {
            uint64_t min_response_time = UINT64_MAX;
            for (uint32_t i = 0; i < lb->backend_count; i++) {
                backend_t* b = lb->backends[i];
                if (b && atomic_load(&b->state) == BACKEND_UP) {
                    uint64_t rt = atomic_load(&b->response_time_ns);
                    uint32_t conns = atomic_load(&b->active_conns);
                    uint64_t score = rt * (conns + 1);
                    if (score < min_response_time) {
                        min_response_time = score;
                        selected = b;
                    }
                }
            }
            break;
        }

        default:
            break;
    }

    return selected;
}