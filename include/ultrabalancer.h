#ifndef ULTRABALANCER_H
#define ULTRABALANCER_H

#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
#include <atomic>
// For C++, use std::atomic directly without macros
#else
#include <stdatomic.h>
#endif
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <pcre.h>

#define UB_VERSION "1.0.0"
#define CACHE_LINE_SIZE 64
#define MAX_BACKENDS 4096
#define MAX_FRONTENDS 1024
#define MAX_LISTENERS 256
#define MAX_EVENTS 10000
#define BUFFER_SIZE 65536
#define MAX_CONNECTIONS 1000000
#define MAX_HEADERS 128
#define MAX_COOKIES 64
#define MAX_ACL_RULES 1024
#define HTTP_MAX_HDR 8192

typedef struct global_config {
    uint32_t maxconn;
    uint32_t nbproc;
    uint32_t nbthread;
    char *chroot;
    char *pidfile;
    char *stats_socket;
    bool daemon;
    bool debug;
    uint32_t tune_bufsize;
    uint32_t tune_maxrewrite;
    uint32_t tune_http_maxhdr;
    uint32_t tune_ssl_cachesize;
    uint32_t tune_ssl_lifetime;
} global_config_t;

typedef struct proxy {
    char *id;
    enum {
        PR_MODE_TCP,
        PR_MODE_HTTP,
        PR_MODE_HEALTH
    } mode;

    uint32_t type;
    uint32_t maxconn;
    uint32_t options;
    uint32_t retries;
    uint32_t check_method;
    uint32_t check_type;
    char *check_uri;

    struct {
        uint32_t client;
        uint32_t server;
        uint32_t connect;
        uint32_t check;
        uint32_t queue;
        uint32_t tarpit;
        uint32_t httpreq;
        uint32_t httpka;
    } timeout;

    uint32_t state;

    struct listener *listeners;
    struct server *servers;
    struct acl *acl_list;
    struct stick_table *table;
    struct cache *cache;

    struct list http_req_rules;
    struct proxy *default_backend;

    lb_algorithm_t lb_algo;
    int (*lb_algo_func)(struct proxy *);

    struct proxy *next;

#ifdef __cplusplus
    std::atomic<uint64_t> fe_counters[32];
    std::atomic<uint64_t> be_counters[32];
#else
    _Atomic uint64_t fe_counters[32];
    _Atomic uint64_t be_counters[32];
#endif
} proxy_t;

#endif