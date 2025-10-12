#ifndef SSL_SSL_H
#define SSL_SSL_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/dh.h>
#include <openssl/engine.h>
#include <openssl/ocsp.h>
#include "core/common.h"

struct server;

#define SSL_SOCK_ST_FL_VERIFY_DONE  0x00000001
#define SSL_SOCK_ST_FL_16K_WBFSIZE  0x00000002
#define SSL_SOCK_SEND_UNLIMITED      0x00000004
#define SSL_SOCK_RECV_HEARTBEAT      0x00000008

#define SSL_SOCK_FL_SSL_STARTED      0x00000001
#define SSL_SOCK_FL_HANDSHAKE_DONE   0x00000002

typedef struct ssl_bind_conf {
    SSL_CTX *ctx;
    char *ciphers;
    char *curves;
    char *ecdhe;
    char *ca_file;
    char *ca_path;
    char *crl_file;
    char *cert;
    char *key;
    char *npn_str;
    char *alpn_str;

    int verify;
    int verify_depth;

    struct {
        char *cert;
        char *key;
        char *ocsp;
        SSL_CTX *ctx;
        struct certificate *chain;
    } *sni_ctx;
    int sni_ctx_count;

    DH *dh_params;

    struct {
        unsigned int lifetime;
        unsigned int size;
    } session_cache;

    struct ssl_bind_conf *next;
} ssl_bind_conf_t;

typedef struct ssl_sock_ctx {
    struct connection *conn;
    SSL *ssl;
    BIO *bio;

    unsigned int flags;
    int send_cnt;
    int recv_cnt;

    struct {
        char *ptr;
        int len;
    } early_data;

    const char *sni;
    const char *alpn;
    int alpn_len;

    void (*info_callback)(const SSL *ssl, int where, int ret);

    struct wait_event wait_event;
    struct wait_event *recv_wait;
    struct wait_event *send_wait;
} ssl_sock_ctx_t;

typedef struct tls_version {
    const char *name;
    int flag;
    int min;
    int max;
} tls_version_t;

typedef struct ssl_methods {
    const char *name;
    const SSL_METHOD *method;
    int flags;
} ssl_methods_t;

int ssl_sock_init();
void ssl_sock_deinit();

SSL_CTX* ssl_ctx_new(ssl_bind_conf_t *conf);
void ssl_ctx_free(SSL_CTX *ctx);

int ssl_ctx_load_cert(SSL_CTX *ctx, const char *cert, const char *key);
int ssl_ctx_load_ca(SSL_CTX *ctx, const char *ca_file, const char *ca_path);
int ssl_ctx_set_ciphers(SSL_CTX *ctx, const char *ciphers);
int ssl_ctx_set_curves(SSL_CTX *ctx, const char *curves);
int ssl_ctx_set_dh_params(SSL_CTX *ctx, DH *dh);

int ssl_sock_handshake(struct connection *conn, unsigned int flag);
int ssl_sock_recv(struct connection *conn, void *buf, size_t len, int flags);
int ssl_sock_send(struct connection *conn, const void *buf, size_t len, int flags);
int ssl_sock_close(struct connection *conn);

int ssl_sock_get_alpn(struct connection *conn, const char **str, int *len);
const char* ssl_sock_get_sni(struct connection *conn);
int ssl_sock_get_cert_used(struct connection *conn);
X509* ssl_sock_get_peer_cert(struct connection *conn);

int ssl_sock_set_alpn(SSL *ssl, const unsigned char *alpn, unsigned int len);
int ssl_sock_set_servername(SSL *ssl, const char *hostname);

int ssl_sock_switchctx_cbk(SSL *ssl, int *al, void *priv);
int ssl_sock_sess_new_cbk(SSL *ssl, SSL_SESSION *sess);
SSL_SESSION* ssl_sock_sess_get_cbk(SSL *ssl, const unsigned char *id, int len, int *copy);
void ssl_sock_sess_remove_cbk(SSL_CTX *ctx, SSL_SESSION *sess);

int ssl_sock_verify_cbk(int ok, X509_STORE_CTX *ctx);
void ssl_sock_info_cbk(const SSL *ssl, int where, int ret);
void ssl_sock_msg_cbk(int write_p, int version, int content_type,
                      const void *buf, size_t len, SSL *ssl, void *arg);

int ssl_sock_load_ocsp(SSL_CTX *ctx, const char *ocsp_file);
int ssl_sock_get_ocsp_status(SSL *ssl);
int ssl_sock_update_ocsp(SSL_CTX *ctx, const char *ocsp_response, int len);

int ssl_sock_prepare_ctx(ssl_bind_conf_t *conf);
int ssl_sock_prepare_srv_ctx(struct server *srv);

void ssl_sock_free_all_ctx(ssl_bind_conf_t *conf);

int ssl_init_single_engine(const char *engine_name);
void ssl_free_engines();

// TLS version negotiation utilities - new feature for SSL/TLS control
const tls_version_t* ssl_get_version_info(const char *version_name);
int ssl_ctx_set_min_version(SSL_CTX *ctx, const char *version);
int ssl_ctx_set_max_version(SSL_CTX *ctx, const char *version);
const char* ssl_get_negotiated_version(SSL *ssl);

#endif