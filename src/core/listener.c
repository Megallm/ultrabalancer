#include "core/proxy.h"
#include "core/common.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>

listener_t* listener_new(const char *name, const char *addr, int port) {
    listener_t *l = calloc(1, sizeof(listener_t));
    if (!l) return NULL;

    l->name = strdup(name);
    l->fd = -1;
    l->state = LI_ASSIGNED;
    l->maxconn = 10000;
    l->backlog = 512;

    struct sockaddr_in *sin = (struct sockaddr_in *)&l->addr;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);

    if (strcmp(addr, "*") == 0) {
        sin->sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, addr, &sin->sin_addr);
    }

    pthread_spin_init(&l->lock, PTHREAD_PROCESS_PRIVATE);

    return l;
}

void listener_free(listener_t *l) {
    if (!l) return;

    if (l->fd >= 0) close(l->fd);

    pthread_spin_destroy(&l->lock);
    free(l->name);
    free(l->ssl_cert);
    free(l->ssl_key);
    free(l->ssl_ca);
    free(l);
}

int listener_bind(listener_t *l) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

    if (bind(fd, (struct sockaddr*)&l->addr, sizeof(l->addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, l->backlog) < 0) {
        close(fd);
        return -1;
    }

    l->fd = fd;
    l->state = LI_READY;

    return 0;
}

int listener_accept(listener_t *l) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(l->fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) return -1;

    atomic_fetch_add(&l->counters[0], 1);

    return client_fd;
}