#ifndef HEALTH_HEALTH_H
#define HEALTH_HEALTH_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <openssl/ssl.h>
#include "core/common.h"

struct real_pcre8_or_16;
typedef struct real_pcre8_or_16 pcre;

#define CHK_ST_INIT          0x00
#define CHK_ST_CONFIGURED    0x01
#define CHK_ST_ENABLED       0x02
#define CHK_ST_RUNNING       0x04
#define CHK_ST_PAUSED        0x08
#define CHK_ST_PORT_MISS     0x10
#define CHK_ST_IN_PROGRESS   0x20
#define CHK_ST_AGENT         0x40

#define CHK_RES_UNKNOWN      0x00
#define CHK_RES_NEUTRAL      0x01
#define CHK_RES_PASSED       0x02
#define CHK_RES_FAILED       0x04
#define CHK_RES_CONDPASS     0x08

typedef enum {
    HCHK_TYPE_TCP,
    HCHK_TYPE_HTTP,
    HCHK_TYPE_HTTPS,
    HCHK_TYPE_SMTP,
    HCHK_TYPE_LDAP,
    HCHK_TYPE_MYSQL,
    HCHK_TYPE_PGSQL,
    HCHK_TYPE_REDIS,
    HCHK_TYPE_SSL,
    HCHK_TYPE_EXTERNAL,
    HCHK_TYPE_AGENT
} check_type_t;

typedef enum {
    HCHK_STATUS_UNKNOWN,
    HCHK_STATUS_INI,
    HCHK_STATUS_UP,
    HCHK_STATUS_L4OK,
    HCHK_STATUS_L4TOUT,
    HCHK_STATUS_L4CON,
    HCHK_STATUS_L6OK,
    HCHK_STATUS_L6TOUT,
    HCHK_STATUS_L6RSP,
    HCHK_STATUS_L7OK,
    HCHK_STATUS_L7TOUT,
    HCHK_STATUS_L7RSP,
    HCHK_STATUS_L7OKC,
    HCHK_STATUS_L7STS,
    HCHK_STATUS_PROCERR,
    HCHK_STATUS_PROCTOUT,
    HCHK_STATUS_PROCOK,
    HCHK_STATUS_HANA
} check_status_t;

typedef struct check {
    check_type_t type;
    struct server *server;
    struct proxy *proxy;

    uint32_t state;
    check_status_t status;
    check_status_t result;

    struct {
        int fd;
        struct sockaddr_storage addr;
        struct connection *conn;
        struct buffer *buf;
        SSL *ssl;
    } conn;

    struct {
        const char *method;
        const char *uri;
        const char *host;
        const char *body;
        size_t body_len;
        int version;
    } http;

    struct {
        const char *username;
        const char *password;
        const char *database;
    } db;

    struct {
        const char *hello;
        const char *domain;
    } smtp;

    struct {
        const char *base_dn;
        const char *filter;
        const char *attribute;
    } ldap;

    struct {
        const char *command;
        char **argv;
        char **envp;
        pid_t pid;
    } external;

    struct {
        char *send_string;
        size_t send_len;
        char *expect_string;
        size_t expect_len;
        pcre *expect_regex;
        int expect_status;
    } tcp;

    struct {
        uint32_t inter;
        uint32_t fastinter;
        uint32_t downinter;
        uint32_t timeout;
        uint32_t rise;
        uint32_t fall;
    } interval;

    uint32_t consecutive_success;
    uint32_t consecutive_errors;

    time_t start_time;
    time_t last_check;
    uint32_t duration;

    uint16_t port;
    int observe;
    int via_socks4;

    struct task *task;
    struct timeval timeout;
    struct wait_event wait_event;

    char desc[HCHK_DESC_LEN];
    int code;
    int use_ssl;
    int send_proxy;
} check_t;

typedef struct agent_check {
    struct check check;
    char *command;
    uint32_t interval;
    uint32_t timeout;
} agent_check_t;

check_t* check_new(check_type_t type);
void check_free(check_t *check);

int check_init(check_t *check, struct server *srv);
int check_start(check_t *check);
void check_stop(check_t *check);

int start_health_check(struct server *srv);
int stop_health_check(struct server *srv);

int check_tcp(check_t *check);
int check_http(check_t *check);
int check_https(check_t *check);
int check_smtp(check_t *check);
int check_mysql(check_t *check);
int check_pgsql(check_t *check);
int check_redis(check_t *check);
int check_ldap(check_t *check);
int check_ssl(check_t *check);
int check_external(check_t *check);

int process_check_result(check_t *check);
void set_server_check_status(check_t *check, check_status_t status, const char *desc);
void set_server_up(check_t *check);
void set_server_down(check_t *check);
void set_server_disabled(check_t *check);
void set_server_enabled(check_t *check);
void set_server_drain(check_t *check);
void set_server_ready(check_t *check);

int wake_health_check_task(check_t *check);
struct task* process_check(struct task *t, void *context, unsigned int state);

int parse_health_check(struct server *srv, const char *args);

const char* get_check_status_string(check_status_t status);
const char* get_check_status_desc(check_status_t status);

#endif