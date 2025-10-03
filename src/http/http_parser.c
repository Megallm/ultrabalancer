#include "http/http.h"
#include "core/proxy.h"
#include "utils/buffer.h"
#include "utils/log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *http_methods[] = {
    "OPTIONS", "GET", "HEAD", "POST", "PUT", "DELETE",
    "TRACE", "CONNECT", "PATCH", NULL
};

static const struct {
    int code;
    const char *text;
} http_status_codes[] = {
    {100, "Continue"},
    {101, "Switching Protocols"},
    {200, "OK"},
    {201, "Created"},
    {202, "Accepted"},
    {204, "No Content"},
    {206, "Partial Content"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {303, "See Other"},
    {304, "Not Modified"},
    {307, "Temporary Redirect"},
    {308, "Permanent Redirect"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {408, "Request Timeout"},
    {413, "Payload Too Large"},
    {414, "URI Too Long"},
    {429, "Too Many Requests"},
    {500, "Internal Server Error"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {504, "Gateway Timeout"},
    {0, NULL}
};

int http_parse_request_line(http_msg_t *msg, char *data, size_t len) {
    char *p = data;
    char *end = data + len;
    char *method_end, *uri_start, *uri_end, *ver_start;

    while (p < end && isspace(*p)) p++;

    method_end = p;
    while (method_end < end && !isspace(*method_end)) method_end++;

    if (method_end == end) return -1;

    msg->som.pos = p - data;
    msg->som.len = method_end - p;

    for (int i = 0; http_methods[i]; i++) {
        if (msg->som.len == strlen(http_methods[i]) &&
            memcmp(p, http_methods[i], msg->som.len) == 0) {
            msg->meth = 1 << i;
            break;
        }
    }

    p = method_end;
    while (p < end && isspace(*p)) p++;

    uri_start = p;
    while (p < end && !isspace(*p)) p++;
    uri_end = p;

    if (uri_end == uri_start) return -1;

    msg->uri = malloc(uri_end - uri_start + 1);
    memcpy(msg->uri, uri_start, uri_end - uri_start);
    msg->uri[uri_end - uri_start] = '\0';
    msg->uri_len = uri_end - uri_start;

    while (p < end && isspace(*p)) p++;

    ver_start = p;
    while (p < end && *p != '\r' && *p != '\n') p++;

    if (memcmp(ver_start, "HTTP/1.0", 8) == 0) {
        msg->flags |= HTTP_MSGF_VER_10;
    } else if (memcmp(ver_start, "HTTP/1.1", 8) == 0) {
        msg->flags |= HTTP_MSGF_VER_11;
    } else if (memcmp(ver_start, "HTTP/2", 6) == 0) {
        msg->flags |= HTTP_MSGF_VER_20;
    }

    msg->sol.pos = 0;
    msg->sol.len = p - data;
    msg->eol.pos = p - data;

    return p - data;
}

int http_parse_headers(http_msg_t *msg, struct buffer *buf) {
    char *p = buf->area + msg->next;
    char *end = buf->area + buf->data;
    http_hdr_t *hdr;

    while (p < end) {
        char *eol = memchr(p, '\n', end - p);
        if (!eol) break;

        if (p == eol || (p + 1 == eol && *p == '\r')) {
            msg->next = eol + 1 - buf->area;
            msg->msg_state = HTTP_MSG_BODY;
            return 1;
        }

        char *colon = memchr(p, ':', eol - p);
        if (!colon) {
            msg->msg_state = HTTP_MSG_ERROR;
            return -1;
        }

        hdr = calloc(1, sizeof(http_hdr_t));
        if (!hdr) return -1;

        hdr->n.ptr = p;
        hdr->n.len = colon - p;

        while (hdr->n.len > 0 && isspace(hdr->n.ptr[hdr->n.len - 1]))
            hdr->n.len--;

        p = colon + 1;
        while (p < eol && isspace(*p)) p++;

        hdr->v.ptr = p;
        hdr->v.len = eol - p;

        if (hdr->v.len > 0 && hdr->v.ptr[hdr->v.len - 1] == '\r')
            hdr->v.len--;

        while (hdr->v.len > 0 && isspace(hdr->v.ptr[hdr->v.len - 1]))
            hdr->v.len--;

        if (strncasecmp(hdr->n.ptr, "content-length", hdr->n.len) == 0) {
            msg->body_len = strtoll(hdr->v.ptr, NULL, 10);
            msg->flags |= HTTP_MSGF_CNT_LEN;
        } else if (strncasecmp(hdr->n.ptr, "transfer-encoding", hdr->n.len) == 0) {
            if (strncasecmp(hdr->v.ptr, "chunked", 7) == 0) {
                msg->flags |= HTTP_MSGF_TE_CHNK;
            }
        } else if (strncasecmp(hdr->n.ptr, "connection", hdr->n.len) == 0) {
            if (strncasecmp(hdr->v.ptr, "close", 5) == 0) {
                msg->flags |= HTTP_MSGF_CONN_CLO;
            } else if (strncasecmp(hdr->v.ptr, "keep-alive", 10) == 0) {
                msg->flags |= HTTP_MSGF_CONN_KAL;
            } else if (strncasecmp(hdr->v.ptr, "upgrade", 7) == 0) {
                msg->flags |= HTTP_MSGF_CONN_UPG;
            }
        } else if (strncasecmp(hdr->n.ptr, "upgrade", hdr->n.len) == 0) {
            if (strncasecmp(hdr->v.ptr, "websocket", 9) == 0) {
                msg->flags |= HTTP_MSGF_WEBSOCKET;
            } else if (strncasecmp(hdr->v.ptr, "h2c", 3) == 0) {
                msg->flags |= HTTP_MSGF_UPGRADE_H2C;
            }
        }

        http_header_add_tail(msg, hdr);
        p = eol + 1;
    }

    msg->next = p - buf->area;
    return 0;
}

int http_parse_chunk_size(http_msg_t *msg, struct buffer *buf) {
    char *p = buf->area + msg->next;
    char *end = buf->area + buf->data;
    uint64_t chunk_size = 0;

    while (p < end && isxdigit(*p)) {
        chunk_size = (chunk_size << 4) |
                    (*p <= '9' ? *p - '0' : toupper(*p) - 'A' + 10);
        p++;
    }

    while (p < end && *p != '\n') {
        if (*p == ';') {
            while (p < end && *p != '\n') p++;
            break;
        }
        p++;
    }

    if (p >= end) return 0;

    p++;

    msg->chunk_len = chunk_size;
    msg->next = p - buf->area;

    if (chunk_size == 0) {
        msg->msg_state = HTTP_MSG_DONE;
    } else {
        msg->msg_state = HTTP_MSG_CHUNK_DATA;
    }

    return 1;
}

int http_msg_analyzer(http_msg_t *msg, struct buffer *buf) {
    if (msg->msg_state == HTTP_MSG_ERROR || msg->msg_state == HTTP_MSG_DONE)
        return 0;

    while (1) {
        switch (msg->msg_state) {
            case HTTP_MSG_RQBEFORE:
            case HTTP_MSG_RQMETH:
                if (http_parse_request_line(msg, buf->area, buf->data) < 0) {
                    msg->msg_state = HTTP_MSG_ERROR;
                    return -1;
                }
                msg->msg_state = HTTP_MSG_HDR_FIRST;
                break;

            case HTTP_MSG_HDR_FIRST:
            case HTTP_MSG_HDR_NAME:
            case HTTP_MSG_HDR_VAL:
                if (http_parse_headers(msg, buf) < 0) {
                    msg->msg_state = HTTP_MSG_ERROR;
                    return -1;
                }
                if (msg->msg_state != HTTP_MSG_BODY)
                    return 0;
                break;

            case HTTP_MSG_BODY:
                if (msg->flags & HTTP_MSGF_TE_CHNK) {
                    msg->msg_state = HTTP_MSG_CHUNK_SIZE;
                } else if (msg->flags & HTTP_MSGF_CNT_LEN) {
                    if (msg->body_len == 0) {
                        msg->msg_state = HTTP_MSG_DONE;
                        return 1;
                    }
                } else {
                    msg->msg_state = HTTP_MSG_DONE;
                    return 1;
                }
                break;

            case HTTP_MSG_CHUNK_SIZE:
                if (http_parse_chunk_size(msg, buf) <= 0)
                    return 0;
                break;

            case HTTP_MSG_CHUNK_DATA:
                if (buf->data - msg->next < msg->chunk_len + 2)
                    return 0;

                msg->next += msg->chunk_len + 2;
                msg->msg_state = HTTP_MSG_CHUNK_SIZE;
                break;

            case HTTP_MSG_DONE:
                return 1;

            default:
                msg->msg_state = HTTP_MSG_ERROR;
                return -1;
        }
    }
}

int http_process_request(struct stream *s, struct channel *req) {
    struct http_txn *txn = s->txn;
    struct proxy *px = s->sess->frontend;
    http_msg_t *msg = &txn->req;

    if (msg->msg_state < HTTP_MSG_BODY) {
        int ret = http_msg_analyzer(msg, &req->buf);
        if (ret == 0) return 0;
        if (ret < 0) {
            txn->status = 400;
            return -1;
        }
    }

    if (px->timeout.httpreq)
        req->analysers = tick_add(now_ms, px->timeout.httpreq);

    if (msg->flags & HTTP_MSGF_WEBSOCKET) {
        s->flags |= SF_WEBSOCKET;
    }



    return 1;
}

int http_process_response(struct stream *s, struct channel *res) {
    struct http_txn *txn = s->txn;
    http_msg_t *msg = &txn->rsp;

    if (msg->msg_state < HTTP_MSG_BODY) {
        int ret = http_msg_analyzer(msg, &res->buf);
        if (ret == 0) return 0;
        if (ret < 0) {
            txn->status = 502;
            return -1;
        }
    }

    if (msg->flags & HTTP_MSGF_CONN_CLO)
        s->flags |= SF_CONN_CLO;

    return 1;
}