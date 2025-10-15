#ifndef LB_TYPES_H
#define LB_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
#include <atomic>
// For C++ we use std::atomic directly without macros
#else
#include <stdatomic.h>
#endif
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define CACHE_LINE_SIZE 64
#define MAX_BACKENDS 4096
#define MAX_EVENTS 10000
#define BUFFER_SIZE 65536
#define MAX_CONNECTIONS 1000000
#define HTTP_HEADER_MAX 8192

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LB_ALGO_ROUNDROBIN,
    LB_ALGO_STATIC_RR,
    LB_ALGO_LEASTCONN,
    LB_ALGO_FIRST,
    LB_ALGO_SOURCE,
    LB_ALGO_URI,
    LB_ALGO_URL_PARAM,
    LB_ALGO_HDR,
    LB_ALGO_RDP_COOKIE,
    LB_ALGO_RANDOM,
    LB_ALGO_STICKY
} lb_algorithm_t;

typedef enum {
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_DISCONNECTING,
    STATE_DISCONNECTED,
    STATE_ERROR
} conn_state_t;

typedef enum {
    BACKEND_UP,
    BACKEND_DOWN,
    BACKEND_DRAIN,
    BACKEND_MAINT
} backend_state_t;

typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) {
#ifdef __cplusplus
    std::atomic<uint64_t> total_requests;
    std::atomic<uint64_t> failed_requests;
    std::atomic<uint64_t> bytes_in;
    std::atomic<uint64_t> bytes_out;
    std::atomic<uint32_t> active_connections;
    std::atomic<uint64_t> response_time_ns;
#else
    _Atomic uint64_t total_requests;
    _Atomic uint64_t failed_requests;
    _Atomic uint64_t bytes_in;
    _Atomic uint64_t bytes_out;
    _Atomic uint32_t active_connections;
    _Atomic uint64_t response_time_ns;
#endif
} stats_t;

typedef struct backend {
    char host[256];
    uint16_t port;
    int sockfd;

#ifdef __cplusplus
    std::atomic<backend_state_t> state;
    std::atomic<uint32_t> active_conns;
    std::atomic<uint32_t> total_conns;
    std::atomic<uint32_t> failed_conns;
    std::atomic<uint32_t> weight;
    std::atomic<uint64_t> last_check_ns;
    std::atomic<uint64_t> response_time_ns;
#else
    _Atomic backend_state_t state;
    _Atomic uint32_t active_conns;
    _Atomic uint32_t total_conns;
    _Atomic uint32_t failed_conns;
    _Atomic uint32_t weight;
    _Atomic uint64_t last_check_ns;
    _Atomic uint64_t response_time_ns;
#endif

    stats_t stats;
    pthread_spinlock_t lock;

    void* conn_pool;
    struct backend* next;

    uint8_t __padding[CACHE_LINE_SIZE - (sizeof(void*) % CACHE_LINE_SIZE)];
} backend_t;

typedef enum {
    SOCKET_TYPE_CLIENT,
    SOCKET_TYPE_BACKEND,
    SOCKET_TYPE_LISTEN
} socket_type_t;

typedef struct epoll_data_wrapper {
    socket_type_t type;
    void* conn;
    int fd;
} epoll_data_wrapper_t;

typedef struct lb_connection {
    int client_fd;
    int backend_fd;
    backend_t* backend;

    conn_state_t state;

    uint8_t* read_buffer;
    uint8_t* write_buffer;
    size_t read_pos;
    size_t write_pos;
    size_t read_size;
    size_t write_size;

    uint64_t start_time_ns;
    struct sockaddr_in client_addr;

    struct connection* next;
    struct connection* prev;

    bool keep_alive;
    bool is_websocket;
    bool is_http2;

    epoll_data_wrapper_t* client_wrapper;
    epoll_data_wrapper_t* backend_wrapper;
} lb_connection_t;

typedef struct {
    uint32_t connect_timeout_ms;
    uint32_t read_timeout_ms;
    uint32_t write_timeout_ms;
    uint32_t keepalive_timeout_ms;
    uint32_t health_check_interval_ms;
    uint32_t max_connections;
    uint32_t health_check_fail_threshold;
    bool tcp_nodelay;
    bool so_reuseport;
    bool defer_accept;
    bool health_check_enabled;
} config_t;

typedef struct loadbalancer {
    int epfd;
    int listen_fd;
    uint16_t port;

    backend_t* backends[MAX_BACKENDS];
    uint32_t backend_count;
#ifdef __cplusplus
    std::atomic<uint32_t> round_robin_idx;
#else
    _Atomic uint32_t round_robin_idx;
#endif

    lb_algorithm_t algorithm;

    lb_connection_t* conn_pool;
    pthread_spinlock_t conn_pool_lock;

    stats_t global_stats;

    bool running;

    uint32_t worker_threads;
    pthread_t* workers;

    void* memory_pool;
    void* consistent_hash;

    config_t config;
} loadbalancer_t;

#ifdef __cplusplus
}
#endif

#endif