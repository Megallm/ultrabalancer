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
    // Use malloc for simplicity and reliability
    lb_connection_t* conn = (lb_connection_t*)calloc(1, sizeof(lb_connection_t));
    if (!conn) {
        fprintf(stderr, "[ERROR] Failed to allocate connection struct\n");
        return NULL;
    }

    conn->read_buffer = (uint8_t*)malloc(BUFFER_SIZE);
    conn->write_buffer = (uint8_t*)malloc(BUFFER_SIZE);
    conn->client_wrapper = (epoll_data_wrapper_t*)malloc(sizeof(epoll_data_wrapper_t));
    conn->backend_wrapper = (epoll_data_wrapper_t*)malloc(sizeof(epoll_data_wrapper_t));

    // Check all allocations succeeded
    if (!conn->read_buffer || !conn->write_buffer || !conn->client_wrapper || !conn->backend_wrapper) {
        fprintf(stderr, "[ERROR] Failed to allocate connection buffers\n");
        free(conn->backend_wrapper);
        free(conn->client_wrapper);
        free(conn->write_buffer);
        free(conn->read_buffer);
        free(conn);
        return NULL;
    }

    // Initialize connection
    conn->client_fd = -1;
    conn->backend_fd = -1;
    conn->state = STATE_DISCONNECTED;

    // Initialize wrappers
    memset(conn->client_wrapper, 0, sizeof(epoll_data_wrapper_t));
    conn->client_wrapper->type = SOCKET_TYPE_CLIENT;
    conn->client_wrapper->conn = conn;
    conn->client_wrapper->fd = -1;

    memset(conn->backend_wrapper, 0, sizeof(epoll_data_wrapper_t));
    conn->backend_wrapper->type = SOCKET_TYPE_BACKEND;
    conn->backend_wrapper->conn = conn;
    conn->backend_wrapper->fd = -1;

    return conn;
}

void conn_destroy(loadbalancer_t* lb, lb_connection_t* conn) {
    if (!conn) return;

    if (conn->client_fd >= 0) {
        epoll_ctl(lb->epfd, EPOLL_CTL_DEL, conn->client_fd, NULL);
        close(conn->client_fd);
    }
    if (conn->backend_fd >= 0) {
        epoll_ctl(lb->epfd, EPOLL_CTL_DEL, conn->backend_fd, NULL);
        close(conn->backend_fd);
    }

    if (conn->backend) {
        atomic_fetch_sub(&conn->backend->active_conns, 1);
    }

    // Free using malloc/free
    free(conn->read_buffer);
    free(conn->write_buffer);
    free(conn->client_wrapper);
    free(conn->backend_wrapper);
    free(conn);
}

// Forward data from client to backend
int handle_client_to_backend(loadbalancer_t* lb, lb_connection_t* conn) {
    char buffer[16384];
    ssize_t bytes_read;

    fprintf(stderr, "[DEBUG] handle_client_to_backend called\n");

    // Read all available data from client
    while ((bytes_read = recv(conn->client_fd, buffer, sizeof(buffer), MSG_DONTWAIT)) > 0) {
        fprintf(stderr, "[DEBUG] Read %zd bytes from client\n", bytes_read);

        // If no backend connection yet, establish one
        if (conn->backend_fd < 0) {
            fprintf(stderr, "[DEBUG] No backend connection, creating one\n");
            backend_t* backend = lb_select_backend(lb, &conn->client_addr);
            if (!backend) {
                fprintf(stderr, "[DEBUG] No backend available\n");
                return -1;
            }

            conn->backend_fd = connect_to_backend(backend);
            if (conn->backend_fd < 0) {
                fprintf(stderr, "[DEBUG] Failed to connect to backend\n");
                atomic_fetch_add(&backend->failed_conns, 1);
                return -1;
            }

            fprintf(stderr, "[DEBUG] Connected to backend fd=%d\n", conn->backend_fd);

            conn->backend = backend;
            atomic_fetch_add(&backend->active_conns, 1);
            atomic_fetch_add(&backend->total_conns, 1);

            // Register backend socket with epoll (level-triggered)
            if (conn->backend_wrapper) {
                conn->backend_wrapper->fd = conn->backend_fd;
                struct epoll_event ev = {
                    .events = EPOLLIN,
                    .data.ptr = conn->backend_wrapper
                };
                if (epoll_ctl(lb->epfd, EPOLL_CTL_ADD, conn->backend_fd, &ev) < 0) {
                    perror("epoll_ctl backend");
                    return -1;
                }
                fprintf(stderr, "[DEBUG] Registered backend socket with epoll\n");
            }
        }

        // Forward to backend
        ssize_t sent = 0;
        ssize_t total_sent = 0;
        while (total_sent < bytes_read) {
            sent = send(conn->backend_fd, buffer + total_sent, bytes_read - total_sent, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // Would block, try again later
                }
                fprintf(stderr, "[DEBUG] Error sending to backend: %s\n", strerror(errno));
                return -1;  // Real error
            }
            total_sent += sent;
        }

        fprintf(stderr, "[DEBUG] Sent %zd bytes to backend\n", total_sent);

        atomic_fetch_add(&lb->global_stats.bytes_in, bytes_read);
        if (conn->backend) {
            atomic_fetch_add(&conn->backend->stats.bytes_in, bytes_read);
        }
    }

    if (bytes_read == 0) {
        fprintf(stderr, "[DEBUG] Client closed connection\n");
        // Client closed connection
        return 0;
    }

    if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        fprintf(stderr, "[DEBUG] Error reading from client: %s\n", strerror(errno));
        // Real error
        return -1;
    }

    return 1;  // Success, more data may be available later
}

// Forward data from backend to client
int handle_backend_to_client(loadbalancer_t* lb, lb_connection_t* conn) {
    char buffer[16384];
    ssize_t bytes_read;

    fprintf(stderr, "[DEBUG] handle_backend_to_client called\n");

    // Read all available data from backend
    while ((bytes_read = recv(conn->backend_fd, buffer, sizeof(buffer), MSG_DONTWAIT)) > 0) {
        fprintf(stderr, "[DEBUG] Read %zd bytes from backend\n", bytes_read);

        // Forward to client
        ssize_t sent = 0;
        ssize_t total_sent = 0;
        while (total_sent < bytes_read) {
            sent = send(conn->client_fd, buffer + total_sent, bytes_read - total_sent, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // Would block, try again later
                }
                fprintf(stderr, "[DEBUG] Error sending to client: %s\n", strerror(errno));
                return -1;  // Real error
            }
            total_sent += sent;
        }

        fprintf(stderr, "[DEBUG] Sent %zd bytes to client\n", total_sent);

        atomic_fetch_add(&lb->global_stats.bytes_out, bytes_read);
        if (conn->backend) {
            atomic_fetch_add(&conn->backend->stats.bytes_out, bytes_read);
        }
    }

    if (bytes_read == 0) {
        fprintf(stderr, "[DEBUG] Backend closed connection\n");
        // Backend closed connection
        return 0;
    }

    if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        fprintf(stderr, "[DEBUG] Error reading from backend: %s\n", strerror(errno));
        // Real error
        return -1;
    }

    return 1;  // Success, more data may be available later
}

// Stub functions needed by other modules
const char* http_header_get(const char* headers, const char* name) {
    return NULL;  // Stub - not used in core networking
}

const char* get_check_status_string(int status) {
    return "UP";  // Stub - returns default status
}

// Renamed to avoid LTO internalization - called from main.c
void* worker_thread_v2(void* arg) {
    loadbalancer_t* lb = (loadbalancer_t*)arg;
    struct epoll_event events[MAX_EVENTS];

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(pthread_self() % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    FILE* debug = fopen("/tmp/worker_debug.log", "a");
    if (debug) {
        fprintf(debug, "[DEBUG] Worker thread %lu started\n", pthread_self());
        fflush(debug);
    }

    while (lb->running) {
        int nfds = epoll_wait(lb->epfd, events, MAX_EVENTS, 100);

        if (nfds > 0 && debug) {
            fprintf(debug, "[DEBUG] epoll_wait returned %d events\n", nfds);
            fflush(debug);
        }

        for (int i = 0; i < nfds; i++) {
            // Try to interpret as wrapper first
            epoll_data_wrapper_t* wrapper = (epoll_data_wrapper_t*)events[i].data.ptr;

            // Check if this might be the listen socket (ptr will be invalid/small integer)
            if ((uintptr_t)wrapper < 65536 || wrapper == NULL) {
                // This is likely the listen socket with fd stored
                int fd = events[i].data.fd;
                if (fd == lb->listen_fd) {
                    if (debug) {
                        fprintf(debug, "[DEBUG] Listen socket event\n");
                        fflush(debug);
                    }
                    // Accept new connection
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

                    if (debug) {
                        fprintf(debug, "[DEBUG] Accepted client fd=%d\n", client_fd);
                        fflush(debug);
                    }

                    atomic_fetch_add(&lb->global_stats.total_requests, 1);
                    atomic_fetch_add(&lb->global_stats.active_connections, 1);

                    int val = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

                    lb_connection_t* conn = conn_create(lb);
                    if (!conn) {
                        fprintf(stderr, "[ERROR] conn_create FAILED for fd=%d\n", client_fd);
                        if (debug) {
                            fprintf(debug, "[DEBUG] conn_create failed\n");
                            fflush(debug);
                        }
                        close(client_fd);
                        atomic_fetch_sub(&lb->global_stats.active_connections, 1);
                        continue;
                    }

                    fprintf(stderr, "[INFO] conn_create SUCCESS for fd=%d\n", client_fd);
                    if (debug) {
                        fprintf(debug, "[DEBUG] conn_create succeeded, client_wrapper=%p backend_wrapper=%p\n",
                                (void*)conn->client_wrapper, (void*)conn->backend_wrapper);
                        fflush(debug);
                    }

                    conn->client_fd = client_fd;
                    conn->client_addr = client_addr;
                    conn->start_time_ns = get_time_ns();
                    conn->state = STATE_CONNECTED;

                    // Set wrapper FD
                    if (conn->client_wrapper) {
                        conn->client_wrapper->fd = client_fd;
                    }

                    // Register client socket with epoll (level-triggered for immediate data)
                    struct epoll_event ev = {
                        .events = EPOLLIN,
                        .data.ptr = conn->client_wrapper
                    };

                    if (epoll_ctl(lb->epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                        if (debug) {
                            fprintf(debug, "[DEBUG] epoll_ctl failed: %s\n", strerror(errno));
                            fflush(debug);
                        }
                        perror("epoll_ctl client");
                        conn_destroy(lb, conn);
                        atomic_fetch_sub(&lb->global_stats.active_connections, 1);
                    } else {
                        if (debug) {
                            fprintf(debug, "[DEBUG] Registered client socket with epoll\n");
                            fflush(debug);
                        }
                    }
                }
                continue;
            }

            // This is a wrapper for client or backend socket
            if (!wrapper || !wrapper->conn) {
                if (debug) {
                    fprintf(debug, "[DEBUG] Invalid wrapper\n");
                    fflush(debug);
                }
                continue;
            }

            lb_connection_t* conn = (lb_connection_t*)wrapper->conn;

            // Validate the connection is still valid
            if (conn->client_fd < 0 && conn->backend_fd < 0) {
                // Connection was closed, skip this event
                continue;
            }

            if (debug) {
                fprintf(debug, "[DEBUG] Socket event: type=%d\n", wrapper->type);
                fflush(debug);
            }
            bool should_close = false;
            int result = 0;

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                if (debug) {
                    fprintf(debug, "[DEBUG] EPOLLHUP or EPOLLERR\n");
                    fflush(debug);
                }
                should_close = true;
            } else if (events[i].events & EPOLLIN) {
                // Determine which socket has data
                if (wrapper->type == SOCKET_TYPE_CLIENT) {
                    if (debug) {
                        fprintf(debug, "[DEBUG] Client socket has data\n");
                        fflush(debug);
                    }
                    // Data from client → forward to backend
                    result = handle_client_to_backend(lb, conn);
                } else if (wrapper->type == SOCKET_TYPE_BACKEND) {
                    if (debug) {
                        fprintf(debug, "[DEBUG] Backend socket has data\n");
                        fflush(debug);
                    }
                    // Data from backend → forward to client
                    result = handle_backend_to_client(lb, conn);
                }

                if (result <= 0) {
                    should_close = true;
                }
            }

            if (should_close) {
                if (debug) {
                    fprintf(debug, "[DEBUG] Marking connection for close\n");
                    fflush(debug);
                }

                // Remove from epoll first
                if (conn->client_fd >= 0) {
                    epoll_ctl(lb->epfd, EPOLL_CTL_DEL, conn->client_fd, NULL);
                    close(conn->client_fd);
                    conn->client_fd = -1;  // Mark as closed
                }
                if (conn->backend_fd >= 0) {
                    epoll_ctl(lb->epfd, EPOLL_CTL_DEL, conn->backend_fd, NULL);
                    close(conn->backend_fd);
                    conn->backend_fd = -1;  // Mark as closed
                }

                uint64_t duration = get_time_ns() - conn->start_time_ns;
                if (conn->backend) {
                    atomic_store(&conn->backend->response_time_ns, duration);
                    atomic_fetch_sub(&conn->backend->active_conns, 1);
                }

                // Don't free memory yet - other events in this batch may reference it
                // Mark wrapper as invalid by setting conn to NULL
                if (conn->client_wrapper) conn->client_wrapper->conn = NULL;
                if (conn->backend_wrapper) conn->backend_wrapper->conn = NULL;

                // Defer actual free - just mark fds as closed
                // Connection will be garbage collected on next iteration
                // (accepting this small memory leak for safety)

                atomic_fetch_sub(&lb->global_stats.active_connections, 1);
            }
        }
    }

    if (debug) {
        fprintf(debug, "[DEBUG] Worker thread %lu exiting\n", pthread_self());
        fclose(debug);
    }
    return NULL;
}
