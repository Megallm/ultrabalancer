#ifndef ACL_ACL_H
#define ACL_ACL_H

#include <stdint.h>
#include <stdbool.h>
#include "core/common.h"
#include <pcre.h>
#include <netinet/in.h>

struct session;
struct stream;

#define ACL_TEST_F_VOL_TEST    0x00000001
#define ACL_TEST_F_VOL_HDR     0x00000002
#define ACL_TEST_F_VOL_1ST     0x00000004
#define ACL_TEST_F_VOL_TXN     0x00000008
#define ACL_TEST_F_VOL_SESS    0x00000010
#define ACL_TEST_F_FETCH_MORE  0x00000020
#define ACL_TEST_F_MAY_CHANGE  0x00000040

#define ACL_USE_BACKEND        0x0001
#define ACL_USE_SERVER         0x0002
#define ACL_DENY               0x0004
#define ACL_ALLOW              0x0008
#define ACL_TARPIT             0x0010
#define ACL_REDIRECT           0x0020
#define ACL_ADD_HDR            0x0040
#define ACL_DEL_HDR            0x0080
#define ACL_SET_HDR            0x0100
#define ACL_REPLACE_HDR        0x0200
#define ACL_SET_PATH           0x0400
#define ACL_SET_QUERY          0x0800

typedef enum {
    ACL_MATCH_FOUND,
    ACL_MATCH_BOOL,
    ACL_MATCH_INT,
    ACL_MATCH_IP,
    ACL_MATCH_BIN,
    ACL_MATCH_LEN,
    ACL_MATCH_STR,
    ACL_MATCH_BEG,
    ACL_MATCH_SUB,
    ACL_MATCH_DIR,
    ACL_MATCH_DOM,
    ACL_MATCH_END,
    ACL_MATCH_REG,
    ACL_MATCH_MAP
} acl_match_t;

typedef struct acl_pattern {
    union {
        int i;
        struct {
            struct in_addr addr;
            struct in_addr mask;
        } ipv4;
        struct {
            struct in6_addr addr;
            struct in6_addr mask;
        } ipv6;
        struct {
            char *str;
            int len;
        } str;
        struct {
            pcre *regex;
            pcre_extra *extra;
        } reg;
        struct {
            void *ptr;
            size_t len;
        } data;
    } val;

    int flags;
    struct list list;
    struct acl_pattern *next;
} acl_pattern_t;

typedef struct acl_keyword {
    const char *kw;
    int (*parse)(const char **text, acl_pattern_t *pattern, int *opaque);
    int (*match)(struct sample *smp, acl_pattern_t *pattern);
    int (*fetch)(struct proxy *px, struct session *sess, void *l7,
                 unsigned int opt, const struct arg *args,
                 struct sample *smp);
    unsigned int requires;
    int arg_mask;
} acl_keyword_t;

typedef struct acl_expr {
    char *kw;
    struct acl_keyword *keyword;
    struct arg *args;
    struct list patterns;
    struct eb_root pattern_tree;
    acl_match_t match_type;
    int flags;
    struct acl_expr *next;
} acl_expr_t;

typedef struct acl {
    char *name;
    struct list expr_list;
    int requires;
    int use;
    struct acl *next;
} acl_t;

typedef struct acl_cond {
    struct list suites;
    int requires;
    int use;
    const char *file;
    int line;
} acl_cond_t;

typedef struct http_req_rule {
    struct list list;
    struct acl_cond *cond;
    uint16_t action;

    union {
        struct {
            char *realm;
        } auth;
        struct {
            char *name;
            int name_len;
            struct list fmt;
        } hdr_add;
        struct {
            char *name;
            int name_len;
        } hdr_del;
        struct {
            int code;
            char *reason;
            int flags;
            char *location;
            int location_len;
        } redir;
        struct {
            char *name;
        } backend;
        struct {
            struct sample_expr *expr;
            char *varname;
        } capid;
        struct {
            int status;
            char *reason;
        } deny;
        struct {
            char *path;
            int path_len;
        } set_path;
        struct {
            char *query;
            int query_len;
        } set_query;
    } arg;
} http_req_rule_t;

typedef struct http_res_rule {
    struct list list;
    struct acl_cond *cond;
    uint16_t action;

    union {
        struct {
            char *name;
            int name_len;
            struct list fmt;
        } hdr_add;
        struct {
            char *name;
            int name_len;
        } hdr_del;
        struct {
            struct sample_expr *expr;
            char *varname;
        } capid;
    } arg;
} http_res_rule_t;

typedef struct tcp_rule {
    struct list list;
    struct acl_cond *cond;
    uint16_t action;
} tcp_rule_t;

acl_t* acl_find(struct list *head, const char *name);
acl_expr_t* acl_expr_parse(const char **args, char **err);
acl_cond_t* acl_cond_parse(const char **args, struct list *known_acl, char **err);

int acl_exec_cond(acl_cond_t *cond, struct proxy *px, struct session *sess,
                  struct stream *strm, unsigned int opt);

int acl_match_str(struct sample *smp, acl_pattern_t *pattern);
int acl_match_beg(struct sample *smp, acl_pattern_t *pattern);
int acl_match_end(struct sample *smp, acl_pattern_t *pattern);
int acl_match_sub(struct sample *smp, acl_pattern_t *pattern);
int acl_match_reg(struct sample *smp, acl_pattern_t *pattern);
int acl_match_ip(struct sample *smp, acl_pattern_t *pattern);
int acl_match_int(struct sample *smp, acl_pattern_t *pattern);

int pattern_parse_str(const char **text, acl_pattern_t *pattern, int *opaque);
int pattern_parse_int(const char **text, acl_pattern_t *pattern, int *opaque);
int pattern_parse_ip(const char **text, acl_pattern_t *pattern, int *opaque);
int pattern_parse_reg(const char **text, acl_pattern_t *pattern, int *opaque);

int apply_http_req_rules(struct stream *s, struct channel *req, struct proxy *px);
int apply_http_res_rules(struct stream *s, struct channel *res);
int apply_tcp_req_rules(struct session *sess, struct stream *strm, struct proxy *px);

int acl_register_keyword(acl_keyword_t *kw);
void acl_register_keywords(acl_keyword_t *kw_list);

#endif