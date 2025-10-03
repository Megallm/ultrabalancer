#ifndef CORE_COMMON_H
#define CORE_COMMON_H

#include <sys/queue.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// List operations
#ifndef LIST_INIT
#define LIST_INIT(ptr) do { \
    (ptr)->n = (ptr)->p = (ptr); \
} while(0)
#endif

#define LIST_ADD(lh, el) do { \
    (el)->n = (lh)->n; \
    (el)->n->p = (el); \
    (el)->p = (lh); \
    (lh)->n = (el); \
} while(0)

#define LIST_ADDQ(lh, el) do { \
    (el)->p = (lh)->p; \
    (el)->p->n = (el); \
    (el)->n = (lh); \
    (lh)->p = (el); \
} while(0)

#define LIST_DEL(el) do { \
    (el)->n->p = (el)->p; \
    (el)->p->n = (el)->n; \
} while(0)

#define LIST_ELEM(lh, type, member) \
    ((type *)((char *)(lh) - offsetof(type, member)))

#define list_for_each_entry(pos, head, member) \
    for (pos = LIST_ELEM((head)->n, typeof(*pos), member); \
         &pos->member != (head); \
         pos = LIST_ELEM(pos->member.n, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = LIST_ELEM((head)->n, typeof(*pos), member), \
         n = LIST_ELEM(pos->member.n, typeof(*pos), member); \
         &pos->member != (head); \
         pos = n, n = LIST_ELEM(n->member.n, typeof(*n), member))

#define list_for_each_entry_safe_rev(pos, n, head, member) \
    for (pos = LIST_ELEM((head)->p, typeof(*pos), member), \
         n = LIST_ELEM(pos->member.p, typeof(*pos), member); \
         &pos->member != (head); \
         pos = n, n = LIST_ELEM(n->member.p, typeof(*n), member))

struct list {
    struct list *n;
    struct list *p;
};

struct eb_root {
    struct eb_node *b;
};

struct eb_node {
    struct eb_node *node_p;
    struct eb_node *leaf_p;
    unsigned short bit;
};

struct eb32_node {
    struct eb_node node;
    uint32_t key;
};

#define EB_ROOT { NULL }

// Global configuration
struct global {
    uint32_t maxconn;
    uint32_t nbproc;
    uint32_t nbthread;
    char *pidfile;
    char *stats_socket;
    int daemon;
    int debug;
    struct {
        uint32_t bufsize;
        uint32_t maxrewrite;
        uint32_t http_maxhdr;
        uint32_t ssl_cachesize;
        uint32_t ssl_lifetime;
    } tune;
    char *ssl_default_bind_ciphers;
};

extern struct global global;

// Time functions
#define TICK_ETERNITY (~0U)
extern volatile unsigned int now_ms;

static inline unsigned int tick_add(unsigned int t, unsigned int d) {
    return t + d;
}

// Connection flags
#define CO_FL_CONNECTED     0x00000001
#define CO_FL_WAIT_RD       0x00000002
#define CO_FL_WAIT_WR       0x00000004
#define CO_FL_ERROR         0x00000008
#define CO_FL_SOCK_RD_SH    0x00000010
#define CO_FL_SOCK_WR_SH    0x00000020

// Session flags
#define SF_ERR_SRVTO        0x00000001
#define SF_WEBSOCKET        0x00000002
#define SF_CONN_CLO         0x00000004
#define SF_TARPIT           0x00000008

// Proxy states
#define PR_FL_READY         0x00000001
#define PR_FL_STOPPED       0x00000002
#define PR_FL_PAUSED        0x00000004
#define PR_FL_DISABLED      0x00000008

// Proxy options
#define PR_O_HTTPLOG        0x00000001
#define PR_O_TCPLOG         0x00000002
#define PR_O_DONTLOGNULL    0x00000004
#define PR_O_FORWARDFOR     0x00000008
#define PR_O_HTTP_SERVER_CLOSE 0x00000010
#define PR_O_HTTP_KEEP_ALIVE   0x00000020
#define PR_O_REDISPATCH     0x00000040

// Listener states
#define LI_ASSIGNED         0
#define LI_READY            1
#define LI_PAUSED           2
#define LI_FULL             3

// Listener options
#define LI_O_SSL            0x00000001

// Server states
#define SRV_RUNNING         0x0001
#define SRV_BACKUP          0x0002
#define SRV_DRAIN           0x0004
#define SRV_WARMUP          0x0008
#define SRV_MAINTAIN        0x0010
#define SRV_SSL             0x0020

// Proxy types
#define PR_TYPE_FRONTEND    1
#define PR_TYPE_BACKEND     2
#define PR_TYPE_LISTEN      3

// HTTP message flags
#define HTTP_MSGF_VER_10    0x00000001
#define HTTP_MSGF_VER_11    0x00000002
#define HTTP_MSGF_VER_20    0x00000004
#define HTTP_MSGF_CNT_LEN   0x00000008
#define HTTP_MSGF_TE_CHNK   0x00000010
#define HTTP_MSGF_CONN_CLO  0x00000020
#define HTTP_MSGF_CONN_KAL  0x00000040
#define HTTP_MSGF_CONN_UPG  0x00000080
#define HTTP_MSGF_WEBSOCKET 0x00000100
#define HTTP_MSGF_UPGRADE_H2C 0x00000200

// Compression types
#define COMP_TYPE_NONE      0
#define COMP_TYPE_GZIP      1
#define COMP_TYPE_DEFLATE   2
#define COMP_TYPE_BROTLI    3

#define COMP_FINISH         1

// Sample types
#define SMP_T_ANY   0
#define SMP_T_BOOL  1
#define SMP_T_SINT  2
#define SMP_T_ADDR  3
#define SMP_T_IPV4  4
#define SMP_T_IPV6  5
#define SMP_T_STR   6
#define SMP_T_BIN   7
#define SMP_T_METH  8

// Argument types
#define ARGT_STOP  0
#define ARGT_SINT  1
#define ARGT_STR   2
#define ARGT_IPV4  3
#define ARGT_IPV6  4
#define ARGT_TIME  5
#define ARGT_SIZE  6
#define ARGT_FE    7
#define ARGT_BE    8
#define ARGT_TAB   9
#define ARGT_SRV   10

// Health check description length
#define HCHK_DESC_LEN 256

struct buffer {
    char *area;
    size_t size;
    size_t data;
    size_t head;
};

struct channel {
    struct buffer buf;
    uint32_t flags;
    size_t total;
    size_t analysers;
    size_t to_forward;
};

struct http_auth_data {
    char *user;
    char *pass;
    uint32_t method;
};

struct h2_frame {
    uint32_t len;
    uint8_t type;
    uint8_t flags;
    uint32_t sid;
};

struct hpack_dht {
    uint32_t size;
    uint32_t used;
    void *entries;
};

// SSL connection structure (different from lb_types connection)
struct connection {
    int fd;
    struct {
        struct sockaddr_storage from;
        struct sockaddr_storage to;
    } addr;
    uint32_t flags;
    void *xprt_ctx;
};

// Missing structures that need to be defined

struct sample {
    unsigned int flags;
    struct {
        int type;
        union {
            long long sint;
            struct in_addr ipv4;
            struct in6_addr ipv6;
            struct {
                char *ptr;
                size_t len;
            } str;
            unsigned int meth;
        } u;
    } data;
};

struct arg {
    unsigned char type;
    union {
        long long sint;
        struct {
            char *ptr;
            int len;
        } str;
    } data;
};

struct sample_expr {
    struct sample_fetch *fetch;
    struct arg *args;
};

struct task {
    struct eb32_node wq;
    void *context;
    struct task* (*process)(struct task *t, void *ctx, unsigned int state);
    unsigned int expire;
};

struct wait_event {
    struct task *task;
    int fd;
    void (*cb)(int fd, void *ctx);
    void *ctx;
};

// Task functions
struct task* task_new(void);
void task_queue(struct task *t);
void task_free(struct task *t);

// Global variables
extern struct proxy *proxies_list;
extern time_t start_time;
extern uint32_t total_connections;

#endif