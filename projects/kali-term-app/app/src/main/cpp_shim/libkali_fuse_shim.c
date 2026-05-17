/*
 * libkali_fuse_shim.so — LD_PRELOAD libc-override a chrooted Kali bash-ban.
 *
 * Cél: a chrooted process /proc, /sys, /dev/bus path-okra hívott libc-
 * funkcók (open/read/stat/opendir/readdir/close) az LKL kernel-fájlrendszerét
 * érjék el ÉLŐ módon, NEM a host-szintű static mirror-t.
 *
 * Mechanizmus: minden ilyen libc-call connectel a `/run/lkl-control.sock`
 * unix-domain-socketre (a `:lkl` Service-process kreálta), és szöveges
 * OPEN/READ/CLOSE/STAT/LISTDIR protokoll-szel az LKL-syscallt route-elja.
 *
 * Build: aarch64-linux-gnu-gcc (GLIBC ABI), NEM Bionic — a chrooted Kali
 * glibc 2.36+ ABI-jával kell kompatibilis legyen.
 *
 *   aarch64-linux-gnu-gcc -shared -fPIC -O2 -D_GNU_SOURCE \
 *       -o libkali_fuse_shim.so libkali_fuse_shim.c -ldl -lpthread
 *
 * Korlátok (jelenleg):
 *   - csak READ; write/seek a kvázi-FS-en nem támogatott
 *   - ioctl-route NEM él (libusb USBDEVFS_* a 3/3 mérföldkő)
 *   - stat/opendir-bázis-implementáció van
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/select.h>
#include <linux/netlink.h>
#include <dirent.h>

#define SOCK_PATH "/run/lkl-control.sock"

/* Magic-fd offset: az LKL-routed fd-ket egy nagy konstans-szal eltoljuk a
 * normál fd-ket alóltól. fd N (LKL) → host-visible fd = MAGIC_FD_BASE + slotN. */
#define MAGIC_FD_BASE  100000
#define MAX_LKL_FDS    1024

struct lkl_slot {
    int  sock;       /* per-fd dedicated socket-handle */
    long lkl_fd;     /* az `:lkl` process-en belüli fd */
};
static struct lkl_slot g_slots[MAX_LKL_FDS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Eredeti libc-functions, dlsym(RTLD_NEXT) — lazy-init. */
static int    (*r_open)(const char *, int, ...)          = NULL;
static int    (*r_openat)(int, const char *, int, ...)   = NULL;
static ssize_t(*r_read)(int, void *, size_t)             = NULL;
static int    (*r_close)(int)                            = NULL;
static off_t  (*r_lseek)(int, off_t, int)                = NULL;
static int    (*r_stat)(const char *, struct stat *)     = NULL;
static int    (*r_lstat)(const char *, struct stat *)    = NULL;
static int    (*r_fstat)(int, struct stat *)             = NULL;
static int    (*r_access)(const char *, int)             = NULL;
static DIR   *(*r_opendir)(const char *)                 = NULL;

#define INIT(fn) do { if (!r_##fn) r_##fn = dlsym(RTLD_NEXT, #fn); } while (0)

/* Path-prefix-szerinti LKL-route-döntés.
 *
 * /sys, /proc: LKL-szolgáltatott élő-FS, route-eljük az LKL-be.
 *
 * /dev/bus: NE LKL-route! Az LKL devtmpfs-en NINCS /dev/bus/usb (udev kreálná
 * a host-Linuxon). A user-mode mirror-megközelítés szolgáltatja a placeholder-
 * fát; a shim ne menjen LKL-be — fel-bukna ENOENT-en. */
static int is_lkl_path(const char *path)
{
    if (!path) return 0;
    if (path[0] != '/') return 0;
    if (strncmp(path, "/sys", 4) == 0 && (path[4] == '/' || path[4] == 0)) return 1;
    if (strncmp(path, "/proc", 5) == 0 && (path[5] == '/' || path[5] == 0)) return 1;
    return 0;
}

/* Unix-socket connect. Non-blocking + select(2-sec) — dead server NE fagyasszon
 * be hangin' connectet. A SO_RCVTIMEO/SO_SNDTIMEO csak read/write-ra hat,
 * a connect maga blokkolhat (pl. accept queue full vagy féltermálkott peer);
 * ezért használjuk az O_NONBLOCK + select kombinációt.
 *
 * Az SO_RCVTIMEO/SO_SNDTIMEO továbbra is be van állítva a sikeres connect
 * UTÁN, hogy a read_line/write is timeoutolható legyen. */
static int sock_connect(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;

    /* Non-blocking connect */
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) {
        close(s); return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    int rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        /* connect azonnal sikerült (Unix-socket: tipikus) */
    } else if (errno == EINPROGRESS) {
        /* várjuk meg max 2 sec-ig hogy összejöjjön */
        fd_set wfd; FD_ZERO(&wfd); FD_SET(s, &wfd);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int sel = select(s + 1, NULL, &wfd, NULL, &tv);
        if (sel <= 0) {
            int e = errno; close(s); errno = (sel == 0) ? ETIMEDOUT : e;
            return -1;
        }
        int err = 0; socklen_t errlen = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) { close(s); errno = err; return -1; }
    } else {
        int e = errno; close(s); errno = e;
        return -1;
    }

    /* Visszaállítjuk blockingre + read/write timeoutok */
    fcntl(s, F_SETFL, fl);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return s;
}

/* Egy line olvasása a socket-ről. */
static ssize_t read_line(int sock, char *buf, size_t bufsz)
{
    size_t n = 0;
    INIT(read);
    while (n + 1 < bufsz) {
        char c;
        ssize_t r = r_read(sock, &c, 1);
        if (r <= 0) { buf[n] = 0; return r; }
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = 0;
    return n;
}

/* Slot-alloc — atomicly. */
static int alloc_slot(int sock, long lkl_fd)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_LKL_FDS; i++) {
        if (g_slots[i].sock == 0) {
            g_slots[i].sock = sock;
            g_slots[i].lkl_fd = lkl_fd;
            pthread_mutex_unlock(&g_lock);
            return MAGIC_FD_BASE + i;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return -1;
}

static struct lkl_slot *get_slot(int fd)
{
    if (fd < MAGIC_FD_BASE || fd >= MAGIC_FD_BASE + MAX_LKL_FDS) return NULL;
    pthread_mutex_lock(&g_lock);
    struct lkl_slot *s = &g_slots[fd - MAGIC_FD_BASE];
    pthread_mutex_unlock(&g_lock);
    return (s->sock > 0) ? s : NULL;
}

static void free_slot(int fd)
{
    if (fd < MAGIC_FD_BASE || fd >= MAGIC_FD_BASE + MAX_LKL_FDS) return;
    INIT(close);
    pthread_mutex_lock(&g_lock);
    struct lkl_slot *s = &g_slots[fd - MAGIC_FD_BASE];
    int sock = s->sock;
    s->sock = 0; s->lkl_fd = -1;
    pthread_mutex_unlock(&g_lock);
    if (sock > 0) r_close(sock);
}

/* ────────────────────────────────────────────────────────────────────
 *  open()/openat()
 * ──────────────────────────────────────────────────────────────────── */

int open(const char *path, int flags, ...)
{
    INIT(open);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    if (!is_lkl_path(path)) return r_open(path, flags, mode);

    int sock = sock_connect();
    if (sock < 0) return r_open(path, flags, mode);

    char req[1280];
    int rn = snprintf(req, sizeof(req), "OPEN %s %d\n", path, flags);
    if (write(sock, req, rn) != rn) {
        close(sock); return r_open(path, flags, mode);
    }
    char resp[128];
    if (read_line(sock, resp, sizeof(resp)) <= 0) {
        close(sock); return r_open(path, flags, mode);
    }
    long lkl_fd = -1;
    if (sscanf(resp, "OK fd=%ld", &lkl_fd) == 1 && lkl_fd >= 0) {
        int magic = alloc_slot(sock, lkl_fd);
        if (magic > 0) return magic;
        close(sock); errno = EMFILE; return -1;
    }
    int err = 0; sscanf(resp, "ERR errno=%d", &err);
    close(sock);
    errno = err ? err : ENOENT;
    return -1;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    INIT(openat);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    /* Csak az abszolút path-t LKL-routol; relatív path-t a real-libc */
    if (path && path[0] == '/' && is_lkl_path(path)) {
        return open(path, flags, mode);
    }
    return r_openat(dirfd, path, flags, mode);
}

/* ────────────────────────────────────────────────────────────────────
 *  read() / pread()
 * ──────────────────────────────────────────────────────────────────── */

ssize_t read(int fd, void *buf, size_t count)
{
    INIT(read);
    if (fd < MAGIC_FD_BASE) return r_read(fd, buf, count);
    struct lkl_slot *s = get_slot(fd);
    if (!s) { errno = EBADF; return -1; }

    char req[64];
    int n = snprintf(req, sizeof(req), "READ %ld %zu\n", s->lkl_fd, count);
    if (write(s->sock, req, n) != n) { errno = EIO; return -1; }
    char hdr[64];
    if (read_line(s->sock, hdr, sizeof(hdr)) <= 0) { errno = EIO; return -1; }
    long got = -1;
    if (sscanf(hdr, "OK len=%ld", &got) == 1) {
        if (got <= 0) return 0;
        if ((size_t)got > count) got = count;
        size_t total = 0;
        while (total < (size_t)got) {
            ssize_t r = r_read(s->sock, (char *)buf + total, (size_t)got - total);
            if (r <= 0) break;
            total += r;
        }
        return total;
    }
    int err = 0; sscanf(hdr, "ERR errno=%d", &err);
    errno = err ? err : EIO;
    return -1;
}

/* ────────────────────────────────────────────────────────────────────
 *  close()
 * ──────────────────────────────────────────────────────────────────── */

int close(int fd)
{
    INIT(close);
    if (fd < MAGIC_FD_BASE) return r_close(fd);
    struct lkl_slot *s = get_slot(fd);
    if (!s) { errno = EBADF; return -1; }

    char req[64];
    int n = snprintf(req, sizeof(req), "CLOSE %ld\n", s->lkl_fd);
    /* fire-and-forget; majd a free_slot zárja a per-fd socket-et */
    write(s->sock, req, n);
    char resp[64];
    read_line(s->sock, resp, sizeof(resp));
    free_slot(fd);
    return 0;
}

/* ────────────────────────────────────────────────────────────────────
 *  stat / lstat / fstat — minimal placeholder mode-okra
 * ──────────────────────────────────────────────────────────────────── */

/* Valódi STAT az LKL-control-socketen — `lkl_sys_newfstatat` mögöttes hívás.
 * Fallback fake-stat ha a socket-kapcsolat nem megy (dead :lkl process). */
static int fake_stat_lkl(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    return 0;
}

static int lkl_stat_real(const char *path, struct stat *st)
{
    int sock = sock_connect();
    if (sock < 0) return fake_stat_lkl(path, st);
    char req[1280];
    int n = snprintf(req, sizeof(req), "STAT %s\n", path);
    if (write(sock, req, n) != n) { close(sock); return fake_stat_lkl(path, st); }
    char resp[160];
    if (read_line(sock, resp, sizeof(resp)) <= 0) {
        close(sock); return fake_stat_lkl(path, st);
    }
    close(sock);
    unsigned int mode = 0;
    long size = 0;
    unsigned long ino = 0;
    if (sscanf(resp, "OK mode=%u size=%ld ino=%lu", &mode, &size, &ino) == 3) {
        memset(st, 0, sizeof(*st));
        st->st_mode = (mode_t)mode;
        st->st_size = (off_t)size;
        st->st_ino = (ino_t)ino;
        st->st_nlink = 1;
        st->st_blksize = 4096;
        st->st_blocks = (size + 511) / 512;
        return 0;
    }
    int err = 0;
    sscanf(resp, "ERR errno=%d", &err);
    errno = err ? err : ENOENT;
    return -1;
}

int stat(const char *path, struct stat *st)
{
    INIT(stat);
    if (is_lkl_path(path)) return lkl_stat_real(path, st);
    return r_stat(path, st);
}

int lstat(const char *path, struct stat *st)
{
    INIT(lstat);
    if (is_lkl_path(path)) return lkl_stat_real(path, st);
    return r_lstat(path, st);
}

int fstat(int fd, struct stat *st)
{
    INIT(fstat);
    if (fd < MAGIC_FD_BASE) return r_fstat(fd, st);
    /* LKL-fd-re: ATM nincs FSTAT a control-socketon; fake. */
    return fake_stat_lkl(NULL, st);
}

/* glibc-régi __xstat/__lxstat/__fxstat alias-ok — a Debian 9-előtti libc
 * ezeket hívja, modern glibc default a direct `stat`-szimbólumokat. */
int __xstat(int ver, const char *path, struct stat *st)  { return stat(path, st); }
int __lxstat(int ver, const char *path, struct stat *st) { return lstat(path, st); }
int __fxstat(int ver, int fd, struct stat *st)           { return fstat(fd, st); }

/* ────────────────────────────────────────────────────────────────────
 *  access() — letiltjuk a 404-et az LKL-path-on (placeholder)
 * ──────────────────────────────────────────────────────────────────── */

int access(const char *path, int mode)
{
    INIT(access);
    if (is_lkl_path(path)) return 0;  /* feltesszük hogy létezik */
    return r_access(path, mode);
}

/* ────────────────────────────────────────────────────────────────────
 *  opendir / readdir / closedir — LKL-LISTDIR
 *
 *  Az LKL-DIR egy custom struct, a fake_dir_t. A glibc DIR* opaque, mi
 *  egy saját-allokált struktúrát adunk vissza. A readdir/closedir
 *  felismerni az LKL-DIR-t a magic-by-allocation.
 * ──────────────────────────────────────────────────────────────────── */

#define LKL_DIR_MAGIC 0xC0DECAFE

struct lkl_dir {
    unsigned     magic;
    char       **entries;
    int          nentries;
    int          pos;
    struct dirent ent;
};

DIR *opendir(const char *path)
{
    INIT(opendir);
    if (!is_lkl_path(path)) return r_opendir(path);

    fprintf(stderr, "[shim opendir LKL] %s\n", path);
    int sock = sock_connect();
    if (sock < 0) { fprintf(stderr, "[shim opendir] sock fail → real\n"); return r_opendir(path); }
    char req[1280];
    int rn = snprintf(req, sizeof(req), "LISTDIR %s\n", path);
    if (write(sock, req, rn) != rn) {
        close(sock); return r_opendir(path);
    }
    char first[128];
    if (read_line(sock, first, sizeof(first)) <= 0 ||
        strncmp(first, "OK", 2) != 0) {
        fprintf(stderr, "[shim opendir] LKL ERR: %s\n", first);
        close(sock); errno = ENOENT; return NULL;
    }

    /* Olvassuk az entry-line-okat, üres-sorig. */
    int cap = 16, nentries = 0;
    char **entries = malloc(cap * sizeof(char *));
    if (!entries) { close(sock); errno = ENOMEM; return NULL; }
    char line[512];
    while (1) {
        ssize_t r = read_line(sock, line, sizeof(line));
        if (r <= 0) break;
        if (line[0] == '\n' || line[0] == 0) break;
        /* trim newline */
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (l == 0) break;
        if (nentries >= cap) {
            cap *= 2;
            char **nb = realloc(entries, cap * sizeof(char *));
            if (!nb) break;
            entries = nb;
        }
        entries[nentries++] = strdup(line);
    }
    close(sock);

    struct lkl_dir *d = calloc(1, sizeof(*d));
    if (!d) {
        for (int i = 0; i < nentries; i++) free(entries[i]);
        free(entries);
        errno = ENOMEM; return NULL;
    }
    d->magic = LKL_DIR_MAGIC;
    d->entries = entries;
    d->nentries = nentries;
    d->pos = 0;
    return (DIR *)d;
}

struct dirent *readdir(DIR *dirp)
{
    /* LKL-DIR? az első 4 byte magic. */
    if (dirp) {
        struct lkl_dir *d = (struct lkl_dir *)dirp;
        if (d->magic == LKL_DIR_MAGIC) {
            if (d->pos >= d->nentries) return NULL;
            const char *name = d->entries[d->pos++];
            memset(&d->ent, 0, sizeof(d->ent));
            d->ent.d_ino = d->pos;
            d->ent.d_off = d->pos;
            d->ent.d_reclen = sizeof(d->ent);
            d->ent.d_type = DT_UNKNOWN;
            strncpy(d->ent.d_name, name, sizeof(d->ent.d_name) - 1);
            return &d->ent;
        }
    }
    /* Real DIR* → real readdir */
    static struct dirent *(*r_readdir)(DIR *) = NULL;
    INIT(readdir);
    if (!r_readdir) r_readdir = dlsym(RTLD_NEXT, "readdir");
    return r_readdir ? r_readdir(dirp) : NULL;
}

int closedir(DIR *dirp)
{
    if (dirp) {
        struct lkl_dir *d = (struct lkl_dir *)dirp;
        if (d->magic == LKL_DIR_MAGIC) {
            for (int i = 0; i < d->nentries; i++) free(d->entries[i]);
            free(d->entries);
            free(d);
            return 0;
        }
    }
    static int (*r_closedir)(DIR *) = NULL;
    if (!r_closedir) r_closedir = dlsym(RTLD_NEXT, "closedir");
    return r_closedir ? r_closedir(dirp) : 0;
}

/* ────────────────────────────────────────────────────────────────────
 *  socket() override — fake udev-monitor (NETLINK_KOBJECT_UEVENT)
 *
 *  A libusb-1.0-Debian-build kötelezően udev-szel init-el: a hotplug-monitor
 *  netlink-socket-jét nyitja, és ha az fail-el, libusb_init = -99
 *  ('LIBUSB_ERROR_NOT_SUPPORTED'). Az Android user-mode-on nincs udev-démon
 *  → fail. A LIBUSB_DISABLE_UDEV env nem hat (compile-time).
 *
 *  Fix: a shim átveszi a NETLINK_KOBJECT_UEVENT socket-hívást, és egy
 *  socketpair() egyik végét adja vissza. A libusb azt hiszi van udev-monitor;
 *  recvmsg sosem ad event-et (csendben blocked-pollol), DE az init megy.
 *
 *  KIEGÉSZÍTÉS: a libudev a fake fd-n bind() + setsockopt(SOL_NETLINK,...)-ot
 *  hív; ezek az AF_UNIX fd-n EINVAL/ENOPROTOOPT-tal failelnek → libusb -99.
 *  Ezért INTERCEPT-eljük a bind/setsockopt-ot is, és AF_NETLINK addr-okra
 *  ill. SOL_NETLINK level-re NO-OP-ot adunk vissza.
 * ──────────────────────────────────────────────────────────────────── */
static int (*r_socket)(int, int, int) = NULL;
static int (*r_bind)(int, const struct sockaddr *, socklen_t) = NULL;
static int (*r_setsockopt)(int, int, int, const void *, socklen_t) = NULL;

#ifndef NETLINK_KOBJECT_UEVENT
#define NETLINK_KOBJECT_UEVENT 15
#endif
#ifndef SOL_NETLINK
#define SOL_NETLINK 270
#endif

int socket(int domain, int type, int protocol)
{
    INIT(socket);
    if (domain == AF_NETLINK && protocol == NETLINK_KOBJECT_UEVENT) {
        int sp[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) < 0) return -1;
        /* a peer-vég (sp[1]) sosem ad data-t → recvmsg blocked-marad,
         * de NEM fail-el. A libusb init OK. */
        fprintf(stderr, "[shim socket] fake netlink fd=%d\n", sp[0]);
        return sp[0];
    }
    return r_socket(domain, type, protocol);
}

/* bind() override — AF_NETLINK addr-okra no-op (fake netlink fd-n a bind
 * úgyis EINVAL-lal failelne). A nem-netlink hívások passzolódnak tovább. */
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    INIT(bind);
    if (addr && addr->sa_family == AF_NETLINK) {
        fprintf(stderr, "[shim bind] AF_NETLINK fd=%d → no-op (OK)\n", sockfd);
        return 0;
    }
    return r_bind(sockfd, addr, addrlen);
}

/* setsockopt() override — SOL_NETLINK level-re no-op (AF_UNIX fd-n a
 * setsockopt(NETLINK_*) ENOPROTOOPT-tal failelne). */
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    INIT(setsockopt);
    if (level == SOL_NETLINK) {
        return 0;
    }
    return r_setsockopt(sockfd, level, optname, optval, optlen);
}

/* Constructor — minden indításkor stderr-re log (egyszerű diag). */
__attribute__((constructor))
static void shim_init(void)
{
    fprintf(stderr, "[kali-fuse-shim] LD_PRELOAD aktív (sock=%s)\n", SOCK_PATH);
}
