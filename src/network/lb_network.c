#include "core/loadbalancer.h"
#include "core/lb_network.h"
#include "core/lb_utils.h"
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
#include <sched.h>

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
    setsockopt(sockfd, IPPROTO_TCP, TCP_QUICKACK, &val, sizeof(val));

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

connection_t* conn_create(loadbalancer_t* lb) {
    memory_pool_t* pool = (memory_pool_t*)lb->memory_pool;

    pthread_spin_lock(&pool->lock);

    connection_t* conn = memory_pool_alloc(pool, sizeof(connection_t));
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

    memset(conn, 0, sizeof(connection_t));
    conn->client_fd = -1;
    conn->backend_fd = -1;
    conn->state = STATE_DISCONNECTED;

    return conn;
}

void conn_destroy(loadbalancer_t* lb, connection_t* conn) {
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
    memory_pool_free(pool, conn, sizeof(connection_t));

    pthread_spin_unlock(&pool->lock);
}

int handle_client_data(loadbalancer_t* lb, connection_t* conn) {
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

        struct epoll_event ev = {
            .events = EPOLLIN | EPOLLOUT | EPOLLET,
            .data.ptr = conn
        };
        epoll_ctl(lb->epfd, EPOLL_CTL_ADD, conn->backend_fd, &ev);
    }

    int pipefd[2];
    if (pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) < 0) {
        return -1;
    }

    ssize_t bytes = splice(conn->client_fd, NULL, pipefd[1], NULL,
                          MAX_SPLICE_SIZE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

    if (bytes <= 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return bytes;
    }

    atomic_fetch_add(&lb->global_stats.bytes_in, bytes);
    atomic_fetch_add(&conn->backend->stats.bytes_in, bytes);

    ssize_t sent = splice(pipefd[0], NULL, conn->backend_fd, NULL,
                         bytes, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

    close(pipefd[0]);
    close(pipefd[1]);

    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }

    return sent;
}

int handle_backend_data(loadbalancer_t* lb, connection_t* conn) {
    int pipefd[2];
    if (pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) < 0) {
        return -1;
    }

    ssize_t bytes = splice(conn->backend_fd, NULL, pipefd[1], NULL,
                          MAX_SPLICE_SIZE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

    if (bytes <= 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return bytes;
    }

    ssize_t sent = splice(pipefd[0], NULL, conn->client_fd, NULL,
                         bytes, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

    close(pipefd[0]);
    close(pipefd[1]);

    atomic_fetch_add(&lb->global_stats.bytes_out, bytes);
    atomic_fetch_add(&conn->backend->stats.bytes_out, sent);

    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }

    return sent;
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
                setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK, &val, sizeof(val));

                connection_t* conn = conn_create(lb);
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
                connection_t* conn = (connection_t*)events[i].data.ptr;

                bool should_close = false;

                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    should_close = true;
                } else {
                    if (events[i].events & EPOLLIN) {
                        int fd = (events[i].data.ptr == conn) ? conn->client_fd : conn->backend_fd;

                        if (fd == conn->client_fd) {
                            if (handle_client_data(lb, conn) < 0) {
                                should_close = true;
                            }
                        } else if (fd == conn->backend_fd) {
                            if (handle_backend_data(lb, conn) < 0) {
                                should_close = true;
                            }
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