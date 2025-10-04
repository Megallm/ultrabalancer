#include "config/config.h"
#include "core/proxy.h"
#include "health/health.h"
#include "acl/acl.h"
#include "http/http.h"
#include "utils/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <yaml.h>
#include <libgen.h>
#include <sys/stat.h>

#define CFG_GLOBAL   0
#define CFG_DEFAULTS 1
#define CFG_FRONTEND 2
#define CFG_BACKEND  3
#define CFG_LISTEN   4

typedef struct config_section {
    const char *name;
    int (*parse)(const char **args, int line);
} config_section_t;

static int current_section = CFG_GLOBAL;
static proxy_t *current_proxy = NULL;
static server_t *current_server = NULL;

static int parse_global(const char **args, int line) {
    if (strcmp(args[0], "daemon") == 0) {
        global.daemon = 1;
    } else if (strcmp(args[0], "maxconn") == 0) {
        global.maxconn = atoi(args[1]);
    } else if (strcmp(args[0], "nbproc") == 0) {
        global.nbproc = atoi(args[1]);
    } else if (strcmp(args[0], "nbthread") == 0) {
        global.nbthread = atoi(args[1]);
    } else if (strcmp(args[0], "log") == 0) {
        log_init(args[1], args[2] ? atoi(args[2]) : LOG_INFO);
    } else if (strcmp(args[0], "pidfile") == 0) {
        global.pidfile = strdup(args[1]);
    } else if (strcmp(args[0], "stats") == 0 && strcmp(args[1], "socket") == 0) {
        global.stats_socket = strdup(args[2]);
    } else if (strcmp(args[0], "tune.bufsize") == 0) {
        global.tune.bufsize = atoi(args[1]);
    } else if (strcmp(args[0], "tune.maxrewrite") == 0) {
        global.tune.maxrewrite = atoi(args[1]);
    } else if (strcmp(args[0], "ssl-default-bind-ciphers") == 0) {
        global.ssl_default_bind_ciphers = strdup(args[1]);
    } else {
        log_warning("Unknown global directive '%s' at line %d", args[0], line);
    }
    return 0;
}

static int parse_defaults(const char **args, int line) {
    if (!current_proxy) {
        current_proxy = proxy_new("defaults", PR_MODE_HTTP);
    }

    if (strcmp(args[0], "mode") == 0) {
        if (strcmp(args[1], "tcp") == 0) {
            current_proxy->mode = PR_MODE_TCP;
        } else if (strcmp(args[1], "http") == 0) {
            current_proxy->mode = PR_MODE_HTTP;
        }
    } else if (strcmp(args[0], "timeout") == 0) {
        uint32_t timeout = atoi(args[2]) * 1000;
        if (strcmp(args[1], "connect") == 0) {
            current_proxy->timeout.connect = timeout;
        } else if (strcmp(args[1], "client") == 0) {
            current_proxy->timeout.client = timeout;
        } else if (strcmp(args[1], "server") == 0) {
            current_proxy->timeout.server = timeout;
        } else if (strcmp(args[1], "check") == 0) {
            current_proxy->timeout.check = timeout;
        } else if (strcmp(args[1], "queue") == 0) {
            current_proxy->timeout.queue = timeout;
        } else if (strcmp(args[1], "http-request") == 0) {
            current_proxy->timeout.httpreq = timeout;
        }
    } else if (strcmp(args[0], "option") == 0) {
        if (strcmp(args[1], "httplog") == 0) {
            current_proxy->options |= PR_O_HTTPLOG;
        } else if (strcmp(args[1], "tcplog") == 0) {
            current_proxy->options |= PR_O_TCPLOG;
        } else if (strcmp(args[1], "dontlognull") == 0) {
            current_proxy->options |= PR_O_DONTLOGNULL;
        } else if (strcmp(args[1], "forwardfor") == 0) {
            current_proxy->options |= PR_O_FORWARDFOR;
        } else if (strcmp(args[1], "http-server-close") == 0) {
            current_proxy->options |= PR_O_HTTP_SERVER_CLOSE;
        } else if (strcmp(args[1], "http-keep-alive") == 0) {
            current_proxy->options |= PR_O_HTTP_KEEP_ALIVE;
        } else if (strcmp(args[1], "redispatch") == 0) {
            current_proxy->options |= PR_O_REDISPATCH;
        }
    } else if (strcmp(args[0], "retries") == 0) {
        current_proxy->retries = atoi(args[1]);
    } else if (strcmp(args[0], "maxconn") == 0) {
        current_proxy->maxconn = atoi(args[1]);
    }

    return 0;
}

static int parse_frontend(const char **args, int line) {
    if (!current_proxy || current_proxy->type != PR_TYPE_FRONTEND) {
        current_proxy = proxy_new(args[1], PR_MODE_HTTP);
        current_proxy->type = PR_TYPE_FRONTEND;
    }

    if (strcmp(args[0], "bind") == 0) {
        char *addr = args[1];
        char *port = strchr(addr, ':');

        if (!port) {
            port = addr;
            addr = "*";
        } else {
            *port++ = '\0';
        }

        listener_t *l = listener_new("frontend", addr, atoi(port));
        if (!l) {
            log_error("Failed to create listener at line %d", line);
            return -1;
        }

        l->frontend = current_proxy;
        l->next = current_proxy->listeners;
        current_proxy->listeners = l;

        for (int i = 2; args[i]; i++) {
            if (strcmp(args[i], "ssl") == 0) {
                l->options |= LI_O_SSL;
            } else if (strcmp(args[i], "crt") == 0) {
                l->ssl_cert = strdup(args[++i]);
            } else if (strcmp(args[i], "alpn") == 0) {
                l->alpn_str = strdup(args[++i]);
            }
        }
    } else if (strcmp(args[0], "acl") == 0) {
        acl_t *acl = calloc(1, sizeof(*acl));
        acl->name = strdup(args[1]);
        // Initialize list manually
        acl->expr_list.n = &acl->expr_list;
        acl->expr_list.p = &acl->expr_list;

        acl->next = current_proxy->acl_list;
        current_proxy->acl_list = acl;
    } else if (strcmp(args[0], "use_backend") == 0) {
        http_req_rule_t *rule = calloc(1, sizeof(*rule));
        rule->action = ACL_USE_BACKEND;
        rule->arg.backend.name = strdup(args[1]);

        if (args[2] && strcmp(args[2], "if") == 0) {
            rule->cond = NULL;
        }

        rule->list.n = &current_proxy->http_req_rules;
        rule->list.p = current_proxy->http_req_rules.p;
        current_proxy->http_req_rules.p->n = &rule->list;
        current_proxy->http_req_rules.p = &rule->list;
    } else if (strcmp(args[0], "default_backend") == 0) {
        current_proxy->default_backend = proxy_find_by_name(args[1]);
    }

    return parse_defaults(args, line);
}

static int parse_backend(const char **args, int line) {
    if (!current_proxy || current_proxy->type != PR_TYPE_BACKEND) {
        current_proxy = proxy_new(args[1], PR_MODE_HTTP);
        current_proxy->type = PR_TYPE_BACKEND;
    }

    if (strcmp(args[0], "balance") == 0) {
        if (strcmp(args[1], "roundrobin") == 0) {
            current_proxy->lb_algo = LB_ALGO_ROUNDROBIN;
        } else if (strcmp(args[1], "leastconn") == 0) {
            current_proxy->lb_algo = LB_ALGO_LEASTCONN;
        } else if (strcmp(args[1], "source") == 0) {
            current_proxy->lb_algo = LB_ALGO_SOURCE;
        } else if (strcmp(args[1], "uri") == 0) {
            current_proxy->lb_algo = LB_ALGO_URI;
        } else if (strcmp(args[1], "url_param") == 0) {
            current_proxy->lb_algo = LB_ALGO_URL_PARAM;
        } else if (strcmp(args[1], "hdr") == 0) {
            current_proxy->lb_algo = LB_ALGO_HDR;
        } else if (strcmp(args[1], "random") == 0) {
            current_proxy->lb_algo = LB_ALGO_RANDOM;
        }
    } else if (strcmp(args[0], "server") == 0) {
        server_t *srv = server_new(args[1]);
        if (!srv) {
            log_error("Failed to create server at line %d", line);
            return -1;
        }

        char *addr = args[2];
        char *port = strchr(addr, ':');

        if (port) {
            *port++ = '\0';
            srv->port = atoi(port);
        }

        server_parse_addr(srv, addr);

        for (int i = 3; args[i]; i++) {
            if (strcmp(args[i], "check") == 0) {
                srv->check = check_new(HCHK_TYPE_TCP);
                srv->check->server = srv;
            } else if (strcmp(args[i], "weight") == 0) {
                srv->weight = atoi(args[++i]);
            } else if (strcmp(args[i], "maxconn") == 0) {
                srv->max_conns = atoi(args[++i]);
            } else if (strcmp(args[i], "backup") == 0) {
                srv->flags |= SRV_BACKUP;
            } else if (strcmp(args[i], "ssl") == 0) {
                srv->flags |= SRV_SSL;
            } else if (strcmp(args[i], "inter") == 0) {
                if (srv->check) {
                    srv->check->interval.inter = atoi(args[++i]);
                }
            } else if (strcmp(args[i], "rise") == 0) {
                if (srv->check) {
                    srv->check->interval.rise = atoi(args[++i]);
                }
            } else if (strcmp(args[i], "fall") == 0) {
                if (srv->check) {
                    srv->check->interval.fall = atoi(args[++i]);
                }
            }
        }

        srv->next = current_proxy->servers;
        current_proxy->servers = srv;
    } else if (strcmp(args[0], "option") == 0) {
        if (strcmp(args[1], "httpchk") == 0) {
            current_proxy->check_method = HTTP_METH_OPTIONS;
            if (args[2]) {
                current_proxy->check_uri = strdup(args[2]);
            }
        } else if (strcmp(args[1], "tcp-check") == 0) {
            current_proxy->check_type = HCHK_TYPE_TCP;
        } else if (strcmp(args[1], "mysql-check") == 0) {
            current_proxy->check_type = HCHK_TYPE_MYSQL;
        } else if (strcmp(args[1], "redis-check") == 0) {
            current_proxy->check_type = HCHK_TYPE_REDIS;
        }
    } else if (strcmp(args[0], "stick-table") == 0) {
        parse_stick_table(current_proxy, &args[1]);
    } else if (strcmp(args[0], "stick") == 0) {
        parse_stick_rule(current_proxy, &args[1]);
    }

    return parse_defaults(args, line);
}

static int parse_listen(const char **args, int line) {
    if (!current_proxy) {
        current_proxy = proxy_new(args[1], PR_MODE_HTTP);
        current_proxy->type = PR_TYPE_LISTEN;
    }

    if (strcmp(args[0], "bind") == 0) {
        return parse_frontend(args, line);
    }

    if (strcmp(args[0], "server") == 0) {
        return parse_backend(args, line);
    }

    return parse_defaults(args, line);
}

int config_parse_file(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        log_error("Cannot open config file: %s", filename);
        return -1;
    }

    char line_buf[4096];
    int line_num = 0;
    int errors = 0;

    while (fgets(line_buf, sizeof(line_buf), file)) {
        line_num++;

        char *line = line_buf;
        while (isspace(*line)) line++;

        if (*line == '#' || *line == '\0' || *line == '\n')
            continue;

        size_t len = strlen(line);
        if (line[len - 1] == '\n')
            line[len - 1] = '\0';

        const char *args[64];
        int arg_count = 0;

        char *token = strtok(line, " \t");
        while (token && arg_count < 63) {
            args[arg_count++] = token;
            token = strtok(NULL, " \t");
        }
        args[arg_count] = NULL;

        if (arg_count == 0)
            continue;

        if (strcmp(args[0], "global") == 0) {
            current_section = CFG_GLOBAL;
            current_proxy = NULL;
        } else if (strcmp(args[0], "defaults") == 0) {
            current_section = CFG_DEFAULTS;
            current_proxy = NULL;
        } else if (strcmp(args[0], "frontend") == 0) {
            current_section = CFG_FRONTEND;
            current_proxy = NULL;
        } else if (strcmp(args[0], "backend") == 0) {
            current_section = CFG_BACKEND;
            current_proxy = NULL;
        } else if (strcmp(args[0], "listen") == 0) {
            current_section = CFG_LISTEN;
            current_proxy = NULL;
        } else {
            int ret = 0;

            switch (current_section) {
                case CFG_GLOBAL:
                    ret = parse_global(args, line_num);
                    break;
                case CFG_DEFAULTS:
                    ret = parse_defaults(args, line_num);
                    break;
                case CFG_FRONTEND:
                    ret = parse_frontend(args, line_num);
                    break;
                case CFG_BACKEND:
                    ret = parse_backend(args, line_num);
                    break;
                case CFG_LISTEN:
                    ret = parse_listen(args, line_num);
                    break;
            }

            if (ret < 0) {
                errors++;
                log_error("Error parsing line %d: %s", line_num, line_buf);
            }
        }
    }

    fclose(file);

    if (errors > 0) {
        log_error("Found %d errors in configuration file", errors);
        return -1;
    }

    return 0;
}

int config_check() {
    proxy_t *px;
    int errors = 0;

    for (px = proxies_list; px; px = px->next) {
        if (px->type == PR_TYPE_FRONTEND || px->type == PR_TYPE_LISTEN) {
            if (!px->listeners) {
                log_error("Proxy '%s' has no listeners", px->id);
                errors++;
            }
        }

        if (px->type == PR_TYPE_BACKEND || px->type == PR_TYPE_LISTEN) {
            if (!px->servers) {
                log_error("Proxy '%s' has no servers", px->id);
                errors++;
            }
        }

        server_t *srv;
        for (srv = px->servers; srv; srv = srv->next) {
            if (srv->check && srv->check->type == HCHK_TYPE_HTTP && !srv->check->http.uri) {
                srv->check->http.uri = "/";
            }
        }
    }

    return errors ? -1 : 0;
}

// Stub implementations for missing functions
struct proxy* proxy_find_by_name(const char *name) {
    // TODO: Implement proxy lookup
    return NULL;
}

int parse_stick_table(struct proxy *px, const char **args) {
    // TODO: Implement stick table parsing
    return 0;
}

int parse_stick_rule(struct proxy *px, const char **args) {
    return 0;
}

static int detect_config_format(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;

    if (strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0) {
        return 1;
    } else if (strcmp(ext, ".cfg") == 0) {
        return 0;
    }

    return 0;
}

static int parse_yaml_global(yaml_document_t *doc, yaml_node_t *node) {
    if (node->type != YAML_MAPPING_NODE) {
        return -1;
    }

    for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; pair++) {
        yaml_node_t *key = yaml_document_get_node(doc, pair->key);
        yaml_node_t *value = yaml_document_get_node(doc, pair->value);

        if (!key || !value) continue;

        const char *key_str = (const char *)key->data.scalar.value;
        const char *val_str = (const char *)value->data.scalar.value;

        if (strcmp(key_str, "daemon") == 0) {
            global.daemon = (strcmp(val_str, "true") == 0 || strcmp(val_str, "1") == 0);
        } else if (strcmp(key_str, "maxconn") == 0) {
            global.maxconn = atoi(val_str);
        } else if (strcmp(key_str, "nbproc") == 0) {
            global.nbproc = atoi(val_str);
        } else if (strcmp(key_str, "nbthread") == 0) {
            global.nbthread = atoi(val_str);
        } else if (strcmp(key_str, "pidfile") == 0) {
            global.pidfile = strdup(val_str);
        } else if (strcmp(key_str, "stats_socket") == 0) {
            global.stats_socket = strdup(val_str);
        } else if (strcmp(key_str, "log") == 0) {
            char *log_str = strdup(val_str);
            char *addr = strtok(log_str, ":");
            char *level = strtok(NULL, ":");
            if (addr) {
                log_init(addr, level ? atoi(level) : LOG_INFO);
            }
            free(log_str);
        }
    }

    return 0;
}

static int parse_yaml_defaults(yaml_document_t *doc, yaml_node_t *node, proxy_t **px) {
    if (node->type != YAML_MAPPING_NODE) {
        return -1;
    }

    if (!*px) {
        *px = proxy_new("defaults", PR_MODE_HTTP);
    }

    for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; pair++) {
        yaml_node_t *key = yaml_document_get_node(doc, pair->key);
        yaml_node_t *value = yaml_document_get_node(doc, pair->value);

        if (!key || !value) continue;

        const char *key_str = (const char *)key->data.scalar.value;

        if (strcmp(key_str, "mode") == 0) {
            const char *val_str = (const char *)value->data.scalar.value;
            if (strcmp(val_str, "tcp") == 0) {
                (*px)->mode = PR_MODE_TCP;
            } else if (strcmp(val_str, "http") == 0) {
                (*px)->mode = PR_MODE_HTTP;
            }
        } else if (strcmp(key_str, "timeout") == 0 && value->type == YAML_MAPPING_NODE) {
            for (yaml_node_pair_t *timeout_pair = value->data.mapping.pairs.start;
                 timeout_pair < value->data.mapping.pairs.top; timeout_pair++) {
                yaml_node_t *t_key = yaml_document_get_node(doc, timeout_pair->key);
                yaml_node_t *t_val = yaml_document_get_node(doc, timeout_pair->value);

                if (!t_key || !t_val) continue;

                const char *t_key_str = (const char *)t_key->data.scalar.value;
                uint32_t timeout = atoi((const char *)t_val->data.scalar.value) * 1000;

                if (strcmp(t_key_str, "connect") == 0) {
                    (*px)->timeout.connect = timeout;
                } else if (strcmp(t_key_str, "client") == 0) {
                    (*px)->timeout.client = timeout;
                } else if (strcmp(t_key_str, "server") == 0) {
                    (*px)->timeout.server = timeout;
                } else if (strcmp(t_key_str, "check") == 0) {
                    (*px)->timeout.check = timeout;
                }
            }
        } else if (strcmp(key_str, "retries") == 0) {
            (*px)->retries = atoi((const char *)value->data.scalar.value);
        } else if (strcmp(key_str, "maxconn") == 0) {
            (*px)->maxconn = atoi((const char *)value->data.scalar.value);
        }
    }

    return 0;
}

static int parse_yaml_frontend(yaml_document_t *doc, yaml_node_t *node, const char *name, proxy_t **px) {
    if (node->type != YAML_MAPPING_NODE) {
        return -1;
    }

    if (!*px || (*px)->type != PR_TYPE_FRONTEND) {
        *px = proxy_new(name, PR_MODE_HTTP);
        (*px)->type = PR_TYPE_FRONTEND;
    }

    for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; pair++) {
        yaml_node_t *key = yaml_document_get_node(doc, pair->key);
        yaml_node_t *value = yaml_document_get_node(doc, pair->value);

        if (!key || !value) continue;

        const char *key_str = (const char *)key->data.scalar.value;

        if (strcmp(key_str, "bind") == 0) {
            if (value->type == YAML_SEQUENCE_NODE) {
                for (yaml_node_item_t *item = value->data.sequence.items.start;
                     item < value->data.sequence.items.top; item++) {
                    yaml_node_t *bind_node = yaml_document_get_node(doc, *item);
                    if (!bind_node) continue;

                    const char *bind_str = (const char *)bind_node->data.scalar.value;
                    char *bind_copy = strdup(bind_str);
                    char *addr = bind_copy;
                    char *port = strchr(addr, ':');

                    if (!port) {
                        port = addr;
                        addr = "*";
                    } else {
                        *port++ = '\0';
                    }

                    listener_t *l = listener_new("frontend", addr, atoi(port));
                    if (l) {
                        l->frontend = *px;
                        l->next = (*px)->listeners;
                        (*px)->listeners = l;
                    }
                    free(bind_copy);
                }
            }
        } else if (strcmp(key_str, "default_backend") == 0) {
            const char *backend_name = (const char *)value->data.scalar.value;
            (*px)->default_backend = proxy_find_by_name(backend_name);
        }
    }

    return 0;
}

static int parse_yaml_backend(yaml_document_t *doc, yaml_node_t *node, const char *name, proxy_t **px) {
    if (node->type != YAML_MAPPING_NODE) {
        return -1;
    }

    if (!*px || (*px)->type != PR_TYPE_BACKEND) {
        *px = proxy_new(name, PR_MODE_HTTP);
        (*px)->type = PR_TYPE_BACKEND;
    }

    for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; pair++) {
        yaml_node_t *key = yaml_document_get_node(doc, pair->key);
        yaml_node_t *value = yaml_document_get_node(doc, pair->value);

        if (!key || !value) continue;

        const char *key_str = (const char *)key->data.scalar.value;

        if (strcmp(key_str, "balance") == 0) {
            const char *algo = (const char *)value->data.scalar.value;
            if (strcmp(algo, "roundrobin") == 0) {
                (*px)->lb_algo = LB_ALGO_ROUNDROBIN;
            } else if (strcmp(algo, "leastconn") == 0) {
                (*px)->lb_algo = LB_ALGO_LEASTCONN;
            } else if (strcmp(algo, "source") == 0) {
                (*px)->lb_algo = LB_ALGO_SOURCE;
            } else if (strcmp(algo, "uri") == 0) {
                (*px)->lb_algo = LB_ALGO_URI;
            } else if (strcmp(algo, "url_param") == 0) {
                (*px)->lb_algo = LB_ALGO_URL_PARAM;
            } else if (strcmp(algo, "hdr") == 0) {
                (*px)->lb_algo = LB_ALGO_HDR;
            } else if (strcmp(algo, "random") == 0) {
                (*px)->lb_algo = LB_ALGO_RANDOM;
            }
        } else if (strcmp(key_str, "servers") == 0 && value->type == YAML_SEQUENCE_NODE) {
            for (yaml_node_item_t *item = value->data.sequence.items.start;
                 item < value->data.sequence.items.top; item++) {
                yaml_node_t *srv_node = yaml_document_get_node(doc, *item);
                if (!srv_node || srv_node->type != YAML_MAPPING_NODE) continue;

                const char *srv_name = NULL;
                const char *srv_addr = NULL;
                int srv_weight = 100;
                int srv_check = 0;

                for (yaml_node_pair_t *srv_pair = srv_node->data.mapping.pairs.start;
                     srv_pair < srv_node->data.mapping.pairs.top; srv_pair++) {
                    yaml_node_t *s_key = yaml_document_get_node(doc, srv_pair->key);
                    yaml_node_t *s_val = yaml_document_get_node(doc, srv_pair->value);

                    if (!s_key || !s_val) continue;

                    const char *s_key_str = (const char *)s_key->data.scalar.value;
                    const char *s_val_str = (const char *)s_val->data.scalar.value;

                    if (strcmp(s_key_str, "name") == 0) {
                        srv_name = s_val_str;
                    } else if (strcmp(s_key_str, "address") == 0) {
                        srv_addr = s_val_str;
                    } else if (strcmp(s_key_str, "weight") == 0) {
                        srv_weight = atoi(s_val_str);
                    } else if (strcmp(s_key_str, "check") == 0) {
                        srv_check = (strcmp(s_val_str, "true") == 0 || strcmp(s_val_str, "1") == 0);
                    }
                }

                if (srv_name && srv_addr) {
                    server_t *srv = server_new(srv_name);
                    if (srv) {
                        char *addr_copy = strdup(srv_addr);
                        char *addr = addr_copy;
                        char *port = strchr(addr, ':');

                        if (port) {
                            *port++ = '\0';
                            srv->port = atoi(port);
                        }

                        server_parse_addr(srv, addr);
                        srv->weight = srv_weight;

                        if (srv_check) {
                            srv->check = check_new(HCHK_TYPE_TCP);
                            srv->check->server = srv;
                        }

                        srv->next = (*px)->servers;
                        (*px)->servers = srv;

                        free(addr_copy);
                    }
                }
            }
        }
    }

    return 0;
}

static int parse_yaml_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        log_error("Cannot open YAML config file: %s", filename);
        return -1;
    }

    yaml_parser_t parser;
    yaml_document_t document;

    if (!yaml_parser_initialize(&parser)) {
        log_error("Failed to initialize YAML parser");
        fclose(file);
        return -1;
    }

    yaml_parser_set_input_file(&parser, file);

    if (!yaml_parser_load(&parser, &document)) {
        log_error("Failed to parse YAML document");
        yaml_parser_delete(&parser);
        fclose(file);
        return -1;
    }

    yaml_node_t *root = yaml_document_get_root_node(&document);
    if (!root || root->type != YAML_MAPPING_NODE) {
        log_error("Invalid YAML document structure");
        yaml_document_delete(&document);
        yaml_parser_delete(&parser);
        fclose(file);
        return -1;
    }

    proxy_t *current_px = NULL;

    for (yaml_node_pair_t *pair = root->data.mapping.pairs.start;
         pair < root->data.mapping.pairs.top; pair++) {
        yaml_node_t *key = yaml_document_get_node(&document, pair->key);
        yaml_node_t *value = yaml_document_get_node(&document, pair->value);

        if (!key || !value) continue;

        const char *section = (const char *)key->data.scalar.value;

        if (strcmp(section, "global") == 0) {
            parse_yaml_global(&document, value);
        } else if (strcmp(section, "defaults") == 0) {
            parse_yaml_defaults(&document, value, &current_px);
        } else if (strcmp(section, "frontends") == 0 && value->type == YAML_MAPPING_NODE) {
            for (yaml_node_pair_t *fe_pair = value->data.mapping.pairs.start;
                 fe_pair < value->data.mapping.pairs.top; fe_pair++) {
                yaml_node_t *fe_key = yaml_document_get_node(&document, fe_pair->key);
                yaml_node_t *fe_val = yaml_document_get_node(&document, fe_pair->value);

                if (!fe_key || !fe_val) continue;

                const char *fe_name = (const char *)fe_key->data.scalar.value;
                current_px = NULL;
                parse_yaml_frontend(&document, fe_val, fe_name, &current_px);
            }
        } else if (strcmp(section, "backends") == 0 && value->type == YAML_MAPPING_NODE) {
            for (yaml_node_pair_t *be_pair = value->data.mapping.pairs.start;
                 be_pair < value->data.mapping.pairs.top; be_pair++) {
                yaml_node_t *be_key = yaml_document_get_node(&document, be_pair->key);
                yaml_node_t *be_val = yaml_document_get_node(&document, be_pair->value);

                if (!be_key || !be_val) continue;

                const char *be_name = (const char *)be_key->data.scalar.value;
                current_px = NULL;
                parse_yaml_backend(&document, be_val, be_name, &current_px);
            }
        }
    }

    yaml_document_delete(&document);
    yaml_parser_delete(&parser);
    fclose(file);

    return 0;
}

int config_parse(const char *filename) {
    if (!filename) {
        log_error("No config file specified");
        return -1;
    }

    struct stat st;
    if (stat(filename, &st) != 0) {
        log_error("Config file not found: %s", filename);
        return -1;
    }

    int format = detect_config_format(filename);

    if (format == 1) {
        return parse_yaml_config(filename);
    } else {
        return config_parse_file(filename);
    }
}