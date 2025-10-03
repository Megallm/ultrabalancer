#include "core/proxy.h"
#include "core/common.h"
#include <stdlib.h>

session_t* session_new(listener_t *l) {
    session_t *sess = calloc(1, sizeof(session_t));
    if (!sess) return NULL;

    sess->listener = l;
    sess->accept_date = time(NULL);
    gettimeofday(&sess->tv_accept, NULL);

    return sess;
}

void session_free(session_t *s) {
    if (!s) return;
    free(s);
}

int session_process(session_t *s) {
    // Process session through frontend/backend
    return 0;
}

stream_t* stream_new(session_t *sess, struct channel *req, struct channel *res) {
    stream_t *s = calloc(1, sizeof(stream_t));
    if (!s) return NULL;

    s->sess = sess;
    s->req = req;
    s->res = res;

    gettimeofday(&s->logs.accept, NULL);

    return s;
}

void stream_free(stream_t *s) {
    if (!s) return;
    free(s);
}

int stream_process(stream_t *s) {
    // Process stream through analyzers
    return 0;
}