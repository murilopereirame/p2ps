/* Internal declarations shared by the p2f source files. */
#ifndef P2F_INTERNAL_H
#define P2F_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>

#include "p2f.h"

#include "mbedtls/aes.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

/* ---- Platform layer (platform_posix.c or platform_ps4.c) -------------- */

/* Fill buf with secure random bytes. Return 0 on success. */
int p2f_platform_random(void *buf, size_t len);
/* Resolve a host name or an IPv4 literal. Return 0 on success.
 * On failure, err receives the reason (may be empty). */
int p2f_platform_resolve(const char *host, struct in_addr *out,
                         char *err, size_t errlen);
/* p2f_dns.c: literal, cache, DNS override, then p2f_platform_resolve. */
int p2f_resolve(const char *host, struct in_addr *out, char *err, size_t errlen);

/* p2f_dns.c: one A-record query over UDP. Return 0 on success. */
int p2f_dns_query_a(const char *server, uint16_t port, const char *host,
                    int timeout_ms, struct in_addr *out,
                    char *err, size_t errlen);

/* ---- Client ----------------------------------------------------------- */

struct p2f_client {
    char host[256];
    uint16_t port;
    char base_path[256];  /* "" or "/prefix" (no trailing slash) */
    char *token;
    int timeout_ms;
    int last_status;
    char err[512];

    /* HTTPS. The TLS config is made on the first request (p2f_tls_setup). */
    int tls;
    char *ca_file;               /* NULL: built-in Mozilla bundle */
    int tls_ready;
    mbedtls_ssl_config ssl_conf;
    mbedtls_x509_crt ca;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_session session; /* for TLS session resumption */
    int have_session;
};

/* p2f_tls.c */
void p2f_tls_init(p2f_client *c);
int p2f_tls_setup(p2f_client *c);
void p2f_tls_free(p2f_client *c);

int p2f_fail(p2f_client *c, int code, const char *fmt, ...);

/* ---- HTTP (p2f_http.c) ------------------------------------------------ */

typedef struct {
    int fd;
    mbedtls_ssl_context *ssl;  /* NULL for plain HTTP */
    int status;
    int64_t content_length;  /* -1 if unknown */
    int chunked;
    int64_t left;            /* bytes left in the body or in this chunk */
    int eof;
    char buf[16384];
    size_t pos, len;
} p2f_http;

/*
 * Send GET base_path + path_query. range_start > 0 adds a Range header.
 * On success the status line and headers are read; the caller reads the
 * body with p2f_http_read. Any status is a success here.
 */
int p2f_http_get(p2f_client *c, const char *path_query, int64_t range_start,
                 int timeout_ms, p2f_http *h);
/* Read body bytes. Return >0 bytes, 0 at the end, <0 on error. */
ssize_t p2f_http_read(p2f_http *h, void *out, size_t n);
/* Read the whole body (NUL-terminated). Limit: max bytes. */
int p2f_http_read_all(p2f_http *h, char **out, size_t *len, size_t max);
void p2f_http_close(p2f_http *h);

/* GET a JSON endpoint: non-2xx becomes P2F_ERR_HTTP with the "error" text. */
int p2f_get_json(p2f_client *c, const char *path_query, char **body);

/* Percent-encode like JavaScript encodeURIComponent. Caller frees. */
char *p2f_urlencode(const char *s);

/* ---- Crypto (p2f_crypto.c) -------------------------------------------- */

typedef struct {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    char pub_b64[128];  /* raw uncompressed point, Base64: the "ck" value */
} p2f_keypair;

int p2f_keypair_generate(p2f_keypair *kp);
void p2f_keypair_free(p2f_keypair *kp);

int p2f_unwrap_transfer_key(p2f_keypair *kp, const char *server_pub_b64,
                            const char *wrapped_b64,
                            uint8_t key[32], uint8_t iv[16]);

/* The CTR counter block_index blocks after iv (128-bit big-endian add). */
void p2f_ctr_counter(const uint8_t iv[16], uint64_t block_index,
                     uint8_t out[16]);

typedef struct {
    mbedtls_aes_context aes;
    uint8_t nonce_counter[16];
    uint8_t stream_block[16];
    size_t nc_off;
} p2f_ctr;

int p2f_ctr_init(p2f_ctr *ctr, const uint8_t key[32], const uint8_t iv[16],
                 uint64_t block_index);
/* Decrypt in place. */
int p2f_ctr_update(p2f_ctr *ctr, uint8_t *buf, size_t len);
void p2f_ctr_free(p2f_ctr *ctr);

#endif
