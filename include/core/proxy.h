#ifndef CORE_PROXY_H
#define CORE_PROXY_H

#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
#include <atomic>
// For C++, use std::atomic directly without macros
#else
#include <stdatomic.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>

#include "core/common.h"
#include "core/lb_types.h"
#include "ultrabalancer.h"

#define SRV_RUNNING        0x0001
#define SRV_BACKUP         0x0002
#define SRV_DRAIN          0x0004
#define SRV_WARMUP         0x0008
#define SRV_MAINTAIN       0x0010
#define SRV_CHECKED        0x0020
#define SRV_AGENT_CHECKED  0x0040

typedef struct server {
    char *id;
    char *hostname;
    struct sockaddr_storage addr;
    uint16_t port;

    uint32_t flags;
    uint32_t admin_flags;
    int32_t cur_state;
    int32_t prev_state;

    uint32_t weight;
    uint32_t uweight;
    uint32_t cur_eweight;
    uint32_t prev_eweight;

#ifdef __cplusplus
    std::atomic<int32_t> cur_conns;
    std::atomic<int32_t> max_conns;
    std::atomic<uint64_t> cum_conns;
#else
    _Atomic int32_t cur_conns;
    _Atomic int32_t max_conns;
    _Atomic uint64_t cum_conns;
#endif

    struct check *check;
    struct agent_check *agent;

    uint32_t slowstart;
    uint32_t warmup;

    struct server *next;
    struct server *track;

#ifdef __cplusplus
    std::atomic<uint64_t> counters[64];
#else
    _Atomic uint64_t counters[64];
#endif

    struct eb_root *pendconns;
    struct list actconns;

    struct {
        uint32_t connect;
        uint32_t queue;
        uint32_t server;
    } timeout;

    void *ssl_ctx;
    char *ssl_cert;
    char *ssl_key;

    struct {
        uint32_t inter;
        uint32_t fastinter;
        uint32_t downinter;
        uint32_t rise;
        uint32_t fall;
    } health;

    time_t last_change;
    uint32_t consecutive_errors;
    uint32_t max_queue;

    struct sockaddr_storage source_addr;
    char *cookie;
    uint32_t rdr_len;
    char *rdr_pfx;

    pthread_spinlock_t lock;
} server_t;

typedef struct listener {
    int fd;
    char *name;
    struct sockaddr_storage addr;

    uint32_t options;
    uint32_t state;
    int32_t nbconn;
    int32_t maxconn;
    uint32_t backlog;

    struct proxy *frontend;
    struct bind_conf *bind_conf;

    struct listener *next;

    void *ssl_ctx;
    char *ssl_cert;
    char *ssl_key;
    char *ssl_ca;
    char *alpn_str;

#ifdef __cplusplus
    std::atomic<uint64_t> counters[32];
#else
    _Atomic uint64_t counters[32];
#endif
    pthread_spinlock_t lock;
} listener_t;

typedef struct session {
    struct listener *listener;
    struct proxy *frontend;
    struct proxy *backend;
    struct server *target;

    struct connection *cli_conn;
    struct connection *srv_conn;

    struct stream *strm;
    struct http_txn *txn;

    uint32_t flags;
    time_t accept_date;
    struct timeval tv_accept;

    struct list list;
    struct eb32_node key;

    void *ssl;
    void *ctx;

    struct stick_match *stkctr;
} session_t;

typedef struct stream {
    struct session *sess;
    struct channel *req;
    struct channel *res;

    struct proxy *fe;
    struct proxy *be;
    struct server *target;

    uint32_t flags;
    uint32_t state;

    struct http_txn *txn;
    struct hlua *hlua;

    struct list list;

    struct {
        struct timeval accept;
        struct timeval request;
        struct timeval queue;
        struct timeval connect;
        struct timeval response;
        struct timeval close;
    } logs;

    uint64_t req_bytes;
    uint64_t res_bytes;
} stream_t;

proxy_t* proxy_new(const char *name, int mode);
void proxy_free(proxy_t *px);
int proxy_parse_config(proxy_t *px, const char *file);
int proxy_start(proxy_t *px);
void proxy_stop(proxy_t *px);
void proxy_pause(proxy_t *px);
void proxy_resume(proxy_t *px);

server_t* server_new(const char *name);
void server_free(server_t *srv);
int server_parse_addr(server_t *srv, const char *addr);
void server_set_state(server_t *srv, int state);
int server_is_usable(server_t *srv);

listener_t* listener_new(const char *name, const char *addr, int port);
void listener_free(listener_t *l);
int listener_bind(listener_t *l);
int listener_accept(listener_t *l);

session_t* session_new(listener_t *l);
void session_free(session_t *s);
int session_process(session_t *s);

stream_t* stream_new(session_t *sess, struct channel *req, struct channel *res);
void stream_free(stream_t *s);
int stream_process(stream_t *s);

#endif