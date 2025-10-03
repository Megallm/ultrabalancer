#include "core/proxy.h"
#include "core/common.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <netdb.h>

server_t* server_new(const char *name) {
    server_t *srv = calloc(1, sizeof(server_t));
    if (!srv) return NULL;

    srv->id = strdup(name);
    srv->weight = 1;
    srv->cur_state = SRV_MAINTAIN;
    pthread_spin_init(&srv->lock, PTHREAD_PROCESS_PRIVATE);

    return srv;
}

void server_free(server_t *srv) {
    if (!srv) return;

    pthread_spin_destroy(&srv->lock);
    free(srv->id);
    free(srv->hostname);
    free(srv->cookie);
    free(srv->rdr_pfx);
    free(srv->ssl_cert);
    free(srv->ssl_key);
    free(srv);
}

int server_parse_addr(server_t *srv, const char *addr) {
    struct sockaddr_in *sin = (struct sockaddr_in *)&srv->addr;

    srv->hostname = strdup(addr);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(srv->port);

    if (inet_pton(AF_INET, addr, &sin->sin_addr) <= 0) {
        struct hostent *he = gethostbyname(addr);
        if (!he) return -1;
        memcpy(&sin->sin_addr, he->h_addr_list[0], he->h_length);
    }

    return 0;
}

void server_set_state(server_t *srv, int state) {
    srv->prev_state = srv->cur_state;
    srv->cur_state = state;
    srv->last_change = time(NULL);
}

int server_is_usable(server_t *srv) {
    return srv->cur_state == SRV_RUNNING &&
           atomic_load(&srv->cur_conns) < srv->max_conns;
}