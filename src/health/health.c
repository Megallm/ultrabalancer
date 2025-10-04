#include "core/loadbalancer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>

void* health_check_thread(void* arg) {
    loadbalancer_t* lb = (loadbalancer_t*)arg;

    while (lb->running) {
        for (uint32_t i = 0; i < lb->backend_count; i++) {
            backend_t* backend = lb->backends[i];
            if (!backend) continue;

            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) continue;

            struct timeval tv = {
                .tv_sec = 2,
                .tv_usec = 0
            };
            setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            // Set connect timeout
            fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(backend->port)
            };

            if (inet_pton(AF_INET, backend->host, &addr.sin_addr) <= 0) {
                struct hostent* he = gethostbyname(backend->host);
                if (!he) {
                    close(sockfd);
                    atomic_store(&backend->state, BACKEND_DOWN);
                    continue;
                }
                memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
            }

            uint64_t start_ns = get_time_ns();

            // Try non-blocking connect with proper timeout handling
            int connect_result = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));

            if (connect_result < 0 && errno == EINPROGRESS) {
                // Wait for connection with select
                fd_set write_fds;
                FD_ZERO(&write_fds);
                FD_SET(sockfd, &write_fds);

                struct timeval connect_tv = { .tv_sec = 2, .tv_usec = 0 };
                int select_result = select(sockfd + 1, NULL, &write_fds, NULL, &connect_tv);

                if (select_result > 0) {
                    int sock_error = 0;
                    socklen_t len = sizeof(sock_error);
                    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sock_error, &len);
                    connect_result = (sock_error == 0) ? 0 : -1;
                } else {
                    connect_result = -1;
                }
            }

            // Set socket back to blocking for send/recv
            fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) & ~O_NONBLOCK);

            if (connect_result == 0) {
                const char* http_req = "HEAD / HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
                ssize_t sent = send(sockfd, http_req, strlen(http_req), MSG_NOSIGNAL);

                if (sent > 0) {
                    char response[512];
                    ssize_t received = recv(sockfd, response, sizeof(response) - 1, 0);

                    if (received > 0) {
                        response[received] = '\0';
                        if (strstr(response, "HTTP/1.") &&
                            (strstr(response, " 200 ") || strstr(response, " 204 ") ||
                             strstr(response, " 301 ") || strstr(response, " 302 "))) {

                            atomic_store(&backend->state, BACKEND_UP);
                            atomic_store(&backend->failed_conns, 0);
                            uint64_t response_time = get_time_ns() - start_ns;
                            atomic_store(&backend->response_time_ns, response_time);
                            atomic_store(&backend->last_check_ns, get_time_ns());
                        } else {
                            uint32_t fails = atomic_fetch_add(&backend->failed_conns, 1) + 1;
                            if (fails >= 10) {
                                atomic_store(&backend->state, BACKEND_DOWN);
                            }
                        }
                    } else {
                        uint32_t fails = atomic_fetch_add(&backend->failed_conns, 1) + 1;
                        if (fails >= 10) {
                            atomic_store(&backend->state, BACKEND_DOWN);
                        }
                    }
                } else {
                    uint32_t fails = atomic_fetch_add(&backend->failed_conns, 1) + 1;
                    if (fails >= 10) {
                        atomic_store(&backend->state, BACKEND_DOWN);
                    }
                }
            } else {
                uint32_t fails = atomic_fetch_add(&backend->failed_conns, 1) + 1;
                if (fails >= 10) {
                    atomic_store(&backend->state, BACKEND_DOWN);
                }
            }

            close(sockfd);
        }

        usleep(lb->config.health_check_interval_ms * 1000 * 2);
    }

    return NULL;
}

void* stats_thread(void* arg) {
    loadbalancer_t* lb = (loadbalancer_t*)arg;

    while (lb->running) {
        printf("\n========== Load Balancer Statistics ==========\n");
        printf("Global Stats:\n");
        printf("  Total Requests:     %lu\n", atomic_load(&lb->global_stats.total_requests));
        printf("  Failed Requests:    %lu\n", atomic_load(&lb->global_stats.failed_requests));
        printf("  Active Connections: %u\n", atomic_load(&lb->global_stats.active_connections));
        printf("  Bytes In:           %lu MB\n", atomic_load(&lb->global_stats.bytes_in) / (1024 * 1024));
        printf("  Bytes Out:          %lu MB\n", atomic_load(&lb->global_stats.bytes_out) / (1024 * 1024));

        printf("\nBackend Stats:\n");
        for (uint32_t i = 0; i < lb->backend_count; i++) {
            backend_t* b = lb->backends[i];
            if (!b) continue;

            const char* state_str = "UNKNOWN";
            switch (atomic_load(&b->state)) {
                case BACKEND_UP: state_str = "UP"; break;
                case BACKEND_DOWN: state_str = "DOWN"; break;
                case BACKEND_DRAIN: state_str = "DRAIN"; break;
                case BACKEND_MAINT: state_str = "MAINT"; break;
            }

            printf("  [%s:%u] State: %s, Active: %u, Total: %u, Failed: %u, RT: %.2fms\n",
                   b->host, b->port, state_str,
                   atomic_load(&b->active_conns),
                   atomic_load(&b->total_conns),
                   atomic_load(&b->failed_conns),
                   atomic_load(&b->response_time_ns) / 1000000.0);
        }

        sleep(5);
    }

    return NULL;
}

int lb_start(loadbalancer_t* lb) {
    if (!lb || lb->running) return -1;

    lb->listen_fd = create_listen_socket(lb->port, lb->config.so_reuseport);
    if (lb->listen_fd < 0) {
        perror("Failed to create listen socket");
        return -1;
    }

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = lb->listen_fd
    };

    if (epoll_ctl(lb->epfd, EPOLL_CTL_ADD, lb->listen_fd, &ev) < 0) {
        close(lb->listen_fd);
        return -1;
    }

    lb->running = true;

    lb->workers = calloc(lb->worker_threads, sizeof(pthread_t));
    if (!lb->workers) {
        close(lb->listen_fd);
        return -1;
    }

    for (uint32_t i = 0; i < lb->worker_threads; i++) {
        if (pthread_create(&lb->workers[i], NULL, worker_thread, lb) != 0) {
            lb->running = false;
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(lb->workers[j], NULL);
            }
            free(lb->workers);
            close(lb->listen_fd);
            return -1;
        }
    }

    pthread_t health_thread;
    pthread_create(&health_thread, NULL, health_check_thread, lb);
    pthread_detach(health_thread);

    pthread_t stats_tid;
    pthread_create(&stats_tid, NULL, stats_thread, lb);
    pthread_detach(stats_tid);

    printf("Load balancer started on port %u with %u workers\n", lb->port, lb->worker_threads);
    printf("Algorithm: ");
    switch (lb->algorithm) {
        case LB_ALGO_ROUNDROBIN: printf("Round Robin\n"); break;
        case LB_ALGO_LEASTCONN: printf("Least Connections\n"); break;
        case LB_ALGO_SOURCE: printf("IP Hash\n"); break;
        case LB_ALGO_STICKY: printf("Weighted\n"); break;
        case LB_ALGO_URI: printf("Consistent Hash\n"); break;
        case LB_ALGO_RANDOM: printf("Least Response Time\n"); break;
        default: printf("Unknown\n");
    }

    return 0;
}

void lb_stop(loadbalancer_t* lb) {
    if (!lb || !lb->running) return;

    lb->running = false;

    if (lb->workers) {
        for (uint32_t i = 0; i < lb->worker_threads; i++) {
            pthread_join(lb->workers[i], NULL);
        }
    }

    if (lb->listen_fd >= 0) {
        close(lb->listen_fd);
    }

    printf("Load balancer stopped\n");
}