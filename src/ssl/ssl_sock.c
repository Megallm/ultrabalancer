#include "ssl/ssl.h"
#include "core/common.h"
#include "utils/log.h"
#include <string.h>
#include <openssl/rand.h>
#include <openssl/pem.h>

static int ssl_initialized = 0;

#if OPENSSL_VERSION_NUMBER < 0x30000000L
// ENGINE API is only used in OpenSSL versions before 3.0
static ENGINE *ssl_engines[32];
static int ssl_engines_count = 0;
#else
// In OpenSSL 3.0+, we don't use engines
static int ssl_engines_count = 0;
#endif

int ssl_sock_verify_cbk(int ok, X509_STORE_CTX *ctx);
void ssl_sock_info_cbk(const SSL *ssl, int where, int ret);
void ssl_sock_msg_cbk(int write_p, int version, int content_type,
                     const void *buf, size_t len, SSL *ssl, void *arg);
int ssl_sock_switchctx_cbk(SSL *ssl, int *al, void *priv);
int ssl_sock_alpn_select_cbk(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                            const unsigned char *in, unsigned int inlen, void *arg);
int ssl_sock_npn_advertise_cbk(SSL *ssl, const unsigned char **data, unsigned int *len, void *arg);

static const tls_version_t tls_versions[] = {
    {"SSLv3",    0x0002, SSL3_VERSION,    SSL3_VERSION},
    {"TLSv1.0",  0x0004, TLS1_VERSION,    TLS1_VERSION},
    {"TLSv1.1",  0x0008, TLS1_1_VERSION,  TLS1_1_VERSION},
    {"TLSv1.2",  0x0010, TLS1_2_VERSION,  TLS1_2_VERSION},
    {"TLSv1.3",  0x0020, TLS1_3_VERSION,  TLS1_3_VERSION},
    {NULL,       0,      0,               0}
};

// Get TLS version info by name
const tls_version_t* ssl_get_version_info(const char *version_name) {
    for (int i = 0; tls_versions[i].name != NULL; i++) {
        if (strcmp(tls_versions[i].name, version_name) == 0) {
            return &tls_versions[i];
        }
    }
    return NULL;
}

// Set minimum TLS version for SSL context
int ssl_ctx_set_min_version(SSL_CTX *ctx, const char *version) {
    const tls_version_t *ver = ssl_get_version_info(version);
    if (!ver) {
        log_error("Unknown TLS version: %s", version);
        return -1;
    }

    if (!SSL_CTX_set_min_proto_version(ctx, ver->min)) {
        log_error("Failed to set minimum TLS version to %s", version);
        return -1;
    }

    log_info("Set minimum TLS version to %s", version);
    return 0;
}

// Set maximum TLS version for SSL context
int ssl_ctx_set_max_version(SSL_CTX *ctx, const char *version) {
    const tls_version_t *ver = ssl_get_version_info(version);
    if (!ver) {
        log_error("Unknown TLS version: %s", version);
        return -1;
    }

    if (!SSL_CTX_set_max_proto_version(ctx, ver->max)) {
        log_error("Failed to set maximum TLS version to %s", version);
        return -1;
    }

    log_info("Set maximum TLS version to %s", version);
    return 0;
}

// Get negotiated TLS version from active connection
const char* ssl_get_negotiated_version(SSL *ssl) {
    if (!ssl) {
        return "Unknown";
    }

    int version = SSL_version(ssl);

    // Look up the version in our table
    for (int i = 0; tls_versions[i].name != NULL; i++) {
        if (tls_versions[i].min == version || tls_versions[i].max == version) {
            return tls_versions[i].name;
        }
    }

    return "Unknown";
}

int ssl_sock_init() {
    if (ssl_initialized)
        return 0;

    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    RAND_poll();

    // OpenSSL 3.0+ has deprecated ENGINE API - these are no-ops in modern versions
    // but we keep them for backward compatibility with older OpenSSL versions
#if OPENSSL_VERSION_NUMBER < 0x30000000L
    ENGINE_load_builtin_engines();
    ENGINE_register_all_complete();
#endif

    ssl_initialized = 1;
    return 0;
}

void ssl_sock_deinit() {
    if (!ssl_initialized)
        return;

    ssl_free_engines();

    EVP_cleanup();
    ERR_free_strings();
    ENGINE_cleanup();
    CRYPTO_cleanup_all_ex_data();

    ssl_initialized = 0;
}

SSL_CTX* ssl_ctx_new(ssl_bind_conf_t *conf) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        log_error("SSL_CTX_new failed");
        return NULL;
    }

    SSL_CTX_set_options(ctx, SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                             SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION |
                             SSL_OP_SINGLE_DH_USE | SSL_OP_SINGLE_ECDH_USE);

    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                          SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                          SSL_MODE_RELEASE_BUFFERS);

    SSL_CTX_set_verify(ctx, conf->verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
                      ssl_sock_verify_cbk);

    if (conf->verify_depth > 0)
        SSL_CTX_set_verify_depth(ctx, conf->verify_depth);

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_sess_set_cache_size(ctx, conf->session_cache.size);
    SSL_CTX_set_timeout(ctx, conf->session_cache.lifetime);

    SSL_CTX_set_info_callback(ctx, ssl_sock_info_cbk);
    SSL_CTX_set_msg_callback(ctx, ssl_sock_msg_cbk);

    SSL_CTX_set_tlsext_servername_callback(ctx, ssl_sock_switchctx_cbk);
    SSL_CTX_set_tlsext_servername_arg(ctx, conf);

    if (conf->alpn_str) {
        SSL_CTX_set_alpn_select_cb(ctx, ssl_sock_alpn_select_cbk, conf);
    }

    if (conf->npn_str) {
        SSL_CTX_set_next_protos_advertised_cb(ctx, ssl_sock_npn_advertise_cbk, conf);
    }

    SSL_CTX_set_session_id_context(ctx, (const unsigned char *)"UltraBalancer", 13);

    return ctx;
}

int ssl_ctx_load_cert(SSL_CTX *ctx, const char *cert, const char *key) {
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) <= 0) {
        log_error("Failed to load certificate from %s", cert);
        return -1;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) <= 0) {
        log_error("Failed to load private key from %s", key);
        return -1;
    }

    if (!SSL_CTX_check_private_key(ctx)) {
        log_error("Private key does not match certificate");
        return -1;
    }

    return 0;
}

int ssl_ctx_set_ciphers(SSL_CTX *ctx, const char *ciphers) {
    if (!SSL_CTX_set_cipher_list(ctx, ciphers)) {
        log_error("Failed to set cipher list: %s", ciphers);
        return -1;
    }
    return 0;
}

int ssl_sock_handshake(struct connection *conn, unsigned int flag) {
    ssl_sock_ctx_t *ssl_ctx = conn->xprt_ctx;
    SSL *ssl = ssl_ctx->ssl;
    int ret;

    if (conn->flags & CO_FL_CONNECTED) {
        ret = SSL_accept(ssl);
    } else {
        ret = SSL_connect(ssl);
    }

    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);

        if (err == SSL_ERROR_WANT_READ) {
            conn->flags |= CO_FL_WAIT_RD;
            return 0;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            conn->flags |= CO_FL_WAIT_WR;
            return 0;
        } else if (err == SSL_ERROR_SYSCALL) {
            conn->flags |= CO_FL_ERROR;
            return -1;
        } else {
            unsigned long e = ERR_get_error();
            char buf[256];
            ERR_error_string_n(e, buf, sizeof(buf));
            log_error("SSL handshake failed: %s", buf);
            conn->flags |= CO_FL_ERROR;
            return -1;
        }
    }

    ssl_ctx->flags |= SSL_SOCK_FL_HANDSHAKE_DONE;
    conn->flags &= ~(CO_FL_WAIT_RD | CO_FL_WAIT_WR);

    const unsigned char *alpn;
    unsigned int alpn_len;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn_len > 0) {
        ssl_ctx->alpn = strndup((char *)alpn, alpn_len);
        ssl_ctx->alpn_len = alpn_len;
    }

    const char *servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (servername) {
        ssl_ctx->sni = strdup(servername);
    }

    return 1;
}

int ssl_sock_recv(struct connection *conn, void *buf, size_t len, int flags) {
    ssl_sock_ctx_t *ssl_ctx = conn->xprt_ctx;
    SSL *ssl = ssl_ctx->ssl;
    int ret;

    ret = SSL_read(ssl, buf, len);

    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);

        if (err == SSL_ERROR_WANT_READ) {
            conn->flags |= CO_FL_WAIT_RD;
            return 0;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            conn->flags |= CO_FL_WAIT_WR;
            return 0;
        } else if (err == SSL_ERROR_ZERO_RETURN) {
            conn->flags |= CO_FL_SOCK_RD_SH;
            return 0;
        } else {
            conn->flags |= CO_FL_ERROR;
            return -1;
        }
    }

    return ret;
}

int ssl_sock_send(struct connection *conn, const void *buf, size_t len, int flags) {
    ssl_sock_ctx_t *ssl_ctx = conn->xprt_ctx;
    SSL *ssl = ssl_ctx->ssl;
    int ret;

    ret = SSL_write(ssl, buf, len);

    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);

        if (err == SSL_ERROR_WANT_WRITE) {
            conn->flags |= CO_FL_WAIT_WR;
            return 0;
        } else if (err == SSL_ERROR_WANT_READ) {
            conn->flags |= CO_FL_WAIT_RD;
            return 0;
        } else {
            conn->flags |= CO_FL_ERROR;
            return -1;
        }
    }

    return ret;
}

int ssl_sock_verify_cbk(int ok, X509_STORE_CTX *ctx) {
    SSL *ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    struct connection *conn = SSL_get_ex_data(ssl, 0);
    ssl_sock_ctx_t *ssl_ctx = conn->xprt_ctx;

    if (!ok) {
        int depth = X509_STORE_CTX_get_error_depth(ctx);
        int err = X509_STORE_CTX_get_error(ctx);
        X509 *cert = X509_STORE_CTX_get_current_cert(ctx);

        char subject[256];
        X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));

        log_warning("SSL verify error: depth=%d error=%d subject=%s",
                   depth, err, subject);

        if (depth == 0) {
            ssl_ctx->flags |= SSL_SOCK_ST_FL_VERIFY_DONE;
        }
    }

    return ok;
}

void ssl_sock_info_cbk(const SSL *ssl, int where, int ret) {
    // Connection tracking for future use
    (void)SSL_get_ex_data(ssl, 0);

    if (where & SSL_CB_HANDSHAKE_START) {
        log_debug("SSL handshake started");
    } else if (where & SSL_CB_HANDSHAKE_DONE) {
        log_debug("SSL handshake completed");
    } else if (where & SSL_CB_ALERT) {
        const char *str = SSL_alert_type_string_long(ret);
        const char *desc = SSL_alert_desc_string_long(ret);

        if (where & SSL_CB_READ) {
            log_debug("SSL alert received: %s:%s", str, desc);
        } else if (where & SSL_CB_WRITE) {
            log_debug("SSL alert sent: %s:%s", str, desc);
        }
    }
}

int ssl_sock_switchctx_cbk(SSL *ssl, int *al, void *priv) {
    ssl_bind_conf_t *conf = priv;
    const char *servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

    if (!servername)
        return SSL_TLSEXT_ERR_NOACK;

    for (int i = 0; i < conf->sni_ctx_count; i++) {
        if (strcasecmp(servername, conf->sni_ctx[i].cert) == 0) {
            SSL_set_SSL_CTX(ssl, conf->sni_ctx[i].ctx);
            return SSL_TLSEXT_ERR_OK;
        }
    }

    return SSL_TLSEXT_ERR_NOACK;
}

int ssl_init_single_engine(const char *engine_name) {
#if OPENSSL_VERSION_NUMBER < 0x30000000L
    // ENGINE API is deprecated in OpenSSL 3.0+
    ENGINE *engine = ENGINE_by_id(engine_name);

    if (!engine) {
        log_error("Failed to load engine: %s", engine_name);
        return -1;
    }

    if (!ENGINE_init(engine)) {
        log_error("Failed to initialize engine: %s", engine_name);
        ENGINE_free(engine);
        return -1;
    }

    if (!ENGINE_set_default(engine, ENGINE_METHOD_ALL)) {
        log_error("Failed to set engine as default: %s", engine_name);
        ENGINE_finish(engine);
        ENGINE_free(engine);
        return -1;
    }

    ssl_engines[ssl_engines_count++] = engine;
    log_info("SSL engine %s loaded successfully", engine_name);

    return 0;
#else
    // In OpenSSL 3.0+, use providers instead of engines
    log_warning("ENGINE API is deprecated in OpenSSL 3.0+, engine '%s' not loaded", engine_name);
    return -1;
#endif
}

void ssl_free_engines() {
#if OPENSSL_VERSION_NUMBER < 0x30000000L
    // ENGINE API is deprecated in OpenSSL 3.0+
    for (int i = 0; i < ssl_engines_count; i++) {
        ENGINE_finish(ssl_engines[i]);
        ENGINE_free(ssl_engines[i]);
    }
#endif
    ssl_engines_count = 0;
}

void ssl_sock_msg_cbk(int write_p, int version, int content_type,
                     const void *buf, size_t len, SSL *ssl, void *arg) {
}

int ssl_sock_alpn_select_cbk(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                            const unsigned char *in, unsigned int inlen, void *arg) {
    *out = in;
    *outlen = inlen > 0 ? 1 : 0;
    return SSL_TLSEXT_ERR_OK;
}

int ssl_sock_npn_advertise_cbk(SSL *ssl, const unsigned char **data, unsigned int *len, void *arg) {
    *data = (const unsigned char *)"";
    *len = 0;
    return SSL_TLSEXT_ERR_OK;
}