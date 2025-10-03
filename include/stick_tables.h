#ifndef STICK_TABLES_H
#define STICK_TABLES_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <netinet/in.h>

#define STKTABLE_TYPE_IP        0x01
#define STKTABLE_TYPE_IPV6      0x02
#define STKTABLE_TYPE_INTEGER   0x04
#define STKTABLE_TYPE_STRING    0x08
#define STKTABLE_TYPE_BINARY    0x10

#define STKTABLE_DATA_CONN_CNT  0x01
#define STKTABLE_DATA_CONN_CUR  0x02
#define STKTABLE_DATA_CONN_RATE 0x04
#define STKTABLE_DATA_SESS_CNT  0x08
#define STKTABLE_DATA_SESS_RATE 0x10
#define STKTABLE_DATA_HTTP_REQ_CNT  0x20
#define STKTABLE_DATA_HTTP_REQ_RATE 0x40
#define STKTABLE_DATA_HTTP_ERR_CNT  0x80
#define STKTABLE_DATA_HTTP_ERR_RATE 0x100
#define STKTABLE_DATA_BYTES_IN  0x200
#define STKTABLE_DATA_BYTES_OUT 0x400
#define STKTABLE_DATA_SERVER_ID 0x800
#define STKTABLE_DATA_GPC0      0x1000
#define STKTABLE_DATA_GPC1      0x2000

typedef struct stick_key {
    int type;
    union {
        struct in_addr ipv4;
        struct in6_addr ipv6;
        uint32_t integer;
        struct {
            char *ptr;
            size_t len;
        } str;
        struct {
            void *ptr;
            size_t len;
        } bin;
    } data;
} stick_key_t;

typedef struct stick_counter {
    _Atomic uint32_t conn_cnt;
    _Atomic uint32_t conn_cur;
    _Atomic uint32_t conn_rate;
    _Atomic uint32_t sess_cnt;
    _Atomic uint32_t sess_rate;
    _Atomic uint32_t http_req_cnt;
    _Atomic uint32_t http_req_rate;
    _Atomic uint32_t http_err_cnt;
    _Atomic uint32_t http_err_rate;
    _Atomic uint64_t bytes_in;
    _Atomic uint64_t bytes_out;
    _Atomic uint32_t server_id;
    _Atomic uint32_t gpc0;
    _Atomic uint32_t gpc1;

    struct {
        time_t conn_rate_ts;
        time_t sess_rate_ts;
        time_t http_req_rate_ts;
        time_t http_err_rate_ts;
    } last_update;
} stick_counter_t;

typedef struct stick_entry {
    stick_key_t key;
    stick_counter_t counters;

    time_t expire;
    time_t last_access;
    uint32_t ref_cnt;

    struct eb32_node node;
    struct list list;

    pthread_rwlock_t lock;
} stick_entry_t;

typedef struct stick_table {
    char *id;
    int type;
    uint32_t size;
    uint32_t current;
    uint32_t expire;
    uint32_t data_types;

    struct eb_root keys;
    struct list lru;

    struct {
        uint32_t size;
        stick_entry_t **entries;
        pthread_spinlock_t *locks;
    } hash;

    struct {
        _Atomic uint64_t lookups;
        _Atomic uint64_t hits;
        _Atomic uint64_t misses;
        _Atomic uint64_t inserts;
        _Atomic uint64_t updates;
        _Atomic uint64_t expires;
    } stats;

    pthread_rwlock_t lock;
    struct stick_table *next;
} stick_table_t;

typedef struct stick_pattern {
    char *pattern;
    int type;
    struct acl_cond *cond;
    stick_table_t *table;
    struct list list;
} stick_pattern_t;

typedef struct stick_rule {
    struct list list;
    struct acl_cond *cond;
    stick_table_t *table;
    int flags;

    union {
        struct sample_expr *expr;
        struct {
            char *name;
            int type;
        } fetch;
    } expr;
} stick_rule_t;

typedef struct stick_match {
    stick_table_t *table;
    stick_entry_t *entry;
    struct server *server;
} stick_match_t;

stick_table_t* stktable_new(const char *id, int type, uint32_t size, uint32_t expire);
void stktable_free(stick_table_t *t);

stick_entry_t* stktable_lookup(stick_table_t *t, stick_key_t *key);
stick_entry_t* stktable_lookup_key(stick_table_t *t, stick_key_t *key);
stick_entry_t* stktable_get(stick_table_t *t, stick_key_t *key);
stick_entry_t* stktable_set(stick_table_t *t, stick_entry_t *entry);

void stktable_touch(stick_table_t *t, stick_entry_t *entry);
void stktable_expire(stick_table_t *t);
void stktable_purge(stick_table_t *t);

int stktable_update_key(stick_table_t *t, stick_key_t *key, int data_type, void *value);
int stktable_inc_counter(stick_table_t *t, stick_key_t *key, int counter);
int stktable_dec_counter(stick_table_t *t, stick_key_t *key, int counter);

void stktable_data_cast(void *data, int type, int value);

int stksess_track(struct session *sess, stick_table_t *t, stick_key_t *key);
void stksess_untrack(struct session *sess, stick_table_t *t);
struct server* stksess_get_server(struct session *sess, stick_table_t *t);

int parse_stick_table(struct proxy *px, const char **args);
int parse_stick_rule(struct proxy *px, const char **args);

void stick_tables_init();
void stick_tables_deinit();

#endif