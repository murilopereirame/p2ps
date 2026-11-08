/*
 * p2f - a C client for the peer-to-file protocol.
 *
 * A port of p2f-lib (TypeScript, Node) to portable C, for platforms
 * without Node, such as the PS4. Same wire protocol, same crypto:
 *
 *   - Bearer token authentication.
 *   - Resumable downloads (HTTP Range on the /api/raw webseed).
 *   - AES-256-CTR decrypt as the bytes arrive. The key is ECDH-wrapped
 *     (P-256 / HKDF-SHA256 / AES-256-GCM) per transfer.
 *   - SHA-256 check of each finished file.
 *
 * Transport: http:// or https:// (TLS 1.2, mbedTLS). For HTTPS the server
 * certificate is checked against a built-in Mozilla CA bundle, or against
 * a CA file that you set with p2f_client_set_ca_file().
 */
#ifndef P2F_H
#define P2F_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes. Zero is success; all errors are negative. */
enum {
    P2F_OK = 0,
    P2F_ERR_ARG = -1,       /* bad argument or bad URL */
    P2F_ERR_NET = -2,       /* connect, send or receive failed (retry) */
    P2F_ERR_HTTP = -3,      /* the server sent a non-2xx status */
    P2F_ERR_PROTO = -4,     /* the response was not in the expected form */
    P2F_ERR_CRYPTO = -5,    /* key exchange or unwrap failed */
    P2F_ERR_IO = -6,        /* local file system error */
    P2F_ERR_ABORT = -7,     /* the progress callback asked to stop */
    P2F_ERR_SHORT = -8,     /* the file is incomplete (retry resumes it) */
    P2F_ERR_CHECKSUM = -9,  /* SHA-256 mismatch; the file was removed */
    P2F_ERR_NOMEM = -10,
    P2F_ERR_NOTFOUND = -11, /* the remote path does not exist */
    P2F_ERR_TLS = -12       /* TLS setup failed or the certificate is not
                               trusted (see p2f_last_error) */
};

/* True for errors where a new call can continue the download. */
int p2f_is_retryable(int err);

typedef struct p2f_client p2f_client;

/*
 * Make a client.
 *   base_url    "http://10.0.0.1:8000" or "https://files.example.com"
 *               (an optional path prefix is kept)
 *   token       an API token that starts with "p2f_"
 *   timeout_ms  timeout of short requests (info, list, metadata);
 *               0 means 30000
 */
int p2f_client_new(p2f_client **out, const char *base_url,
                   const char *token, int timeout_ms);
void p2f_client_free(p2f_client *c);

/*
 * HTTPS only: trust the certificates in this PEM file instead of the
 * built-in Mozilla CA bundle. Use it for a private CA or a self-signed
 * server certificate. Call it before the first request.
 */
int p2f_client_set_ca_file(p2f_client *c, const char *pem_path);

/*
 * Use this DNS server ("1.1.1.1" or "192.168.1.1:53") for all host names,
 * instead of the system resolver. NULL or "" goes back to the system
 * resolver. This setting is global (all clients).
 */
int p2f_set_dns_server(const char *server);

/* A text description of the last error on this client. */
const char *p2f_last_error(const p2f_client *c);
/* The HTTP status of the last P2F_ERR_HTTP, else 0. */
int p2f_last_status(const p2f_client *c);

/* ---- Listing ---------------------------------------------------------- */

typedef struct {
    char *name;
    int is_dir;
    int64_t size;  /* -1 for a folder */
} p2f_entry;

/* List a remote folder. "" is the shared root. Free with p2f_entries_free. */
int p2f_list(p2f_client *c, const char *path,
             p2f_entry **entries, size_t *count);
void p2f_entries_free(p2f_entry *entries, size_t count);

/* ---- Download --------------------------------------------------------- */

typedef struct {
    uint64_t bytes_done;
    uint64_t bytes_total;
    int files_done;
    int files_total;
    const char *current;  /* the remote path in progress, or NULL */
} p2f_progress;

/* Return non-zero to stop. The bytes on disk are kept for a resume. */
typedef int (*p2f_progress_fn)(const p2f_progress *p, void *user);

typedef struct {
    p2f_progress_fn on_progress;  /* may be NULL */
    void *user;
    int verify;           /* 1 = check SHA-256 (the default if opts is NULL) */
    int idle_timeout_ms;  /* drop a stalled stream; 0 means 60000 */
} p2f_download_opts;

/*
 * Download a remote file or folder into local_dir. The result is
 * local_dir/<basename of remote>. A second call after an error resumes.
 * out_local (may be NULL) receives the local path.
 */
int p2f_download_path(p2f_client *c, const char *remote,
                      const char *local_dir, const p2f_download_opts *opts,
                      char *out_local, size_t out_local_len);

#ifdef __cplusplus
}
#endif

#endif
