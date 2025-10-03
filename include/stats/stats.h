#ifndef STATS_STATS_H
#define STATS_STATS_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdatomic.h>

struct stream;
struct channel;
struct proxy;
struct server;
struct listener;

#define STATS_TYPE_FE    0x01
#define STATS_TYPE_BE    0x02
#define STATS_TYPE_SV    0x04
#define STATS_TYPE_SO    0x08

typedef enum {
    STAT_PX_REQ_RATE,
    STAT_PX_REQ_RATE_MAX,
    STAT_PX_REQ_TOT,
    STAT_PX_CONN_RATE,
    STAT_PX_CONN_RATE_MAX,
    STAT_PX_CONN_TOT,
    STAT_PX_CONN_CUR,
    STAT_PX_CONN_MAX,
    STAT_PX_SESS_CUR,
    STAT_PX_SESS_MAX,
    STAT_PX_SESS_LIMIT,
    STAT_PX_SESS_TOT,
    STAT_PX_BYTES_IN,
    STAT_PX_BYTES_OUT,
    STAT_PX_DENIED_REQ,
    STAT_PX_DENIED_RESP,
    STAT_PX_FAILED_REQ,
    STAT_PX_FAILED_HCHK,
    STAT_PX_STATUS,
    STAT_PX_WEIGHT,
    STAT_PX_ACT,
    STAT_PX_BCK,
    STAT_PX_CHKDOWN,
    STAT_PX_LASTCHG,
    STAT_PX_DOWNTIME,
    STAT_PX_QCUR,
    STAT_PX_QMAX,
    STAT_PX_QLIMIT,
    STAT_PX_THROTTLE,
    STAT_PX_RATE,
    STAT_PX_RATE_MAX,
    STAT_PX_CHECK_STATUS,
    STAT_PX_CHECK_CODE,
    STAT_PX_CHECK_DURATION,
    STAT_PX_HRSP_1XX,
    STAT_PX_HRSP_2XX,
    STAT_PX_HRSP_3XX,
    STAT_PX_HRSP_4XX,
    STAT_PX_HRSP_5XX,
    STAT_PX_HRSP_OTHER,
    STAT_PX_CACHE_HITS,
    STAT_PX_CACHE_MISSES,
    STAT_PX_COMP_IN,
    STAT_PX_COMP_OUT,
    STAT_PX_COMP_BYP,
    STAT_PX_COMP_RSP,
    STAT_PX_LASTSESS,
    STAT_PX_QTIME,
    STAT_PX_CTIME,
    STAT_PX_RTIME,
    STAT_PX_TTIME,
    STAT_PX_MAX
} stats_field_t;

typedef struct field {
    uint32_t type;
    union {
        int32_t s32;
        uint32_t u32;
        int64_t s64;
        uint64_t u64;
        const char *str;
    } u;
} field_t;

typedef struct stats_module {
    const char *name;
    int (*fill_stats)(void *context, field_t *stats, int len);
    size_t stats_count;
    struct stats_module *next;
} stats_module_t;

typedef struct stats_ctx {
    int type;
    int flags;
    int iid;
    int sid;

    struct proxy *px;
    struct server *sv;
    struct listener *li;

    field_t *stats;
    int stats_count;

    struct buffer *buf;
    int (*format)(struct stats_ctx *ctx);
} stats_ctx_t;

typedef struct stats_counters {
    _Atomic uint64_t cum_req;
    _Atomic uint64_t cum_conn;
    _Atomic uint64_t cum_sess;

    _Atomic uint64_t bytes_in;
    _Atomic uint64_t bytes_out;

    _Atomic uint64_t denied_req;
    _Atomic uint64_t denied_resp;
    _Atomic uint64_t failed_req;
    _Atomic uint64_t failed_conns;
    _Atomic uint64_t failed_resp;
    _Atomic uint64_t retries;
    _Atomic uint64_t redispatches;

    _Atomic uint64_t cache_hits;
    _Atomic uint64_t cache_misses;
    _Atomic uint64_t cache_lookups;

    _Atomic uint64_t comp_in;
    _Atomic uint64_t comp_out;
    _Atomic uint64_t comp_byp;

    _Atomic uint32_t conn_rate;
    _Atomic uint32_t conn_rate_max;
    _Atomic uint32_t req_rate;
    _Atomic uint32_t req_rate_max;

    _Atomic uint32_t cur_conn;
    _Atomic uint32_t cur_sess;
    _Atomic uint32_t cur_req;

    _Atomic uint32_t max_conn;
    _Atomic uint32_t max_sess;
    _Atomic uint32_t max_req;

    struct {
        _Atomic uint64_t http_1xx;
        _Atomic uint64_t http_2xx;
        _Atomic uint64_t http_3xx;
        _Atomic uint64_t http_4xx;
        _Atomic uint64_t http_5xx;
        _Atomic uint64_t http_other;
    } http;

    struct {
        _Atomic uint64_t connect;
        _Atomic uint64_t queue;
        _Atomic uint64_t response;
        _Atomic uint64_t total;
    } time;
} stats_counters_t;

int stats_fill_fe_stats(struct proxy *px, field_t *stats, int len);
int stats_fill_be_stats(struct proxy *px, field_t *stats, int len);
int stats_fill_sv_stats(struct proxy *px, struct server *sv, field_t *stats, int len);
int stats_fill_li_stats(struct listener *li, field_t *stats, int len);

int stats_dump_stat_to_buffer(struct stream *s, struct channel *res);
int stats_dump_json_to_buffer(struct stream *s, struct channel *res);
int stats_dump_html_to_buffer(struct stream *s, struct channel *res);
int stats_dump_csv_header(struct channel *chn);

int stats_dump_prometheus(struct stream *s, struct channel *res);

void stats_update_proxy(struct proxy *px);
void stats_update_server(struct server *sv);

const char* stats_get_field_name(int field);
const char* stats_get_field_desc(int field);

void stats_register_module(stats_module_t *module);
void stats_init();

int stats_process_request(struct stream *s, struct channel *req);

struct stats_admin_rule {
    const char *action;
    int (*handler)(struct stream *s, struct proxy *px, struct server *sv);
};

int stats_admin_handler(struct stream *s, struct channel *req, struct channel *res);

#endif