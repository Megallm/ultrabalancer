#include "core/loadbalancer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MAX_SPLICE_SIZE (64 * 1024)

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int connect_to_backend(backend_t* backend) {
    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0) return -1;

    int val = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(backend->port),
    };

    if (inet_pton(AF_INET, backend->host, &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(backend->host);
        if (!he) {
            close(sockfd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            close(sockfd);
            return -1;
        }
    }

    return sockfd;
}

lb_connection_t* conn_create(loadbalancer_t* lb) {
    memory_pool_t* pool = (memory_pool_t*)lb->memory_pool;

    pthread_spin_lock(&pool->lock);

    lb_connection_t* conn = memory_pool_alloc(pool, sizeof(lb_connection_t));
    if (!conn) {
        pthread_spin_unlock(&pool->lock);
        return NULL;
    }

    conn->read_buffer = memory_pool_alloc(pool, BUFFER_SIZE);
    conn->write_buffer = memory_pool_alloc(pool, BUFFER_SIZE);

    pthread_spin_unlock(&pool->lock);

    if (!conn->read_buffer || !conn->write_buffer) {
        return NULL;
    }

    memset(conn, 0, sizeof(lb_connection_t));
    conn->client_fd = -1;
    conn->backend_fd = -1;
    conn->state = STATE_DISCONNECTED;

    return conn;
}

void conn_destroy(loadbalancer_t* lb, lb_connection_t* conn) {
    if (!conn) return;

    if (conn->client_fd >= 0) {
        close(conn->client_fd);
    }
    if (conn->backend_fd >= 0) {
        close(conn->backend_fd);
    }

    if (conn->backend) {
        atomic_fetch_sub(&conn->backend->active_conns, 1);
    }

    memory_pool_t* pool = (memory_pool_t*)lb->memory_pool;
    pthread_spin_lock(&pool->lock);

    if (conn->read_buffer) {
        memory_pool_free(pool, conn->read_buffer, BUFFER_SIZE);
    }
    if (conn->write_buffer) {
        memory_pool_free(pool, conn->write_buffer, BUFFER_SIZE);
    }
    memory_pool_free(pool, conn, sizeof(lb_connection_t));

    pthread_spin_unlock(&pool->lock);
}

int handle_client_data(loadbalancer_t* lb, lb_connection_t* conn) {
    char buffer[16384];
    ssize_t bytes_read = recv(conn->client_fd, buffer, sizeof(buffer), 0);

    if (bytes_read <= 0) {
        return bytes_read;
    }

    if (conn->backend_fd < 0) {
        backend_t* backend = lb_select_backend(lb, &conn->client_addr);
        if (!backend) {
            return -1;
        }

        conn->backend_fd = connect_to_backend(backend);
        if (conn->backend_fd < 0) {
            atomic_fetch_add(&backend->failed_conns, 1);
            return -1;
        }

        conn->backend = backend;
        atomic_fetch_add(&backend->active_conns, 1);
        atomic_fetch_add(&backend->total_conns, 1);
    }

    ssize_t sent = send(conn->backend_fd, buffer, bytes_read, 0);
    if (sent < 0) {
        return -1;
    }

    atomic_fetch_add(&lb->global_stats.bytes_in, bytes_read);
    if (conn->backend) {
        atomic_fetch_add(&conn->backend->stats.bytes_in, bytes_read);
    }

    char response[16384];
    ssize_t resp_bytes = recv(conn->backend_fd, response, sizeof(response), 0);

    if (resp_bytes > 0) {
        ssize_t sent_client = send(conn->client_fd, response, resp_bytes, 0);
        if (sent_client < 0) {
            return -1;
        }

        atomic_fetch_add(&lb->global_stats.bytes_out, resp_bytes);
        if (conn->backend) {
            atomic_fetch_add(&conn->backend->stats.bytes_out, resp_bytes);
        }
    }

    return bytes_read;
}

void* worker_thread(void* arg) {
    loadbalancer_t* lb = (loadbalancer_t*)arg;
    struct epoll_event events[MAX_EVENTS];

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(pthread_self() % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    while (lb->running) {
        int nfds = epoll_wait(lb->epfd, events, MAX_EVENTS, 100);

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == lb->listen_fd) {
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);

                int client_fd = accept4(lb->listen_fd, (struct sockaddr*)&client_addr,
                                       &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);

                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("accept");
                    }
                    continue;
                }

                atomic_fetch_add(&lb->global_stats.total_requests, 1);
                atomic_fetch_add(&lb->global_stats.active_connections, 1);

                int val = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

                struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
                setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                lb_connection_t* conn = conn_create(lb);
                if (!conn) {
                    close(client_fd);
                    atomic_fetch_sub(&lb->global_stats.active_connections, 1);
                    continue;
                }

                conn->client_fd = client_fd;
                conn->client_addr = client_addr;
                conn->start_time_ns = get_time_ns();
                conn->state = STATE_CONNECTED;

                struct epoll_event ev = {
                    .events = EPOLLIN | EPOLLET,
                    .data.ptr = conn
                };

                if (epoll_ctl(lb->epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                    conn_destroy(lb, conn);
                    atomic_fetch_sub(&lb->global_stats.active_connections, 1);
                }
            } else {
                lb_connection_t* conn = (lb_connection_t*)events[i].data.ptr;

                bool should_close = false;

                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    should_close = true;
                } else {
                    if (events[i].events & EPOLLIN) {
                        if (handle_client_data(lb, conn) <= 0) {
                            should_close = true;
                        }
                    }
                    if ((events[i].events & EPOLLOUT) && conn->backend_fd >= 0) {
                        int err = 0;
                        socklen_t len = sizeof(err);
                        if (getsockopt(conn->backend_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                            should_close = true;
                        }
                    }
                }

                if (should_close) {
                    epoll_ctl(lb->epfd, EPOLL_CTL_DEL, conn->client_fd, NULL);
                    if (conn->backend_fd >= 0) {
                        epoll_ctl(lb->epfd, EPOLL_CTL_DEL, conn->backend_fd, NULL);
                    }

                    uint64_t duration = get_time_ns() - conn->start_time_ns;
                    if (conn->backend) {
                        atomic_store(&conn->backend->response_time_ns, duration);
                    }

                    conn_destroy(lb, conn);
                    atomic_fetch_sub(&lb->global_stats.active_connections, 1);
                }
            }
        }
    }

    return NULL;
}