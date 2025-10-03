#ifndef HTTP_HTTP_H
#define HTTP_HTTP_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "core/common.h"

struct stream;
struct channel;

#define HTTP_MSG_RQBEFORE     0x00000001
#define HTTP_MSG_RQMETH       0x00000002
#define HTTP_MSG_RQURI        0x00000004
#define HTTP_MSG_RQVER        0x00000008
#define HTTP_MSG_HDR_FIRST    0x00000010
#define HTTP_MSG_HDR_NAME     0x00000020
#define HTTP_MSG_HDR_VAL      0x00000040
#define HTTP_MSG_BODY         0x00000080
#define HTTP_MSG_CHUNK_SIZE   0x00000100
#define HTTP_MSG_CHUNK_DATA   0x00000200
#define HTTP_MSG_CHUNK_CRLF   0x00000400
#define HTTP_MSG_DONE         0x00000800
#define HTTP_MSG_ERROR        0x00001000

#define HTTP_METH_OPTIONS     0x0001
#define HTTP_METH_GET         0x0002
#define HTTP_METH_HEAD        0x0004
#define HTTP_METH_POST        0x0008
#define HTTP_METH_PUT         0x0010
#define HTTP_METH_DELETE      0x0020
#define HTTP_METH_TRACE       0x0040
#define HTTP_METH_CONNECT     0x0080
#define HTTP_METH_PATCH       0x0100

typedef struct http_msg {
    uint32_t msg_state;
    uint32_t flags;
    uint64_t chunk_len;
    uint64_t body_len;
    int32_t err_pos;

    struct {
        char *ptr;
        size_t len;
    } start_line;

    char *chn;

    struct {
        int32_t pos;
        int32_t len;
    } sol, eol;

    struct {
        int32_t pos;
        int32_t len;
    } som, eom;

    int64_t sov;
    int64_t next;

    uint32_t meth;
    char *uri;
    size_t uri_len;
} http_msg_t;

typedef struct http_txn {
    uint16_t status;
    uint32_t flags;
    uint32_t meth;

    struct http_msg req;
    struct http_msg rsp;

    char *uri;
    size_t uri_len;

    struct http_auth_data auth;

    struct {
        char *ptr;
        size_t len;
    } path;

    struct {
        int32_t cookie_first_date;
        int32_t cookie_last_date;
    } cookie;

    struct http_req_rule *rules;
} http_txn_t;

typedef struct http_hdr {
    struct {
        char *ptr;
        size_t len;
    } n;

    struct {
        char *ptr;
        size_t len;
    } v;

    struct http_hdr *next;
} http_hdr_t;

typedef struct h1_conn {
    uint32_t flags;
    struct buffer ibuf;
    struct buffer obuf;
    struct http_msg req;
    struct http_msg res;
    struct wait_event wait_event;
} h1_conn_t;

typedef struct h2_conn {
    uint32_t flags;
    uint32_t errcode;
    uint32_t last_sid;
    uint32_t max_id;
    uint32_t streams_count;

    struct {
        uint32_t initial_window;
        uint32_t max_concurrent_streams;
        uint32_t header_table_size;
        uint32_t enable_push;
        uint32_t max_frame_size;
        uint32_t max_header_list_size;
    } settings;

    struct eb_root streams_by_id;
    struct list send_list;
    struct list fctl_list;

    struct buffer dbuf;
    struct wait_event wait_event;

    struct hpack_dht *ddht;
} h2_conn_t;

typedef struct h2_stream {
    uint32_t id;
    uint32_t flags;
    uint32_t state;
    int32_t recv_window;
    int32_t send_window;

    struct eb32_node by_id;
    struct list list;

    struct buffer rxbuf;
    struct wait_event wait_event;
} h2_stream_t;

int http_msg_analyzer(http_msg_t *msg, struct buffer *buf);
int http_parse_request_line(http_msg_t *msg, char *data, size_t len);
int http_parse_status_line(http_msg_t *msg, char *data, size_t len);
int http_parse_headers(http_msg_t *msg, struct buffer *buf);
int http_parse_chunk_size(http_msg_t *msg, struct buffer *buf);

int http_process_request(struct stream *s, struct channel *req);
int http_process_response(struct stream *s, struct channel *res);
int http_process_tarpit(struct stream *s, struct channel *req);

int http_wait_for_request(struct stream *s, struct channel *req);
int http_wait_for_response(struct stream *s, struct channel *rep);

void http_txn_reset_req(http_txn_t *txn);
void http_txn_reset_res(http_txn_t *txn);

int http_header_add_tail(http_msg_t *msg, http_hdr_t *hdr);
int http_header_add(http_msg_t *msg, const char *name, const char *value);
int http_header_del(http_msg_t *msg, const char *name);
char* http_header_get(http_msg_t *msg, const char *name);

int http_replace_req_line(http_txn_t *txn, const char *line, size_t len);
int http_replace_res_line(http_txn_t *txn, const char *line, size_t len);

int http_transform_header(struct stream *s, http_msg_t *msg, const char *name,
                         const char *value, int action);

int h2_init(h2_conn_t *h2c);
int h2_recv(h2_conn_t *h2c);
int h2_send(h2_conn_t *h2c);
int h2_process(h2_conn_t *h2c);
void h2_release(h2_conn_t *h2c);

int h2_parse_frame_header(struct buffer *buf, struct h2_frame *frm);
int h2_parse_settings(h2_conn_t *h2c, struct buffer *buf);
int h2_parse_headers(h2_conn_t *h2c, struct buffer *buf);
int h2_parse_data(h2_conn_t *h2c, struct buffer *buf);

int h2_send_settings(h2_conn_t *h2c);
int h2_send_ping(h2_conn_t *h2c, uint64_t data);
int h2_send_goaway(h2_conn_t *h2c, uint32_t last_stream_id, uint32_t error_code);
int h2_send_window_update(h2_conn_t *h2c, uint32_t stream_id, uint32_t increment);
int h2_send_rst_stream(h2_conn_t *h2c, uint32_t stream_id, uint32_t error_code);

#endif