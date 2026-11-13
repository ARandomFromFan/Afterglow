/* nn::ssl over OpenSSL.

   Two layers live here:
   1. nn::ssl::BuiltInManager (below): the CA certificates the game would get from the console's built-in
      store. The SDK's SSL setup wants a root CA; offline nothing is served (0xca7b, the same error the game
      already handles), online the host serves the playdb CA that signed the npln stub's server certificate
      (pki/ca.der, HOS_NPLN_CA overrides).
   2. the nnssl* C API (nn::ssl::Connection's inline wrappers) and nn::ssl::Context::Create/Import*: the
      game's TLS client. The SDK bodies in code_061.c talk to the console's ssl service, which does not exist
      here (every command was answered empty), so the TLS runs in the host instead, on the game's own socket:
        - guest fd == host fd (host/nn_socket.c), SetSocketDescriptor hands us that fd and we never close it,
        - our own test CA only (pki/ca.der, HOS_NPLN_CA): no system store is loaded and verification is
          never disabled globally - SetVerifyOption decides per connection (bit0 peer, bit1 host name),
        - the SDK has no "would block" Result (its client blocks inside the service IPC), so the handshake
          and Read/Write/Peek block in bounded poll() loops (HOS_SSL_TIMEOUT_MS, default 30000 ms) and
          report 0xffffda7b on timeout,
        - one "ssl: ..." log line per call type on first use, and one line per handshake with host, verify
          bits, result, TLS version, cipher, ALPN and the peer's CN. No keys or tokens are logged. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#define SSL_HANDLE_MAGIC_CTX 0x50000000u   /* stored as the u64 the game keeps at the object's byte 0 */
#define SSL_HANDLE_MAGIC_CONN 0x51000000u
#define SSL_SLOT_MAX 16
#define SSL_GUEST_PTR_MIN 0x1000000ULL     /* guest heap/stack pointers are far above any buffer size */
#define SSL_HOST_MAX 256
#define SSL_ALPN_MAX 64
#define SSL_R_INVALID 0xE27Bu              /* what the SDK returns for a null/invalid object */
#define SSL_R_ZERO_SIZE 0xE07Bu
#define SSL_R_NOT_CREATED 0xDC7Bu
#define SSL_R_ALLOC 0xCC7Bu
#define SSL_R_DEAD 0x0000DA7Bu             /* our own: the connection is gone, the I/O failed or timed out */

#define SSL_CA_MAX 16384
#define SSL_CA_DEFAULT "pki/ca.der"   /* relative to the working directory */
#define SSL_ID_ALL 0xFFFFFFFFu
#define SSL_CERT_INFO_SIZE 24   /* u32 id, u32 status, u64 size, u64 data */

static uint8_t s_ca[SSL_CA_MAX];
static size_t s_ca_len;
static int s_ca_tried;
static pthread_once_t s_ca_once = PTHREAD_ONCE_INIT;
static int s_logged_size, s_logged_served;

static void ssl_ca_load(void) {
    const char *path = getenv("HOS_NPLN_CA");
    if (!path || !*path) path = SSL_CA_DEFAULT;
    FILE *f = fopen(path, "rb");
    s_ca_tried = 1;
    if (!f) {
        hos_log("ssl: no CA at %s", path);
        return;
    }
    s_ca_len = fread(s_ca, 1, sizeof(s_ca), f);
    fclose(f);
    if (!s_ca_len) {
        hos_log("ssl: no CA at %s", path);
        return;
    }
    hos_log("ssl: loaded CA %s (%zu bytes)", path, s_ca_len);
}

/* the CA, or 0 when there is none (offline or no file): load it once */
static const uint8_t *ssl_ca(size_t *len) {
    pthread_once(&s_ca_once, ssl_ca_load);
    if (!s_ca_tried || !s_ca_len) return NULL;
    *len = s_ca_len;
    return s_ca;
}

/* how many certificate entries the request asks for: one for CaCertificateId::All, else one per id */
static uint32_t ssl_cert_count(const uint32_t *ids, uint32_t idCount) {
    for (uint32_t i = 0; i < idCount; i++)
        if (ids[i] == SSL_ID_ALL) return 1;
    return idCount;
}

/* GetBuiltInCertificateBufSize(u32* outSize, const CaCertificateId* ids, u32 idCount) */
HLE(_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj) {
    const uint64_t outSize = ARG(0), ids = ARG(1);
    const uint32_t idCount = (uint32_t)ARG(2);
    if (!outSize || (!ids && idCount)) HLE_RETURN(0xE27B);
    if (!idCount) HLE_RETURN(0xFE7B);
    if (!hos_force_online()) HLE_RETURN(0xCA7B);
    size_t ca_len;
    if (!ssl_ca(&ca_len)) HLE_RETURN(0xCA7B);
    const uint32_t n = ssl_cert_count((const uint32_t *)GP(ids), idCount);
    const uint32_t size = SSL_CERT_INFO_SIZE * n + (uint32_t)ca_len;
    G32(outSize) = size;
    if (!s_logged_size++) hos_log("ssl: builtin certs bufsize n=%u ca=%zu", n, ca_len);
    RET(0);
}

/* GetBuiltInCertificates(BuiltInCertificateInfoBase** outInfo, u8* buf, u32 bufSize, const CaCertificateId* ids,
   u32 idCount): the info array and the CA bytes both go into the caller's buffer, the CA after the info array */
HLE(_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj) {
    const uint64_t outInfo = ARG(0), buf = ARG(1), ids = ARG(3);
    const uint32_t bufSize = (uint32_t)ARG(2), idCount = (uint32_t)ARG(4);
    if (!outInfo || !buf || (!ids && idCount)) HLE_RETURN(0xE27B);
    if (!bufSize) HLE_RETURN(0xE07B);
    if (!idCount) HLE_RETURN(0xFE7B);
    if (!hos_force_online()) HLE_RETURN(0xCA7B);
    size_t ca_len;
    const uint8_t *ca = ssl_ca(&ca_len);
    if (!ca) HLE_RETURN(0xCA7B);
    const uint32_t *idp = (const uint32_t *)GP(ids);
    const uint32_t n = ssl_cert_count(idp, idCount);
    const uint32_t needed = SSL_CERT_INFO_SIZE * n + (uint32_t)ca_len;
    if (bufSize < needed) {
        hos_log("ssl: builtin certs buffer too small: %u < %u", bufSize, needed);
        HLE_RETURN(0xCA7B);
    }
    char idtext[4 * 9 + 1];
    idtext[0] = 0;
    for (uint32_t i = 0; i < n && i < 4; i++) {
        char one[10];
        snprintf(one, sizeof(one), "%s%x", i ? "," : "", (n == 1 && idCount != 1) ? 1u : idp[i]);
        strncat(idtext, one, sizeof(idtext) - strlen(idtext) - 1);
    }
    const uint64_t data = buf + SSL_CERT_INFO_SIZE * n;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t info = buf + SSL_CERT_INFO_SIZE * i;
        G32(info + 0) = (n == 1 && idCount != 1) ? 1u : idp[i];
        G32(info + 4) = 1;   /* BuiltinDataStatus::Loaded */
        G64(info + 8) = ca_len;
        G64(info + 16) = data;
    }
    memcpy(GP(data), ca, ca_len);
    G64(outInfo) = buf;
    if (!s_logged_served++) hos_log("ssl: builtin certs served n=%u ids=%s", n, idtext);
    RET(0);
}


/* ---- tables --------------------------------------------------------------------------------- */
typedef struct {
    int used;
    SSL_CTX *ctx;
    uint32_t verify;
    uint32_t version;
    uint64_t id;
} SslCtxSlot;

typedef struct {
    int used;
    SSL *ssl;
    int fd;
    char host[SSL_HOST_MAX];
    uint32_t verify;
    uint32_t io_mode;
    uint32_t option[4];
    uint32_t session_cache_mode;
    uint64_t scert_addr;
    uint32_t scert_size;
    unsigned char alpn_wire[SSL_ALPN_MAX];   /* the ALPN list in wire form: one length byte per protocol */
    size_t alpn_wire_len;
    long verify_result;
    uint64_t id;
} SslConnSlot;

static SslCtxSlot s_ctx[SSL_SLOT_MAX];
static SslConnSlot s_conn[SSL_SLOT_MAX];
static int s_conn_by_fd[1024];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_openssl_ready;
static int s_logged[32];
static uint32_t s_next_id = 1;

#define SSL_LOG_ONCE(n, ...) do { if (!s_logged[n]) { s_logged[n] = 1; hos_log(__VA_ARGS__); } } while (0)

static void ssl_global_init(void) {
    pthread_mutex_lock(&s_lock);
    if (!s_openssl_ready) {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
        for (int i = 0; i < 1024; i++) s_conn_by_fd[i] = -1;
        s_openssl_ready = 1;
    }
    pthread_mutex_unlock(&s_lock);
}

/* the game keeps the handle we wrote in the object's first 8 bytes and hands the object back to us */
static uint64_t ssl_guest_handle(uint64_t obj) { return obj ? G64(obj) : 0; }

static int ssl_handle_index(uint64_t h, uint32_t magic) {
    if (h >> 32) return -1;
    uint32_t v = (uint32_t)h;
    if ((v & 0xFF000000u) != magic) return -1;
    uint32_t idx = v & 0x00FFFFFFu;
    if (idx >= SSL_SLOT_MAX) return -1;
    return (int)idx;
}

static void ssl_set_handle(uint64_t obj, uint32_t magic, int idx) { G64(obj) = (uint64_t)(magic | (uint32_t)idx); }

static SslCtxSlot *ssl_ctx_lookup(uint64_t obj) {
    int i = ssl_handle_index(ssl_guest_handle(obj), SSL_HANDLE_MAGIC_CTX);
    return (i >= 0 && s_ctx[i].used) ? &s_ctx[i] : NULL;
}

static SslConnSlot *ssl_conn_lookup(uint64_t obj) {
    int i = ssl_handle_index(ssl_guest_handle(obj), SSL_HANDLE_MAGIC_CONN);
    return (i >= 0 && s_conn[i].used) ? &s_conn[i] : NULL;
}

static int ssl_timeout_ms(void) {
    const char *e = getenv("HOS_SSL_TIMEOUT_MS");
    int ms = (e && *e) ? atoi(e) : 0;
    return ms > 0 ? ms : 30000;
}

static int64_t ssl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ssl_log_ssl_error(const char *what) {
    unsigned long e = ERR_get_error();
    char buf[256];
    if (!e) { hos_log("ssl: %s: no openssl error queued", what); return; }
    ERR_error_string_n(e, buf, sizeof buf);
    hos_log("ssl: %s: %s", what, buf);
}

static int ssl_set_nonblock(int fd, int *saved) {
    *saved = fcntl(fd, F_GETFL, 0);
    if (*saved < 0) return -1;
    if ((*saved & O_NONBLOCK) == 0 && fcntl(fd, F_SETFL, *saved | O_NONBLOCK) < 0) return -1;
    return 0;
}

static void ssl_restore_flags(int fd, int saved) {
    if (saved >= 0 && (saved & O_NONBLOCK) == 0) fcntl(fd, F_SETFL, saved);
}

/* the SDK's Read/Write/Peek take (conn, buf, u32* outCount, u32 size) but the lifted bodies only prove
   arg0, so pick the out-count apart from the size by range: a guest pointer, never a buffer size */
static void ssl_split_count_size(uint64_t a2, uint64_t a3, uint64_t *outp, uint32_t *sizep, int *swapped) {
    *swapped = 0;
    if (a2 >= SSL_GUEST_PTR_MIN && a3 < SSL_GUEST_PTR_MIN) { *outp = a2; *sizep = (uint32_t)a3; return; }
    if (a3 >= SSL_GUEST_PTR_MIN && a2 < SSL_GUEST_PTR_MIN) { *outp = a3; *sizep = (uint32_t)a2; *swapped = 1; return; }
    if (a2 >= SSL_GUEST_PTR_MIN) { *outp = a2; *sizep = (uint32_t)a3; return; }
    *outp = 0;
    *sizep = (uint32_t)(a2 ? a2 : a3);
}

static SSL_CTX *ssl_ctx_new(uint32_t version, int *have_ca) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    *have_ca = 0;
    if (!ctx) { ssl_log_ssl_error("SSL_CTX_new"); return NULL; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);   /* per connection, from SetVerifyOption */
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    size_t ca_len = 0;
    const uint8_t *ca = ssl_ca(&ca_len);
    if (ca && ca_len) {
        const unsigned char *p = ca;
        X509 *x = d2i_X509(NULL, &p, (long)ca_len);
        if (x) {
            if (X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), x) == 1) *have_ca = 1;
            else { ERR_clear_error(); }   /* already present */
            X509_free(x);
        } else {
            ssl_log_ssl_error("d2i_X509(pki/ca.der)");
        }
    }
    (void)version;   /* SslVersion only selects the protocol range: TLS 1.2 and up */
    return ctx;
}

/* SNI, name check and peer verification for one connection: bit0 peer, bit1 host name */
static void ssl_apply_verify(SslConnSlot *cn) {
    SSL_set_verify(cn->ssl, (cn->verify & 1u) ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    if (cn->host[0]) SSL_set_tlsext_host_name(cn->ssl, cn->host);
    if ((cn->verify & 2u) && cn->host[0]) {
        X509_VERIFY_PARAM *param = SSL_get0_param(cn->ssl);
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (X509_VERIFY_PARAM_set1_host(param, cn->host, 0) != 1) ssl_log_ssl_error("X509_VERIFY_PARAM_set1_host");
    }
}

static void ssl_apply_alpn(SslConnSlot *cn) {
    if (!cn->alpn_wire_len) return;
    if (SSL_set_alpn_protos(cn->ssl, cn->alpn_wire, (unsigned int)cn->alpn_wire_len) != 0)
        ssl_log_ssl_error("SSL_set_alpn_protos");
}

static void ssl_peer_name(SSL *ssl, char *out, size_t out_size) {
    out[0] = 0;
    X509 *peer = SSL_get1_peer_certificate(ssl);
    if (!peer) return;
    X509_NAME *name = X509_get_subject_name(peer);
    if (name) X509_NAME_get_text_by_NID(name, NID_commonName, out, (int)out_size);
    X509_free(peer);
}


/* ---- bounded I/O on the game's socket ------------------------------------------------------- */
static int ssl_io_connect(SSL *ssl, int fd) {
    const int64_t deadline = ssl_now_ms() + ssl_timeout_ms();
    for (;;) {
        ERR_clear_error();
        int r = SSL_connect(ssl);
        if (r == 1) return 1;
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;
        short ev = err == SSL_ERROR_WANT_READ ? POLLIN : err == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
        if (!ev) return -1;
        int64_t left = deadline - ssl_now_ms();
        if (left <= 0) return -2;
        struct pollfd p = { .fd = fd, .events = ev, .revents = 0 };
        int pr = poll(&p, 1, (int)left);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (pr == 0) return -2;
    }
}

/* SSL_read / SSL_peek with the same bounded wait; returns the byte count, 0 on EOF, -1 error, -2 timeout */
static int ssl_io_read(SSL *ssl, int fd, void *buf, uint32_t size, int peek) {
    const int64_t deadline = ssl_now_ms() + ssl_timeout_ms();
    for (;;) {
        ERR_clear_error();
        int r = peek ? SSL_peek(ssl, buf, (int)size) : SSL_read(ssl, buf, (int)size);
        if (r > 0) return r;
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;
        short ev = err == SSL_ERROR_WANT_READ ? POLLIN : err == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
        if (!ev) return -1;
        int64_t left = deadline - ssl_now_ms();
        if (left <= 0) return -2;
        struct pollfd p = { .fd = fd, .events = ev, .revents = 0 };
        int pr = poll(&p, 1, (int)left);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (pr == 0) return -2;
    }
}

static int ssl_io_write(SSL *ssl, int fd, const void *buf, uint32_t size) {
    const int64_t deadline = ssl_now_ms() + ssl_timeout_ms();
    for (;;) {
        ERR_clear_error();
        int r = SSL_write(ssl, buf, (int)size);
        if (r > 0) return r;
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;
        short ev = err == SSL_ERROR_WANT_READ ? POLLIN : err == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
        if (!ev) return -1;
        int64_t left = deadline - ssl_now_ms();
        if (left <= 0) return -2;
        struct pollfd p = { .fd = fd, .events = ev, .revents = 0 };
        int pr = poll(&p, 1, (int)left);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (pr == 0) return -2;
    }
}

/* one "ssl: handshake ..." line per handshake: host, verify bits, result, version, cipher, peer CN */
static void ssl_log_handshake(SslConnSlot *cn, const char *kind, int ok, int code) {
    char cn_name[128];
    ssl_peer_name(cn->ssl, cn_name, sizeof cn_name);
    char cipher[64] = "";
    if (ok > 0) {
        const SSL_CIPHER *c = SSL_get_current_cipher(cn->ssl);
        if (c) snprintf(cipher, sizeof cipher, "%s", SSL_CIPHER_get_name(c));
    }
    const unsigned char *proto = NULL;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected(cn->ssl, &proto, &proto_len);
    char alpn[32] = "";
    if (proto && proto_len && proto_len < sizeof alpn) { memcpy(alpn, proto, proto_len); alpn[proto_len] = 0; }
    hos_log("ssl: handshake %s host=%s verify=%u result=%s code=%#x tls=%s cipher=%s alpn=%s peer=%s vres=%ld",
            kind, cn->host[0] ? cn->host : "-", cn->verify,
            ok > 0 ? "ok" : (ok == -2 ? "timeout" : "failed"), code, ok > 0 ? SSL_get_version(cn->ssl) : "-", cipher,
            alpn[0] ? alpn : "-", cn_name[0] ? cn_name : "-", cn->verify_result);
}

static uint32_t ssl_do_handshake(SslConnSlot *cn, const char *kind, uint64_t a1, uint64_t a2) {
    if (!cn->ssl) return SSL_R_DEAD;
    if (cn->fd < 0) {
        hos_log("ssl: handshake %s without a socket descriptor (host=%s)", kind, cn->host[0] ? cn->host : "-");
        return SSL_R_INVALID;
    }
    ssl_apply_verify(cn);
    ssl_apply_alpn(cn);
    SSL_set_connect_state(cn->ssl);
    int saved = -1;
    if (ssl_set_nonblock(cn->fd, &saved) != 0) {
        hos_log("ssl: handshake %s: fcntl(fd=%d) failed: %s", kind, cn->fd, strerror(errno));
        return SSL_R_DEAD;
    }
    int r = ssl_io_connect(cn->ssl, cn->fd);
    ssl_restore_flags(cn->fd, saved);
    cn->verify_result = SSL_get_verify_result(cn->ssl);
    if (r != 1) {
        ssl_log_ssl_error(r == -2 ? "handshake timeout" : "handshake");
        ssl_log_handshake(cn, kind, r == -2 ? -2 : -1, SSL_R_DEAD);
        (void)a1; (void)a2;
        return SSL_R_DEAD;
    }
    ssl_log_handshake(cn, kind, 1, 0);
    return 0;
}


/* ---- nn::ssl entry points ------------------------------------------------------------------ */
HLE(_ZN2nn3ssl10InitializeEv) {
    ssl_global_init();
    SSL_LOG_ONCE(0, "ssl: Initialize -> 0");
    RET(0);
}

HLE(_ZN2nn3ssl8FinalizeEv) {
    SSL_LOG_ONCE(1, "ssl: Finalize -> 0");
    RET(0);
}

HLE(nnsslInitialize) { ssl_global_init(); SSL_LOG_ONCE(2, "ssl: nnsslInitialize -> 0"); RET(0); }
HLE(nnsslFinalize) { SSL_LOG_ONCE(3, "ssl: nnsslFinalize -> 0"); RET(0); }

/* Context::Create(SslVersion): x0 = the game's context object, its first 8 bytes become our handle */
HLE(_ZN2nn3ssl7Context6CreateENS1_10SslVersionE) {
    const uint64_t obj = ARG(0);
    const uint32_t version = (uint32_t)ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    ssl_global_init();
    int have_ca = 0;
    SSL_CTX *ctx = ssl_ctx_new(version, &have_ca);
    if (!ctx) HLE_RETURN(0xCC7B);   /* what the SDK returns when its allocation fails */
    pthread_mutex_lock(&s_lock);
    int idx = -1;
    for (int i = 0; i < SSL_SLOT_MAX; i++)
        if (!s_ctx[i].used) { idx = i; break; }
    if (idx < 0) {
        pthread_mutex_unlock(&s_lock);
        SSL_CTX_free(ctx);
        hos_log("ssl: Context::Create: no free context slot (max %d)", SSL_SLOT_MAX);
        HLE_RETURN(0xCC7B);
    }
    s_ctx[idx].used = 1;
    s_ctx[idx].ctx = ctx;
    s_ctx[idx].verify = 0;
    s_ctx[idx].version = version;
    s_ctx[idx].id = s_next_id++;
    ssl_set_handle(obj, SSL_HANDLE_MAGIC_CTX, idx);
    pthread_mutex_unlock(&s_lock);
    SSL_LOG_ONCE(4, "ssl: Context::Create(version %#x) idx %d id %llu ca=%s (own pki only, no system store)",
                 version, idx, (unsigned long long)s_ctx[idx].id, have_ca ? "pki/ca.der" : "none/verify-none");
    RET(0);
}

HLE(nnsslContextGetContextId) {
    const uint64_t obj = ARG(0), out = ARG(1);
    if (!obj || !out) HLE_RETURN(SSL_R_INVALID);
    SslCtxSlot *slot = NULL;
    pthread_mutex_lock(&s_lock);
    slot = ssl_ctx_lookup(obj);
    uint64_t id = slot ? slot->id : 0;
    pthread_mutex_unlock(&s_lock);
    G64(out) = id;
    SSL_LOG_ONCE(5, "ssl: nnsslContextGetContextId -> %llu", (unsigned long long)id);
    RET(0);
}

HLE(nnsslContextDestroy) {
    const uint64_t obj = ARG(0);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    pthread_mutex_lock(&s_lock);
    int i = ssl_handle_index(ssl_guest_handle(obj), SSL_HANDLE_MAGIC_CTX);
    SSL_CTX *ctx = (i >= 0 && s_ctx[i].used) ? s_ctx[i].ctx : NULL;
    if (i >= 0) { memset(&s_ctx[i], 0, sizeof s_ctx[i]); }
    G64(obj) = 0;
    pthread_mutex_unlock(&s_lock);
    if (ctx) SSL_CTX_free(ctx);
    SSL_LOG_ONCE(6, "ssl: nnsslContextDestroy idx %d", i);
    RET(0);
}

HLE(_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE) {
    const uint64_t obj = ARG(0), out_id = ARG(1), data = ARG(2);
    const uint32_t size = (uint32_t)ARG(3), format = (uint32_t)ARG(4);
    /* a server certificate for the game's own listener: keep it for verification, do not fail the call */
    int added = 0;
    pthread_mutex_lock(&s_lock);
    SslCtxSlot *slot = ssl_ctx_lookup(obj);
    if (slot && data && size) {
        X509 *x = NULL;
        if (format == 2 /* Der */) { const unsigned char *p = (const unsigned char *)GP(data); x = d2i_X509(NULL, &p, (long)size); }
        else { BIO *b = BIO_new_mem_buf(GP(data), (int)size); if (b) { x = PEM_read_bio_X509(b, NULL, NULL, NULL); BIO_free(b); } }
        if (x) { added = X509_STORE_add_cert(SSL_CTX_get_cert_store(slot->ctx), x) == 1; X509_free(x); ERR_clear_error(); }
    }
    uint64_t id = 0;
    if (slot) id = slot->id;
    pthread_mutex_unlock(&s_lock);
    if (out_id) G64(out_id) = id;
    SSL_LOG_ONCE(7, "ssl: ImportServerPki size=%u format=%u added=%d -> 0", size, format, added);
    RET(0);
}

HLE(_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj) {
    const uint64_t obj = ARG(0), out_id = ARG(1), cert = ARG(2), key = ARG(3);
    const uint32_t cert_size = (uint32_t)ARG(4), key_size = (uint32_t)ARG(5);
    uint64_t id = 0;
    pthread_mutex_lock(&s_lock);
    SslCtxSlot *slot = ssl_ctx_lookup(obj);
    if (slot) id = slot->id;
    pthread_mutex_unlock(&s_lock);
    if (out_id) G64(out_id) = id;
    SSL_LOG_ONCE(8, "ssl: ImportClientPki cert=%u key=%u (accepted, client auth not used over our CA) -> 0",
                 cert_size, key_size);
    (void)cert; (void)key; (void)key_size;
    RET(0);
}

HLE(_ZN2nn3ssl7Context9ImportCrlEPmPKcj) {
    const uint64_t obj = ARG(0), out_id = ARG(1);
    const uint32_t size = (uint32_t)ARG(3);
    uint64_t id = 0;
    pthread_mutex_lock(&s_lock);
    SslCtxSlot *slot = ssl_ctx_lookup(obj);
    if (slot) id = slot->id;
    pthread_mutex_unlock(&s_lock);
    if (out_id) G64(out_id) = id;
    SSL_LOG_ONCE(9, "ssl: ImportCrl size=%u (accepted, revocation not checked) -> 0", size);
    RET(0);
}


/* ---- connections --------------------------------------------------------------------------- */
HLE(nnsslConnectionCreate) {
    const uint64_t obj = ARG(0), ctx_obj = ARG(1);
    if (!obj || !ctx_obj) HLE_RETURN(SSL_R_INVALID);
    ssl_global_init();
    pthread_mutex_lock(&s_lock);
    SslCtxSlot *cslot = ssl_ctx_lookup(ctx_obj);
    if (!cslot) {
        pthread_mutex_unlock(&s_lock);
        hos_log("ssl: nnsslConnectionCreate: the context object has no handle (x1=%#llx)", (unsigned long long)ctx_obj);
        HLE_RETURN(0xDC7B);   /* impl object not created */
    }
    SSL *ssl = SSL_new(cslot->ctx);
    int idx = -1;
    for (int i = 0; i < SSL_SLOT_MAX; i++)
        if (!s_conn[i].used) { idx = i; break; }
    if (!ssl || idx < 0) {
        pthread_mutex_unlock(&s_lock);
        if (ssl) SSL_free(ssl);
        hos_log("ssl: nnsslConnectionCreate: no free connection slot%s", ssl ? "" : " (SSL_new failed)");
        HLE_RETURN(0xCC7B);
    }
    SslConnSlot *cn = &s_conn[idx];
    memset(cn, 0, sizeof *cn);
    cn->used = 1;
    cn->ssl = ssl;
    cn->fd = -1;
    cn->verify = cslot->verify;
    cn->id = s_next_id++;
    ssl_set_handle(obj, SSL_HANDLE_MAGIC_CONN, idx);
    pthread_mutex_unlock(&s_lock);
    SSL_LOG_ONCE(10, "ssl: nnsslConnectionCreate idx %d id %llu -> 0", idx, (unsigned long long)cn->id);
    RET(0);
}

HLE(nnsslConnectionDestroy) {
    const uint64_t obj = ARG(0);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    pthread_mutex_lock(&s_lock);
    int i = ssl_handle_index(ssl_guest_handle(obj), SSL_HANDLE_MAGIC_CONN);
    SSL *ssl = (i >= 0 && s_conn[i].used) ? s_conn[i].ssl : NULL;
    int fd = (i >= 0) ? s_conn[i].fd : -1;
    if (i >= 0) {
        memset(&s_conn[i], 0, sizeof s_conn[i]);
        if (fd > 0 && fd < 1024) s_conn_by_fd[fd] = -1;
    }
    G64(obj) = 0;
    pthread_mutex_unlock(&s_lock);
    if (ssl) SSL_free(ssl);   /* the BIO is BIO_NOCLOSE: the game's socket stays open */
    SSL_LOG_ONCE(11, "ssl: nnsslConnectionDestroy idx %d (socket left to the game)", i);
    RET(0);
}

HLE(nnsslConnectionGetConnectionId) {
    const uint64_t obj = ARG(0), out = ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    uint64_t id = 0;
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) id = cn->id;
    pthread_mutex_unlock(&s_lock);
    if (out) G64(out) = id;
    SSL_LOG_ONCE(12, "ssl: nnsslConnectionGetConnectionId -> %llu", (unsigned long long)id);
    RET(0);
}

HLE(nnsslConnectionSetSocketDescriptor) {
    const uint64_t obj = ARG(0);
    const int fd = (int)ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) {
        cn->fd = fd;
        if (fd >= 0 && fd < 1024) s_conn_by_fd[fd] = (int)(cn - s_conn);
        if (SSL_set_fd(cn->ssl, fd) != 1) ssl_log_ssl_error("SSL_set_fd");
    }
    pthread_mutex_unlock(&s_lock);
    hos_log("ssl: SetSocketDescriptor(fd=%d) -> 0 (guest fd == host fd)", fd);
    RET(0);
}

HLE(nnsslConnectionSetHostName) {
    const uint64_t obj = ARG(0), name = ARG(1);
    const uint32_t len = (uint32_t)ARG(2);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    char host[SSL_HOST_MAX];
    host[0] = 0;
    if (name) {
        const char *src = (const char *)GP(name);
        size_t n = len ? len : strlen(src);
        if (n >= sizeof host) n = sizeof host - 1;
        memcpy(host, src, n);
        host[n] = 0;
    }
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) { memcpy(cn->host, host, sizeof host); SSL_set_tlsext_host_name(cn->ssl, cn->host); }
    pthread_mutex_unlock(&s_lock);
    hos_log("ssl: SetHostName(\"%s\") -> 0", host);
    RET(0);
}

HLE(nnsslConnectionSetVerifyOption) {
    const uint64_t obj = ARG(0);
    const uint32_t bits = (uint32_t)ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) cn->verify = bits;
    pthread_mutex_unlock(&s_lock);
    hos_log("ssl: SetVerifyOption(%#x: peer=%d hostname=%d) -> 0", bits, bits & 1, (bits >> 1) & 1);
    RET(0);
}

HLE(nnsslConnectionSetOption) {
    const uint64_t obj = ARG(0);
    const uint32_t option = (uint32_t)ARG(1);
    const uint32_t value = (uint32_t)ARG(2) & 1u;
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn && option < 4) cn->option[option] = value;
    pthread_mutex_unlock(&s_lock);
    SSL_LOG_ONCE(13, "ssl: SetOption(option=%u value=%u) -> 0 (stored, no equivalent needed)", option, value);
    RET(0);
}

HLE(nnsslConnectionSetSessionCacheMode) {
    const uint64_t obj = ARG(0);
    const uint32_t mode = (uint32_t)ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) cn->session_cache_mode = mode;
    pthread_mutex_unlock(&s_lock);
    SSL_LOG_ONCE(14, "ssl: SetSessionCacheMode(%u) -> 0 (no session reuse)", mode);
    RET(0);
}

HLE(nnsslConnectionSetIoMode) {
    const uint64_t obj = ARG(0);
    const uint32_t mode = (uint32_t)ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) cn->io_mode = mode;
    pthread_mutex_unlock(&s_lock);
    SSL_LOG_ONCE(15, "ssl: SetIoMode(%u) -> 0 (blocking host I/O)", mode);
    RET(0);
}

HLE(nnsslConnectionSetNextAlpnProto) {
    const uint64_t obj = ARG(0), proto = ARG(1);
    const uint32_t len = (uint32_t)ARG(2);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    char name[64];
    name[0] = 0;
    if (proto) {
        const char *src = (const char *)GP(proto);
        size_t n = len ? len : strlen(src);
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, src, n);
        name[n] = 0;
    }
    /* each call adds one protocol to the list (the SDK appends, it does not overwrite): keep the wire form */
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) {
        size_t n = strlen(name);
        if (n && cn->alpn_wire_len + 1 + n <= sizeof cn->alpn_wire) {
            cn->alpn_wire[cn->alpn_wire_len++] = (unsigned char)n;
            memcpy(cn->alpn_wire + cn->alpn_wire_len, name, n);
            cn->alpn_wire_len += n;
        }
    }
    pthread_mutex_unlock(&s_lock);
    hos_log("ssl: SetNextAlpnProto(\"%s\") -> 0", name);
    RET(0);
}

HLE(nnsslConnectionGetNextAlpnProto) {
    const uint64_t obj = ARG(0), buf = ARG(1), out_len = ARG(2);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    const unsigned char *proto = NULL;
    unsigned int proto_len = 0;
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) SSL_get0_alpn_selected(cn->ssl, &proto, &proto_len);
    pthread_mutex_unlock(&s_lock);
    if (buf && proto && proto_len) memcpy(GP(buf), proto, proto_len);
    if (out_len) G32(out_len) = proto_len;
    SSL_LOG_ONCE(16, "ssl: GetNextAlpnProto -> \"%.*s\"", (int)proto_len, proto ? (const char *)proto : "");
    RET(0);
}

HLE(nnsslConnectionSetServerCertBuffer) {
    const uint64_t obj = ARG(0), buf = ARG(1);
    const uint32_t size = (uint32_t)ARG(2);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) { cn->scert_addr = buf; cn->scert_size = size; }
    pthread_mutex_unlock(&s_lock);
    SSL_LOG_ONCE(17, "ssl: SetServerCertBuffer(%u bytes) -> 0 (kept)", size);
    RET(0);
}

HLE(nnsslConnectionGetServerCertDetail) {
    const uint64_t obj = ARG(0), out = ARG(1);
    if (!obj || !out) HLE_RETURN(SSL_R_INVALID);
    SSL_LOG_ONCE(18, "ssl: GetServerCertDetail -> 0 (detail left zeroed)");
    RET(0);
}

HLE(nnsslConnectionGetVerifyCertError) {
    const uint64_t obj = ARG(0), out = ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    long vres = 0;
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) vres = cn->verify_result;
    pthread_mutex_unlock(&s_lock);
    if (out) G32(out) = (uint32_t)vres;
    SSL_LOG_ONCE(19, "ssl: GetVerifyCertError -> %ld", vres);
    RET(0);
}

HLE(nnsslConnectionPending) {
    const uint64_t obj = ARG(0), out = ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    uint32_t pending = 0;
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) pending = (uint32_t)SSL_pending(cn->ssl);
    pthread_mutex_unlock(&s_lock);
    if (out && out >= SSL_GUEST_PTR_MIN) G32(out) = pending;
    SSL_LOG_ONCE(20, "ssl: Pending -> %u", pending);
    RET(0);
}

HLE(nnsslConnectionPeek) {
    const uint64_t obj = ARG(0), buf = ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    uint64_t out_addr = 0;
    uint32_t size = 0;
    int swapped = 0;
    ssl_split_count_size(ARG(2), ARG(3), &out_addr, &size, &swapped);
    SSL_LOG_ONCE(21, "ssl: Peek args x2=%#llx x3=%#llx -> out=%#llx size=%u%s", (unsigned long long)ARG(2),
                 (unsigned long long)ARG(3), (unsigned long long)out_addr, size, swapped ? " (swapped)" : "");
    SSL *ssl = NULL;
    int fd = -1;
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) { ssl = cn->ssl; fd = cn->fd; }
    pthread_mutex_unlock(&s_lock);
    if (!ssl || fd < 0) HLE_RETURN(SSL_R_DEAD);
    if (!size) HLE_RETURN(SSL_R_ZERO_SIZE);
    int saved = -1;
    if (ssl_set_nonblock(fd, &saved) != 0) HLE_RETURN(SSL_R_DEAD);
    int r = ssl_io_read(ssl, fd, GP(buf), size, 1);
    ssl_restore_flags(fd, saved);
    if (r < 0) {
        if (r == -2) hos_log("ssl: Peek timed out after %d ms (fd=%d)", ssl_timeout_ms(), fd);
        else ssl_log_ssl_error("Peek");
        if (out_addr) G32(out_addr) = 0;
        HLE_RETURN(SSL_R_DEAD);
    }
    if (out_addr) G32(out_addr) = (uint32_t)r;
    RET(0);
}


HLE(nnsslConnectionRead) {
    const uint64_t obj = ARG(0), buf = ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    uint64_t out_addr = 0;
    uint32_t size = 0;
    int swapped = 0;
    ssl_split_count_size(ARG(2), ARG(3), &out_addr, &size, &swapped);
    SSL_LOG_ONCE(22, "ssl: Read args x2=%#llx x3=%#llx -> out=%#llx size=%u%s", (unsigned long long)ARG(2),
                 (unsigned long long)ARG(3), (unsigned long long)out_addr, size, swapped ? " (swapped)" : "");
    SSL *ssl = NULL;
    int fd = -1;
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) { ssl = cn->ssl; fd = cn->fd; }
    pthread_mutex_unlock(&s_lock);
    if (!ssl || fd < 0) HLE_RETURN(SSL_R_DEAD);
    if (!size) HLE_RETURN(SSL_R_ZERO_SIZE);
    int saved = -1;
    if (ssl_set_nonblock(fd, &saved) != 0) HLE_RETURN(SSL_R_DEAD);
    int r = ssl_io_read(ssl, fd, GP(buf), size, 0);
    ssl_restore_flags(fd, saved);
    if (r < 0) {
        if (r == -2) hos_log("ssl: Read timed out after %d ms (fd=%d, wanted %u bytes)", ssl_timeout_ms(), fd, size);
        else ssl_log_ssl_error("Read");
        if (out_addr) G32(out_addr) = 0;
        HLE_RETURN(SSL_R_DEAD);
    }
    if (out_addr) G32(out_addr) = (uint32_t)r;
    RET(0);
}

HLE(nnsslConnectionWrite) {
    const uint64_t obj = ARG(0), buf = ARG(1);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    uint64_t out_addr = 0;
    uint32_t size = 0;
    int swapped = 0;
    ssl_split_count_size(ARG(2), ARG(3), &out_addr, &size, &swapped);
    SSL_LOG_ONCE(23, "ssl: Write args x2=%#llx x3=%#llx -> out=%#llx size=%u%s", (unsigned long long)ARG(2),
                 (unsigned long long)ARG(3), (unsigned long long)out_addr, size, swapped ? " (swapped)" : "");
    SSL *ssl = NULL;
    int fd = -1;
    pthread_mutex_lock(&s_lock);
    SslConnSlot *cn = ssl_conn_lookup(obj);
    if (cn) { ssl = cn->ssl; fd = cn->fd; }
    pthread_mutex_unlock(&s_lock);
    if (!ssl || fd < 0) HLE_RETURN(SSL_R_DEAD);
    if (!size) HLE_RETURN(SSL_R_ZERO_SIZE);
    int saved = -1;
    if (ssl_set_nonblock(fd, &saved) != 0) HLE_RETURN(SSL_R_DEAD);
    int r = ssl_io_write(ssl, fd, GP(buf), size);
    ssl_restore_flags(fd, saved);
    if (r < 0) {
        if (r == -2) hos_log("ssl: Write timed out after %d ms (fd=%d, %u bytes)", ssl_timeout_ms(), fd, size);
        else ssl_log_ssl_error("Write");
        if (out_addr) G32(out_addr) = 0;
        HLE_RETURN(SSL_R_DEAD);
    }
    if (out_addr) G32(out_addr) = (uint32_t)r;
    RET(0);
}

HLE(nnsslConnectionDoHandshakeWithCertBuffer) {
    const uint64_t obj = ARG(0);
    if (!obj) HLE_RETURN(SSL_R_INVALID);
    SslConnSlot *cn = NULL;
    pthread_mutex_lock(&s_lock);
    cn = ssl_conn_lookup(obj);
    pthread_mutex_unlock(&s_lock);
    if (!cn) HLE_RETURN(SSL_R_DEAD);
    SSL_LOG_ONCE(24, "ssl: DoHandshakeWithCertBuffer x1=%#llx x2=%#llx x3=%#llx (a client sends no certificate buffer)",
                 (unsigned long long)ARG(1), (unsigned long long)ARG(2), (unsigned long long)ARG(3));
    RET(ssl_do_handshake(cn, "with-cert-buffer", ARG(1), ARG(2)));
}

/* nn_socket.c: bytes already decrypted and buffered in the host for this fd, so poll()/select() report it */
int nn_ssl_pending(int fd) {
    if (fd < 0 || fd >= 1024) return 0;
    int pending = 0;
    int idx = -1;
    if (pthread_mutex_trylock(&s_lock) != 0) return 0;
    if (s_conn_by_fd[fd] >= 0) {
        idx = s_conn_by_fd[fd];
        if (idx < SSL_SLOT_MAX && s_conn[idx].used && s_conn[idx].ssl) pending = SSL_pending(s_conn[idx].ssl);
    }
    pthread_mutex_unlock(&s_lock);
    return pending;
}
