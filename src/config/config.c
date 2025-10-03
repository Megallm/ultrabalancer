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
            // TODO: Need to convert acl_list to proper list structure
            rule->cond = NULL; // acl_cond_parse(&args[3], &current_proxy->acl_list, NULL);
        }

        LIST_ADDQ(&current_proxy->http_req_rules, &rule->list);
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
    // TODO: Implement stick rule parsing
    return 0;
}