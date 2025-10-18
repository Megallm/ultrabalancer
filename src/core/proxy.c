#include "core/proxy.h"
#include "core/lb_types.h"
#include "utils/log.h"
#include "health/health.h"
#include "http/http.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

struct proxy *proxies_list = NULL;
static pthread_rwlock_t proxy_lock = PTHREAD_RWLOCK_INITIALIZER;

proxy_t* proxy_new(const char *name, int mode) {
    proxy_t *px = calloc(1, sizeof(proxy_t));
    if (!px) return NULL;

    px->id = strdup(name);
    px->mode = mode;

    px->timeout.client = 30000;
    px->timeout.server = 30000;
    px->timeout.connect = 5000;
    px->timeout.check = 2000;
    px->timeout.queue = 30000;
    px->timeout.httpreq = 10000;
    px->timeout.httpka = 60000;
    px->timeout.tarpit = 60000;

    px->lb_algo = LB_ALGO_ROUNDROBIN;

    pthread_rwlock_wrlock(&proxy_lock);
    px->next = proxies_list;
    proxies_list = px;
    pthread_rwlock_unlock(&proxy_lock);

    return px;
}

void proxy_free(proxy_t *px) {
    if (!px) return;

    listener_t *l = px->listeners;
    while (l) {
        listener_t *next = l->next;
        listener_free(l);
        l = next;
    }

    server_t *srv = px->servers;
    while (srv) {
        server_t *next = srv->next;
        server_free(srv);
        srv = next;
    }

    free(px->id);
    free(px);
}

int proxy_start(proxy_t *px) {
    if (!px) return -1;

    listener_t *l;
    for (l = px->listeners; l; l = l->next) {
        if (listener_bind(l) < 0) {
            log_error("Failed to bind listener %s", l->name);
            return -1;
        }
        l->state = LI_READY;
    }

    server_t *srv;
    for (srv = px->servers; srv; srv = srv->next) {
        srv->cur_state = SRV_RUNNING;
        if (srv->check) {
            start_health_check(srv);
        }
    }

    px->state = PR_FL_READY;
    log_info("Proxy %s started", px->id);
    return 0;
}

void proxy_stop(proxy_t *px) {
    if (!px) return;

    px->state = PR_FL_STOPPED;

    listener_t *l;
    for (l = px->listeners; l; l = l->next) {
        if (l->fd > 0) {
            close(l->fd);
            l->fd = -1;
        }
        l->state = LI_ASSIGNED;
    }

    server_t *srv;
    for (srv = px->servers; srv; srv = srv->next) {
        srv->cur_state = SRV_MAINTAIN;
    }

    log_info("Proxy %s stopped", px->id);
}

void proxy_pause(proxy_t *px) {
    if (!px) return;

    px->state |= PR_FL_PAUSED;

    listener_t *l;
    for (l = px->listeners; l; l = l->next) {
        l->state = LI_PAUSED;
    }

    log_info("Proxy %s paused", px->id);
}

void proxy_resume(proxy_t *px) {
    if (!px) return;

    px->state &= ~PR_FL_PAUSED;

    listener_t *l;
    for (l = px->listeners; l; l = l->next) {
        if (l->state == LI_PAUSED) {
            l->state = LI_READY;
        }
    }

    log_info("Proxy %s resumed", px->id);
}

server_t* select_server_roundrobin(proxy_t *px) {
    static _Atomic uint32_t rr_idx = 0;
    uint32_t idx = atomic_fetch_add(&rr_idx, 1);

    server_t *srv;
    uint32_t count = 0;

    for (srv = px->servers; srv; srv = srv->next) {
        if (server_is_usable(srv)) {
            count++;
        }
    }

    if (count == 0) return NULL;

    idx %= count;
    count = 0;

    for (srv = px->servers; srv; srv = srv->next) {
        if (server_is_usable(srv)) {
            if (count == idx) {
                return srv;
            }
            count++;
        }
    }

    return NULL;
}

server_t* select_server_leastconn(proxy_t *px) {
    server_t *srv, *best = NULL;
    int32_t min_conns = INT32_MAX;

    for (srv = px->servers; srv; srv = srv->next) {
        if (!server_is_usable(srv)) continue;

        int32_t cur_conns = atomic_load(&srv->cur_conns);
        if (srv->cur_eweight > 0) {
            cur_conns = (cur_conns * 256) / srv->cur_eweight;
        }

        if (cur_conns < min_conns) {
            min_conns = cur_conns;
            best = srv;
        }
    }

    return best;
}

server_t* select_server_source(proxy_t *px, uint32_t hash) {
    server_t *srv;
    uint32_t count = 0;

    for (srv = px->servers; srv; srv = srv->next) {
        if (server_is_usable(srv)) {
            count++;
        }
    }
    

    if (count == 0) return NULL;

    hash %= count;
    count = 0;

    for (srv = px->servers; srv; srv = srv->next) {
        if (server_is_usable(srv)) {
            if (count == hash) {
                return srv;
            }
            count++;
        }
    }

    return NULL;
}

server_t* select_server_uri(proxy_t *px, const char *uri, size_t len) {
    uint32_t hash = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + uri[i];
    }

    return select_server_source(px, hash);
}

server_t* proxy_select_server(proxy_t *px, session_t *sess) {
    if (!px || !px->servers) return NULL;

    switch (px->lb_algo) {
        case LB_ALGO_ROUNDROBIN:
        case LB_ALGO_STATIC_RR:
            return select_server_roundrobin(px);

        case LB_ALGO_LEASTCONN:
            return select_server_leastconn(px);

        case LB_ALGO_SOURCE:
            if (sess && sess->cli_conn) {
                struct sockaddr_in *addr = (struct sockaddr_in *)&sess->cli_conn->addr.from;
                return select_server_source(px, addr->sin_addr.s_addr);
            }
            return select_server_roundrobin(px);

        case LB_ALGO_URI:
            if (sess && sess->txn && sess->txn->uri) {
                return select_server_uri(px, sess->txn->uri, sess->txn->uri_len);
            }
            return select_server_roundrobin(px);

        case LB_ALGO_RANDOM:
            return select_server_source(px, rand());

        default:
            return select_server_roundrobin(px);
    }
}

void proxy_inc_fe_conn(proxy_t *px) {
    atomic_fetch_add(&px->fe_counters[0], 1);
}

void proxy_dec_fe_conn(proxy_t *px) {
    atomic_fetch_sub(&px->fe_counters[0], 1);
}

void proxy_inc_be_conn(proxy_t *px) {
    atomic_fetch_add(&px->be_counters[0], 1);
}

void proxy_dec_be_conn(proxy_t *px) {
    atomic_fetch_sub(&px->be_counters[0], 1);
}

int proxy_dispatch_session(session_t *sess) {
    proxy_t *px = sess->frontend;
    if (!px) return -1;

    server_t *srv = proxy_select_server(px, sess);
    if (!srv) {
        sess->flags |= SF_ERR_SRVTO;
        return -1;
    }

    sess->target = srv;
    atomic_fetch_add(&srv->cur_conns, 1);
    atomic_fetch_add(&srv->cum_conns, 1);

    return 0;
}