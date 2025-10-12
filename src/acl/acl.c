#include "acl/acl.h"
#include "core/proxy.h"
#include "http/http.h"
#include "utils/log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <arpa/inet.h>

static struct acl_keyword acl_keywords[256];
static int acl_keywords_count = 0;

static int fetch_src(struct proxy *px, struct session *sess, void *l7,
                    unsigned int opt, const struct arg *args,
                    struct sample *smp) {
    if (!sess || !sess->cli_conn)
        return 0;

    struct sockaddr_in *addr = (struct sockaddr_in *)&sess->cli_conn->addr.from;
    smp->data.type = SMP_T_IPV4;
    smp->data.u.ipv4 = addr->sin_addr;
    return 1;
}

static int fetch_dst(struct proxy *px, struct session *sess, void *l7,
                    unsigned int opt, const struct arg *args,
                    struct sample *smp) {
    if (!sess || !sess->cli_conn)
        return 0;

    struct sockaddr_in *addr = (struct sockaddr_in *)&sess->cli_conn->addr.to;
    smp->data.type = SMP_T_IPV4;
    smp->data.u.ipv4 = addr->sin_addr;
    return 1;
}

static int fetch_path(struct proxy *px, struct session *sess, void *l7,
                     unsigned int opt, const struct arg *args,
                     struct sample *smp) {
    struct http_txn *txn = sess->txn;
    if (!txn || !txn->uri)
        return 0;

    char *path = txn->uri;
    char *end = strchr(path, '?');

    smp->data.type = SMP_T_STR;
    smp->data.u.str.ptr = path;
    smp->data.u.str.len = end ? (end - path) : strlen(path);
    return 1;
}

static int fetch_hdr(struct proxy *px, struct session *sess, void *l7,
                    unsigned int opt, const struct arg *args,
                    struct sample *smp) {
    struct http_txn *txn = sess->txn;
    if (!txn || !args || args[0].type != ARGT_STR)
        return 0;

    char *hdr = http_header_get(&txn->req, args[0].data.str.ptr);
    if (!hdr)
        return 0;

    smp->data.type = SMP_T_STR;
    smp->data.u.str.ptr = hdr;
    smp->data.u.str.len = strlen(hdr);
    return 1;
}

static int fetch_method(struct proxy *px, struct session *sess, void *l7,
                       unsigned int opt, const struct arg *args,
                       struct sample *smp) {
    struct http_txn *txn = sess->txn;
    if (!txn)
        return 0;

    smp->data.type = SMP_T_METH;
    smp->data.u.meth = txn->meth;
    return 1;
}

static int fetch_url_param(struct proxy *px, struct session *sess, void *l7,
                          unsigned int opt, const struct arg *args,
                          struct sample *smp) {
    struct http_txn *txn = sess->txn;
    if (!txn || !txn->uri || !args || args[0].type != ARGT_STR)
        return 0;

    char *query = strchr(txn->uri, '?');
    if (!query)
        return 0;

    query++;
    const char *param = args[0].data.str.ptr;
    size_t param_len = strlen(param);

    char *p = query;
    while (*p) {
        if (strncmp(p, param, param_len) == 0 && p[param_len] == '=') {
            p += param_len + 1;
            char *end = strchr(p, '&');

            smp->data.type = SMP_T_STR;
            smp->data.u.str.ptr = p;
            smp->data.u.str.len = end ? (end - p) : strlen(p);
            return 1;
        }

        p = strchr(p, '&');
        if (!p) break;
        p++;
    }

    return 0;
}

int acl_match_str(struct sample *smp, acl_pattern_t *pattern) {
    if (smp->data.type != SMP_T_STR)
        return 0;

    return (smp->data.u.str.len == pattern->val.str.len &&
            memcmp(smp->data.u.str.ptr, pattern->val.str.str, pattern->val.str.len) == 0);
}

int acl_match_beg(struct sample *smp, acl_pattern_t *pattern) {
    if (smp->data.type != SMP_T_STR)
        return 0;

    if (smp->data.u.str.len < pattern->val.str.len)
        return 0;

    return memcmp(smp->data.u.str.ptr, pattern->val.str.str, pattern->val.str.len) == 0;
}

int acl_match_end(struct sample *smp, acl_pattern_t *pattern) {
    if (smp->data.type != SMP_T_STR)
        return 0;

    if (smp->data.u.str.len < pattern->val.str.len)
        return 0;

    return memcmp(smp->data.u.str.ptr + smp->data.u.str.len - pattern->val.str.len,
                  pattern->val.str.str, pattern->val.str.len) == 0;
}

int acl_match_sub(struct sample *smp, acl_pattern_t *pattern) {
    if (smp->data.type != SMP_T_STR)
        return 0;

    return memmem(smp->data.u.str.ptr, smp->data.u.str.len,
                  pattern->val.str.str, pattern->val.str.len) != NULL;
}

int acl_match_reg(struct sample *smp, acl_pattern_t *pattern) {
    if (smp->data.type != SMP_T_STR || !pattern->val.reg.regex)
        return 0;

#ifdef USE_PCRE
    int ovector[30];
    int ret = pcre_exec(pattern->val.reg.regex, pattern->val.reg.extra,
                       smp->data.u.str.ptr, smp->data.u.str.len,
                       0, 0, ovector, 30);

    return ret >= 0;
#else
    log_warning("PCRE regex matching not available");
    return 0;
#endif
}

int acl_match_ip(struct sample *smp, acl_pattern_t *pattern) {
    if (smp->data.type != SMP_T_IPV4)
        return 0;

    uint32_t addr = ntohl(smp->data.u.ipv4.s_addr);
    uint32_t pat_addr = ntohl(pattern->val.ipv4.addr.s_addr);
    uint32_t mask = ntohl(pattern->val.ipv4.mask.s_addr);

    return (addr & mask) == (pat_addr & mask);
}

int acl_match_int(struct sample *smp, acl_pattern_t *pattern) {
    if (smp->data.type != SMP_T_SINT)
        return 0;

    return smp->data.u.sint == pattern->val.i;
}

int pattern_parse_str(const char **text, acl_pattern_t *pattern, int *opaque) {
    const char *start = *text;
    const char *end = start;

    while (*end && !isspace(*end))
        end++;

    pattern->val.str.str = strndup(start, end - start);
    pattern->val.str.len = end - start;

    *text = end;
    return 1;
}

int pattern_parse_int(const char **text, acl_pattern_t *pattern, int *opaque) {
    char *endptr;
    pattern->val.i = strtol(*text, &endptr, 0);

    if (endptr == *text)
        return 0;

    *text = endptr;
    return 1;
}

int pattern_parse_ip(const char **text, acl_pattern_t *pattern, int *opaque) {
    char addr_str[64];
    const char *start = *text;
    const char *end = start;

    while (*end && !isspace(*end) && *end != '/')
        end++;

    if (end - start >= sizeof(addr_str))
        return 0;

    memcpy(addr_str, start, end - start);
    addr_str[end - start] = '\0';

    if (inet_pton(AF_INET, addr_str, &pattern->val.ipv4.addr) != 1)
        return 0;

    if (*end == '/') {
        end++;
        int cidr = strtol(end, (char **)&end, 10);
        if (cidr < 0 || cidr > 32)
            return 0;

        pattern->val.ipv4.mask.s_addr = htonl(~((1U << (32 - cidr)) - 1));
    } else {
        pattern->val.ipv4.mask.s_addr = 0xFFFFFFFF;
    }

    *text = end;
    return 1;
}

int pattern_parse_reg(const char **text, acl_pattern_t *pattern, int *opaque) {
#ifdef USE_PCRE
    const char *error;
    int erroffset;

    const char *start = *text;
    const char *end = start;

    while (*end && !isspace(*end))
        end++;

    char *regex_str = strndup(start, end - start);

    pattern->val.reg.regex = pcre_compile(regex_str, PCRE_CASELESS,
                                          &error, &erroffset, NULL);

    free(regex_str);

    if (!pattern->val.reg.regex) {
        log_error("Failed to compile regex at offset %d: %s", erroffset, error);
        return 0;
    }

    pattern->val.reg.extra = pcre_study(pattern->val.reg.regex, 0, &error);

    *text = end;
    return 1;
#else
    log_error("PCRE regex parsing not available (library not compiled)");
    return 0;
#endif
}

acl_t* acl_find(struct list *head, const char *name) {
    // Search through the ACL list for a matching name
    if (!head || !name) {
        return NULL;
    }

    // TODO: Implement proper ACL lookup when list traversal is needed
    // For now, return NULL as ACLs are stored in proxy-specific lists
    return NULL;
}

acl_expr_t* acl_expr_parse(const char **args, char **err) {
    acl_expr_t *expr = calloc(1, sizeof(*expr));
    if (!expr)
        return NULL;

    expr->patterns.n = &expr->patterns;
    expr->patterns.p = &expr->patterns;

    expr->kw = strdup(args[0]);

    for (int i = 0; acl_keywords[i].kw; i++) {
        if (strcmp(acl_keywords[i].kw, expr->kw) == 0) {
            expr->keyword = &acl_keywords[i];
            break;
        }
    }

    if (!expr->keyword) {
        *err = strdup("Unknown ACL keyword");
        free(expr->kw);
        free(expr);
        return NULL;
    }

    args++;

    while (*args) {
        acl_pattern_t *pattern = calloc(1, sizeof(*pattern));
        if (!pattern)
            break;

        if (!expr->keyword->parse(args, pattern, NULL)) {
            free(pattern);
            break;
        }

        LIST_ADDQ(&expr->patterns, &pattern->list);
    }

    return expr;
}

acl_cond_t* acl_cond_parse(const char **args, struct list *known_acl, char **err) {
    acl_cond_t *cond = calloc(1, sizeof(*cond));
    if (!cond)
        return NULL;

    cond->suites.n = &cond->suites;
    cond->suites.p = &cond->suites;

    while (*args) {
        if (strcmp(*args, "if") == 0 || strcmp(*args, "unless") == 0) {
            args++;
            continue;
        }

        const char *name = *args++;
        acl_t *acl = acl_find(known_acl, name);

        if (!acl) {
            *err = strdup("Unknown ACL");
            free(cond);
            return NULL;
        }

        cond->requires |= acl->requires;
        cond->use |= acl->use;
    }

    return cond;
}

int acl_exec_cond(acl_cond_t *cond, struct proxy *px, struct session *sess,
                  struct stream *strm, unsigned int opt) {
    if (!cond)
        return 1;

    return 1;
}

int apply_http_req_rules(struct stream *s, struct channel *req, struct proxy *px) {
    return 1;
}

int apply_http_res_rules(struct stream *s, struct channel *res) {
    return 1;
}

void acl_register_keywords(acl_keyword_t *kw_list) {
    for (int i = 0; kw_list[i].kw; i++) {
        acl_keywords[acl_keywords_count++] = kw_list[i];
    }
}

static acl_keyword_t builtin_keywords[] = {
    {"src",        pattern_parse_ip,   acl_match_ip,   fetch_src, 0, 0},
    {"dst",        pattern_parse_ip,   acl_match_ip,   fetch_dst, 0, 0},
    {"path",       pattern_parse_str,  acl_match_str,  fetch_path, 0, 0},
    {"path_beg",   pattern_parse_str,  acl_match_beg,  fetch_path, 0, 0},
    {"path_end",   pattern_parse_str,  acl_match_end,  fetch_path, 0, 0},
    {"path_sub",   pattern_parse_str,  acl_match_sub,  fetch_path, 0, 0},
    {"path_reg",   pattern_parse_reg,  acl_match_reg,  fetch_path, 0, 0},
    {"hdr",        pattern_parse_str,  acl_match_str,  fetch_hdr, 0, 0},
    {"method",     pattern_parse_str,  acl_match_str,  fetch_method, 0, 0},
    {"url_param",  pattern_parse_str,  acl_match_str,  fetch_url_param, 0, 0},
    {NULL, NULL, NULL, NULL, 0, 0}
};

void acl_init() {
    acl_register_keywords(builtin_keywords);
}