/*
 * p2f for PS4 - browse a peer-to-file server and download files to the PS4.
 *
 * Config: /data/p2f/config.ini
 *   url   = https://files.example.com   (or http://10.0.0.1:8000)
 *   token = p2f_...
 *   dest  = /data/p2f/downloads         (or /mnt/usb0/p2f)
 *   ca    = /data/p2f/ca.pem            (optional, HTTPS: trust this CA
 *                                        instead of the built-in bundle)
 *   dns   = 192.168.1.1                 (optional: DNS server for host names)
 *
 * Controls:
 *   Up/Down  move     Left/Right  page     X  open folder / queue file
 *   Triangle queue this folder   O  parent folder
 *   Square   cancel the current download    Options  refresh
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

#include "gfx.h"
#include "p2f.h"

#define APP_DIR     "/data/p2f"
#define CONFIG_PATH APP_DIR "/config.ini"
#define LOG_PATH    APP_DIR "/p2f.log"
#define MAX_QUEUE   64
#define MAX_TRIES   8
#define ROWS        16

/* Colors. */
#define C_BG     0x14161c
#define C_PANEL  0x1f232c
#define C_TEXT   0xe6e8ee
#define C_DIM    0x8a91a3
#define C_ACCENT 0x4c8dff
#define C_OK     0x3ccf7a
#define C_ERR    0xff5a5a
#define C_SEL    0x2b3a5c

/* ---- Config and log --------------------------------------------------- */

static char cfg_url[512], cfg_token[256], cfg_dest[512] = APP_DIR "/downloads";
static char cfg_ca[512], cfg_dns[64];

static void trim(char *s)
{
    char *p = s, *e;
    while (*p == ' ' || *p == '\t')
        p++;
    memmove(s, p, strlen(p) + 1);
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
}

static int load_config(void)
{
    char line[1024];
    FILE *f;

    mkdir(APP_DIR, 0777);
    f = fopen(CONFIG_PATH, "r");
    if (!f) {
        f = fopen(CONFIG_PATH, "w");
        if (f) {
            fputs("# p2f for PS4\n"
                  "# http://host:port or https://host[:port]\n"
                  "url   = https://files.example.com\n"
                  "token = p2f_REPLACE_ME\n"
                  "dest  = " APP_DIR "/downloads\n"
                  "# HTTPS with a private CA or a self-signed certificate:\n"
                  "# ca  = " APP_DIR "/ca.pem\n"
                  "# Optional DNS server for host names (default: PS4 settings):\n"
                  "# dns = 1.1.1.1\n", f);
            fclose(f);
        }
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq;
        trim(line);
        if (line[0] == '#' || line[0] == ';' || !(eq = strchr(line, '=')))
            continue;
        *eq = '\0';
        trim(line);
        trim(eq + 1);
        if (strcasecmp(line, "url") == 0)
            snprintf(cfg_url, sizeof(cfg_url), "%s", eq + 1);
        else if (strcasecmp(line, "token") == 0)
            snprintf(cfg_token, sizeof(cfg_token), "%s", eq + 1);
        else if (strcasecmp(line, "dest") == 0 && eq[1])
            snprintf(cfg_dest, sizeof(cfg_dest), "%s", eq + 1);
        else if (strcasecmp(line, "ca") == 0)
            snprintf(cfg_ca, sizeof(cfg_ca), "%s", eq + 1);
        else if (strcasecmp(line, "dns") == 0)
            snprintf(cfg_dns, sizeof(cfg_dns), "%s", eq + 1);
    }
    fclose(f);
    {   /* No trailing slash on dest. */
        size_t n = strlen(cfg_dest);
        while (n > 1 && cfg_dest[n - 1] == '/')
            cfg_dest[--n] = '\0';
    }
    return (cfg_url[0] && cfg_token[0] && strstr(cfg_token, "REPLACE") == NULL) ? 0 : -1;
}

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_line(const char *fmt, ...)
{
    va_list ap;
    FILE *f;
    pthread_mutex_lock(&log_lock);
    f = fopen(LOG_PATH, "a");
    if (f) {
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    pthread_mutex_unlock(&log_lock);
}

static void notify(const char *msg)
{
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    req.useIconImageUri = 1;
    snprintf(req.message, sizeof(req.message), "%s", msg);
    snprintf(req.iconUri, sizeof(req.iconUri), "cxml://psnotification/tex_icon_system");
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void human(uint64_t b, char *out, size_t n)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        i++;
    }
    snprintf(out, n, i ? "%.1f %s" : "%.0f %s", v, u[i]);
}

/* ---- Download worker -------------------------------------------------- */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

static char *queue[MAX_QUEUE];
static int q_head, q_len;

static struct {
    int active;
    char name[512];
    p2f_progress prog;
    int attempt;
    volatile int cancel;
    char status[512];
    uint32_t status_color;
} dl;

static int queue_push(const char *remote)
{
    int ok = 0;
    pthread_mutex_lock(&lock);
    if (q_len < MAX_QUEUE) {
        queue[(q_head + q_len) % MAX_QUEUE] = strdup(remote);
        q_len++;
        ok = 1;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

static void set_status(uint32_t color, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&lock);
    va_start(ap, fmt);
    vsnprintf(dl.status, sizeof(dl.status), fmt, ap);
    va_end(ap);
    dl.status_color = color;
    pthread_mutex_unlock(&lock);
}

static int on_progress(const p2f_progress *p, void *user)
{
    (void)user;
    pthread_mutex_lock(&lock);
    dl.prog = *p;
    dl.prog.current = NULL;  /* do not keep a pointer into the library */
    pthread_mutex_unlock(&lock);
    return dl.cancel;
}

/* A client for the configured server (and CA file, if set). */
static p2f_client *make_client(int timeout_ms)
{
    p2f_client *c = NULL;
    if (p2f_client_new(&c, cfg_url, cfg_token, timeout_ms) != P2F_OK)
        return NULL;
    if (cfg_ca[0])
        p2f_client_set_ca_file(c, cfg_ca);
    return c;
}

static void *worker(void *arg)
{
    p2f_client *c;
    (void)arg;

    c = make_client(15000);
    if (!c) {
        set_status(C_ERR, "Bad url in config.ini (use http:// or https://)");
        return NULL;
    }

    for (;;) {
        char *remote;
        int rc = P2F_OK, attempt;
        /* prepare_timeout_ms 0: wait for the server as long as it takes. */
        p2f_download_opts opts = { on_progress, NULL, 1, 30000, 0 };

        pthread_mutex_lock(&lock);
        while (q_len == 0)
            pthread_cond_wait(&wake, &lock);
        remote = queue[q_head];
        q_head = (q_head + 1) % MAX_QUEUE;
        q_len--;
        dl.active = 1;
        dl.cancel = 0;
        memset(&dl.prog, 0, sizeof(dl.prog));
        snprintf(dl.name, sizeof(dl.name), "%s", remote);
        pthread_mutex_unlock(&lock);

        log_line("start %s", remote);
        for (attempt = 1; attempt <= MAX_TRIES; attempt++) {
            pthread_mutex_lock(&lock);
            dl.attempt = attempt;
            pthread_mutex_unlock(&lock);
            rc = p2f_download_path(c, remote, cfg_dest, &opts, NULL, 0);
            if (rc == P2F_OK || rc == P2F_ERR_ABORT || !p2f_is_retryable(rc))
                break;
            log_line("retry %d for %s: %s", attempt, remote, p2f_last_error(c));
            set_status(C_DIM, "Retry %d/%d: %s", attempt, MAX_TRIES, p2f_last_error(c));
            {   /* Back off 2, 4, 8 ... 30 s; a cancel stops the wait. */
                int s = 2 << (attempt - 1), i;
                if (s > 30)
                    s = 30;
                for (i = 0; i < s * 10 && !dl.cancel; i++)
                    sceKernelUsleep(100000);
            }
            if (dl.cancel) {
                rc = P2F_ERR_ABORT;
                break;
            }
        }

        if (rc == P2F_OK) {
            char msg[600];
            snprintf(msg, sizeof(msg), "Downloaded: %s", remote);
            set_status(C_OK, "%s", msg);
            notify(msg);
            log_line("done %s", remote);
        } else if (rc == P2F_ERR_ABORT) {
            set_status(C_DIM, "Stopped: %s (resume: queue it again)", remote);
            log_line("stopped %s", remote);
        } else {
            char msg[800];
            snprintf(msg, sizeof(msg), "Failed: %s - %s", remote, p2f_last_error(c));
            set_status(C_ERR, "%s", msg);
            notify(msg);
            log_line("%s", msg);
        }

        pthread_mutex_lock(&lock);
        dl.active = 0;
        pthread_mutex_unlock(&lock);
        free(remote);
    }
    return NULL;
}

/* ---- Browser ---------------------------------------------------------- */

static p2f_client *ui_client;
static char cwd[1024];
static p2f_entry *entries;
static size_t n_entries;
static int sel, top;
static char browse_err[512];

static int cmp_entry(const void *a, const void *b)
{
    const p2f_entry *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void draw(void);

static void load_dir(const char *path)
{
    p2f_entry *e = NULL;
    size_t n = 0;
    int rc;

    browse_err[0] = '\0';
    snprintf(browse_err, sizeof(browse_err), "Loading...");
    draw();
    rc = p2f_list(ui_client, path, &e, &n);
    if (rc != P2F_OK) {
        snprintf(browse_err, sizeof(browse_err), "%s", p2f_last_error(ui_client));
        return;
    }
    browse_err[0] = '\0';
    p2f_entries_free(entries, n_entries);
    qsort(e, n, sizeof(*e), cmp_entry);
    entries = e;
    n_entries = n;
    if (path != cwd)
        snprintf(cwd, sizeof(cwd), "%s", path);
    sel = top = 0;
}

static void child_path(const char *name, char *out, size_t n)
{
    if (cwd[0])
        snprintf(out, n, "%s/%s", cwd, name);
    else
        snprintf(out, n, "%s", name);
}

static void go_parent(void)
{
    char parent[1024], *slash;
    char last[256] = "";
    if (!cwd[0])
        return;
    snprintf(parent, sizeof(parent), "%s", cwd);
    slash = strrchr(parent, '/');
    if (slash) {
        snprintf(last, sizeof(last), "%s", slash + 1);
        *slash = '\0';
    } else {
        snprintf(last, sizeof(last), "%s", parent);
        parent[0] = '\0';
    }
    load_dir(parent);
    /* Put the cursor back on the folder we came from. */
    {
        size_t i;
        for (i = 0; i < n_entries; i++) {
            if (strcmp(entries[i].name, last) == 0) {
                sel = (int)i;
                top = sel > ROWS / 2 ? sel - ROWS / 2 : 0;
                break;
            }
        }
    }
}

/* ---- Drawing ---------------------------------------------------------- */

static uint64_t speed_bps;

static void draw(void)
{
    char buf[1024], a[32], b[32];
    int i, y;
    int active, attempt, qn;
    p2f_progress prog;
    char name[512], status[512];
    uint32_t scol;

    pthread_mutex_lock(&lock);
    active = dl.active;
    attempt = dl.attempt;
    prog = dl.prog;
    qn = q_len;
    snprintf(name, sizeof(name), "%s", dl.name);
    snprintf(status, sizeof(status), "%s", dl.status);
    scol = dl.status_color ? dl.status_color : C_DIM;
    pthread_mutex_unlock(&lock);

    gfx_clear(C_BG);

    /* Header. */
    gfx_text(80, 50, 5, C_TEXT, "P2PS", 0);
    gfx_text(280, 66, 3, C_DIM, "Powered by P2F", 0);
    snprintf(buf, sizeof(buf), "%s -> %s", cfg_url, cfg_dest);
    gfx_text(80, 120, 2, C_DIM, buf, 110);

    /* Path. */
    snprintf(buf, sizeof(buf), "/%s", cwd);
    gfx_text(80, 170, 3, C_ACCENT, buf, 72);

    /* List. */
    y = 220;
    if (browse_err[0]) {
        gfx_text(100, y, 3, strcmp(browse_err, "Loading...") ? C_ERR : C_DIM, browse_err, 70);
    } else if (n_entries == 0) {
        gfx_text(100, y, 3, C_DIM, "(empty folder)", 0);
    }
    for (i = 0; !browse_err[0] && i < ROWS && top + i < (int)n_entries; i++) {
        const p2f_entry *e = &entries[top + i];
        int row_y = y + i * 34;
        if (top + i == sel)
            gfx_rect(70, row_y - 6, 1780, 34, C_SEL);
        gfx_text(90, row_y, 3, e->is_dir ? C_ACCENT : C_TEXT, e->is_dir ? "[D]" : "   ", 0);
        gfx_text(190, row_y, 3, e->is_dir ? C_ACCENT : C_TEXT, e->name, 55);
        if (!e->is_dir) {
            human((uint64_t)e->size, a, sizeof(a));
            gfx_text(1600, row_y, 3, C_DIM, a, 0);
        }
    }
    if (n_entries > ROWS) {
        snprintf(buf, sizeof(buf), "%d / %d", sel + 1, (int)n_entries);
        gfx_text(1600, 170, 3, C_DIM, buf, 0);
    }

    /* Download panel. */
    gfx_rect(60, 790, 1800, 200, C_PANEL);
    if (active && prog.preparing) {
        /* The server reads and hashes a big file once before it can send
         * it. That can take minutes; the library waits (Square stops). */
        double frac = prog.prepare_total ? (double)prog.prepare_done / (double)prog.prepare_total : 0.0;
        gfx_text(90, 810, 3, C_TEXT, name, 62);
        gfx_rect(90, 855, 1740, 30, 0x2c3240);
        gfx_rect(90, 855, (int)(1740 * frac), 30, C_DIM);
        if (prog.prepare_total) {
            human(prog.prepare_done, a, sizeof(a));
            human(prog.prepare_total, b, sizeof(b));
            snprintf(buf, sizeof(buf), "Server is preparing the file: %s of %s", a, b);
        } else {
            snprintf(buf, sizeof(buf), "Server is preparing the file (large files take a while)...");
        }
        gfx_text(90, 900, 3, C_DIM, buf, 0);
    } else if (active) {
        double frac = prog.bytes_total ? (double)prog.bytes_done / (double)prog.bytes_total : 0.0;
        snprintf(buf, sizeof(buf), "%s", name);
        gfx_text(90, 810, 3, C_TEXT, buf, 62);
        gfx_rect(90, 855, 1740, 30, 0x2c3240);
        gfx_rect(90, 855, (int)(1740 * frac), 30, C_ACCENT);
        human(prog.bytes_done, a, sizeof(a));
        human(prog.bytes_total, b, sizeof(b));
        snprintf(buf, sizeof(buf), "%5.1f%%   %s of %s   file %d/%d", frac * 100.0, a, b,
                 prog.files_done + (prog.files_done < prog.files_total), prog.files_total);
        gfx_text(90, 900, 3, C_TEXT, buf, 0);
        human(speed_bps, a, sizeof(a));
        if (speed_bps > 0 && prog.bytes_total > prog.bytes_done) {
            uint64_t eta = (prog.bytes_total - prog.bytes_done) / speed_bps;
            snprintf(buf, sizeof(buf), "%s/s  ETA %02d:%02d:%02d", a, (int)(eta / 3600),
                     (int)(eta / 60 % 60), (int)(eta % 60));
        } else {
            snprintf(buf, sizeof(buf), "%s/s", a);
        }
        gfx_text(1300, 900, 3, C_DIM, buf, 0);
        if (attempt > 1) {
            snprintf(buf, sizeof(buf), "try %d", attempt);
            gfx_text(1700, 810, 3, C_DIM, buf, 0);
        }
    } else {
        gfx_text(90, 830, 3, C_DIM, "No active download.", 0);
    }
    if (qn > 0) {
        snprintf(buf, sizeof(buf), "Queued: %d", qn);
        gfx_text(90, 950, 2, C_DIM, buf, 0);
    }
    if (status[0])
        gfx_text(300, 950, 2, scol, status, 100);

    /* Footer. */
    gfx_text(80, 1020, 2, C_DIM,
             "X open/queue   TRIANGLE queue folder   O back   SQUARE cancel   OPTIONS refresh",
             0);
    gfx_present();
}

static void draw_message(const char *l1, const char *l2, const char *l3)
{
    gfx_clear(C_BG);
    gfx_text(80, 50, 5, C_TEXT, "P2PS", 0);
    gfx_text(80, 300, 3, C_ERR, l1, 0);
    if (l2)
        gfx_text(80, 360, 3, C_TEXT, l2, 0);
    if (l3)
        gfx_text(80, 420, 3, C_DIM, l3, 0);
    gfx_present();
}

/* ---- Input ------------------------------------------------------------ */

static int pad = -1;

static void pad_init(void)
{
    OrbisUserServiceInitializeParams p = { 256 };
    int user = 0;
    sceUserServiceInitialize(&p);
    sceUserServiceGetInitialUser(&user);
    scePadInit();
    pad = scePadOpen(user, 0, 0, NULL);
}

static unsigned pad_buttons(void)
{
    OrbisPadData d;
    if (pad < 0)
        return 0;
    memset(&d, 0, sizeof(d));
    if (scePadReadState(pad, &d) != 0 || !d.connected)
        return 0;
    return d.buttons;
}

/* ---- Main ------------------------------------------------------------- */

int main(void)
{
    pthread_t th;
    unsigned prev = 0;
    int hold = 0;
    uint64_t last_bytes = 0;
    uint64_t last_us = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (gfx_init() != 0)
        for (;;) sceKernelUsleep(1000000);
    sceSystemServiceHideSplashScreen();
    pad_init();

    if (load_config() != 0) {
        for (;;) {
            draw_message("Set up the server first.",
                         "Edit " CONFIG_PATH " (FTP), then start the app again.",
                         "Keys: url = https://<server>   token = p2f_...   dest = folder   ca = file (optional)");
        }
    }
    mkdir(cfg_dest, 0777);
    if (cfg_dns[0] && p2f_set_dns_server(cfg_dns) != P2F_OK) {
        for (;;)
            draw_message("The dns value in config.ini is not valid.",
                         "Use an IPv4 address, for example dns = 1.1.1.1", NULL);
    }
    ui_client = make_client(10000);
    if (!ui_client) {
        for (;;)
            draw_message("The url in config.ini is not valid.",
                         "Use https://files.example.com or http://10.0.0.1:8000", NULL);
    }
    pthread_create(&th, NULL, worker, NULL);
    log_line("app start, server %s", cfg_url);
    load_dir("");

    for (;;) {
        unsigned b = pad_buttons();
        unsigned pressed = b & ~prev;
        uint64_t now = sceKernelGetProcessTime();

        /* Hold Up/Down to repeat. */
        if (b & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN)) {
            if (++hold > 20 && hold % 3 == 0)
                pressed |= b & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN);
        } else {
            hold = 0;
        }
        prev = b;

        if (n_entries > 0) {
            if (pressed & ORBIS_PAD_BUTTON_UP)
                sel = sel > 0 ? sel - 1 : (int)n_entries - 1;
            if (pressed & ORBIS_PAD_BUTTON_DOWN)
                sel = sel < (int)n_entries - 1 ? sel + 1 : 0;
            if (pressed & ORBIS_PAD_BUTTON_LEFT)
                sel = sel - ROWS < 0 ? 0 : sel - ROWS;
            if (pressed & ORBIS_PAD_BUTTON_RIGHT)
                sel = sel + ROWS >= (int)n_entries ? (int)n_entries - 1 : sel + ROWS;
            if (sel < top)
                top = sel;
            if (sel >= top + ROWS)
                top = sel - ROWS + 1;
        }
        if ((pressed & ORBIS_PAD_BUTTON_CROSS) && n_entries > 0) {
            char p[1024];
            child_path(entries[sel].name, p, sizeof(p));
            if (entries[sel].is_dir) {
                load_dir(p);
            } else if (queue_push(p)) {
                set_status(C_DIM, "Queued: %s", p);
            }
        }
        if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) && cwd[0]) {
            if (queue_push(cwd))
                set_status(C_DIM, "Queued folder: %s", cwd);
        }
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
            go_parent();
        if (pressed & ORBIS_PAD_BUTTON_SQUARE)
            dl.cancel = 1;
        if (pressed & ORBIS_PAD_BUTTON_OPTIONS)
            load_dir(cwd);

        /* Speed, sampled every second; keep the console awake. */
        if (now - last_us >= 1000000) {
            uint64_t done;
            pthread_mutex_lock(&lock);
            done = dl.active ? dl.prog.bytes_done : 0;
            pthread_mutex_unlock(&lock);
            if (done >= last_bytes && last_us) {
                uint64_t inst = (done - last_bytes) * 1000000 / (now - last_us);
                speed_bps = speed_bps ? (speed_bps * 7 + inst * 3) / 10 : inst;
            } else {
                speed_bps = 0;
            }
            last_bytes = done;
            last_us = now;
            if (dl.active)
                sceSystemServicePowerTick();
        }

        draw();
    }
    return 0;
}
