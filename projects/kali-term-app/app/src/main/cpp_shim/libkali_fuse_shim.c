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
#include <sys/sysmacros.h>  /* makedev() — libudev replacement */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/vfs.h>
#include <sys/statfs.h>
#include <linux/magic.h>
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

/* DEBUG mód env-flag-re: a stderr-üzenetek alapból CSENDESEK, hogy
 * ne keveredjenek a Kali programok normál outputjába (pl. lsusb-ben).
 * Bekapcsolható: KALITERM_SHIM_DEBUG=1 a launch.sh env-jében.
 * KORÁN definiált, hogy minden lejjebb-lévő használat lássa. */
static int shim_dbg_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("KALITERM_SHIM_DEBUG");
        cached = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return cached;
}
#define SHIM_DBG(...) do { if (shim_dbg_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

/* Path-prefix-szerinti LKL-route-döntés.
 *
 * /sys, /proc: LKL-szolgáltatott élő-FS, route-eljük az LKL-be.
 *
 * /dev/bus: NE LKL-route! Az LKL devtmpfs-en NINCS /dev/bus/usb (udev kreálná
 * a host-Linuxon). A user-mode mirror-megközelítés szolgáltatja a placeholder-
 * fát; a shim ne menjen LKL-be — fel-bukna ENOENT-en. */
/* PHASE-B-hijack-extension: a file-op route-olást VISSZAKAPCSOLOM.
 * Cél: minden /sys, /proc, /dev syscallt LKL-kernelre küldünk a control-
 * socketen át. A bash a :lkl-gyermek, fork+execve+LD_PRELOAD-on át tölti
 * be ezt a shim-et. Az opendir/readdir-be valódi host-fd-t kreálunk
 * (memfd_create), hogy a libsystemd `dirfd(d) >= 0` assertion-je elfogadja.
 * Ezzel a libudev a /sys-en valódi LKL-tartalmat lát LIVE-MODE-ban. */
/* Forward declarations */
static int is_lkl_path(const char *path);
static int is_virt_fs_path(const char *p);

/* PHASE 4 — A control-socket-on-keresztüli LKL file-routing-ot KIKAPCSOLVA
 * tartjuk általánosan (régi dirfd-assertion bug). Helyette: a libudev
 * REPLACEMENT (lentebb) kezeli a /sys/bus/usb enumeráció specifikus
 * problémáját közvetlen libudev-API-override-okkal.
 *
 * EGY KIVÉTEL: /dev/kmsg — a chrooted dmesg ezt nyitja, és az LKL kernel
 * ring-buffer-jét akarja kiolvasni. Ezt explicit LKL-routon küldjük át. */
static int is_lkl_path(const char *path)
{
    if (path && strcmp(path, "/dev/kmsg") == 0) return 1;
    return 0;
}

/* Forward declarations — a sock_connect és read_line lentebb vannak
 * definiálva, de a /dev/kmsg-handlerből hívjuk őket. */
static int sock_connect(void);
static ssize_t read_line(int sock, char *buf, size_t bufsz);

/* Speciálisan a /dev/kmsg-re: a control-socket KMSG parancsa az LKL
 * syslog(2) hívást futtatja és visszaadja a ring-buffer-t. A shim
 * egyszerűsíti az open-flow-t — nincs OPEN/READ-cycle, egy hívásban
 * megkapjuk az egész output-ot, és in-memory tartjuk a fd-hez. */
struct kmsg_buf {
    char *data;
    size_t len;
    size_t pos;
};
#define MAX_KMSG_BUFS 16
static struct kmsg_buf g_kmsg[MAX_KMSG_BUFS];
static pthread_mutex_t g_kmsg_lock = PTHREAD_MUTEX_INITIALIZER;

static int kmsg_open_via_lkl(void)
{
    int sock = sock_connect();
    if (sock < 0) return -1;
    if (write(sock, "KMSG\n", 5) != 5) { close(sock); return -1; }
    char first[64];
    if (read_line(sock, first, sizeof(first)) <= 0) {
        close(sock); return -1;
    }
    long size = 0;
    if (sscanf(first, "OK size=%ld", &size) != 1 || size < 0) {
        close(sock); return -1;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { close(sock); return -1; }
    size_t got = 0;
    INIT(read);
    while (got < (size_t)size) {
        ssize_t r = r_read(sock, buf + got, size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(sock);
    buf[got] = 0;

    pthread_mutex_lock(&g_kmsg_lock);
    for (int i = 0; i < MAX_KMSG_BUFS; i++) {
        if (g_kmsg[i].data == NULL) {
            g_kmsg[i].data = buf;
            g_kmsg[i].len = got;
            g_kmsg[i].pos = 0;
            pthread_mutex_unlock(&g_kmsg_lock);
            return MAGIC_FD_BASE + MAX_LKL_FDS + i;  /* külön namespace */
        }
    }
    pthread_mutex_unlock(&g_kmsg_lock);
    free(buf);
    return -1;
}

static int is_kmsg_fd(int fd) {
    return fd >= MAGIC_FD_BASE + MAX_LKL_FDS &&
           fd <  MAGIC_FD_BASE + MAX_LKL_FDS + MAX_KMSG_BUFS;
}
static struct kmsg_buf *get_kmsg(int fd) {
    if (!is_kmsg_fd(fd)) return NULL;
    return &g_kmsg[fd - (MAGIC_FD_BASE + MAX_LKL_FDS)];
}

/* ────────────────────────────────────────────────────────────────────
 *  NETLINK socket-route — AF_NETLINK → LKL kernel
 *
 *  Az iw, wpa_supplicant, nl80211-userspace toolok AF_NETLINK socket-tel
 *  beszélnek a cfg80211/nl80211 driver-okhoz. Android-on a chrooted process
 *  AF_NETLINK syscallja az Android kernel netlinkjére megy, NEM az LKL
 *  kernelre, ahol a Wi-Fi driver (rtl8xxxu / rtw88 / mac80211) él.
 *
 *  Megoldás: a libc `socket()`-jét felülírjuk. Ha AF_NETLINK kérés érkezik,
 *  egy LKL-belső netlink socket-et nyitunk a control-socket NLOPEN parancsán
 *  át, és magic-fd-t adunk vissza. A subsequenct bind/sendto/recvfrom/
 *  sendmsg/recvmsg/setsockopt/getsockname/close hívásokat is intercept-eljük
 *  és LKL-route-oljuk. */
#define NL_FD_BASE     (MAGIC_FD_BASE + MAX_LKL_FDS + MAX_KMSG_BUFS)
#define MAX_NL_FDS     64

struct nl_slot {
    int  sock;        /* per-fd control-socket connection */
    long lkl_fd;      /* LKL-kernel-belső netlink fd */
};
static struct nl_slot g_nl[MAX_NL_FDS];
static pthread_mutex_t g_nl_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_nl_fd(int fd) {
    return fd >= NL_FD_BASE && fd < NL_FD_BASE + MAX_NL_FDS;
}
static struct nl_slot *get_nl(int fd) {
    if (!is_nl_fd(fd)) return NULL;
    pthread_mutex_lock(&g_nl_lock);
    struct nl_slot *s = &g_nl[fd - NL_FD_BASE];
    pthread_mutex_unlock(&g_nl_lock);
    return (s->sock > 0) ? s : NULL;
}
static int alloc_nl_slot(int sock, long lkl_fd)
{
    pthread_mutex_lock(&g_nl_lock);
    for (int i = 0; i < MAX_NL_FDS; i++) {
        if (g_nl[i].sock == 0) {
            g_nl[i].sock = sock;
            g_nl[i].lkl_fd = lkl_fd;
            pthread_mutex_unlock(&g_nl_lock);
            return NL_FD_BASE + i;
        }
    }
    pthread_mutex_unlock(&g_nl_lock);
    return -1;
}
static void free_nl_slot(int fd)
{
    if (!is_nl_fd(fd)) return;
    INIT(close);
    pthread_mutex_lock(&g_nl_lock);
    struct nl_slot *s = &g_nl[fd - NL_FD_BASE];
    int sock = s->sock;
    s->sock = 0; s->lkl_fd = -1;
    pthread_mutex_unlock(&g_nl_lock);
    if (sock > 0) r_close(sock);
}
static void free_kmsg(int fd) {
    if (!is_kmsg_fd(fd)) return;
    pthread_mutex_lock(&g_kmsg_lock);
    struct kmsg_buf *k = &g_kmsg[fd - (MAGIC_FD_BASE + MAX_LKL_FDS)];
    free(k->data); k->data = NULL; k->len = 0; k->pos = 0;
    pthread_mutex_unlock(&g_kmsg_lock);
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
 *
 *  KRITIKUS: a `-D_FILE_OFFSET_BITS=64` glibc header miatt a `<fcntl.h>`
 *  `extern int open() __asm__("open64")` rename-t használ. A C-szintű
 *  `__attribute__((alias))` HATÁSTALAN — a compiler az asm-rename-elt
 *  névre rakja az aliast (open → open64).
 *
 *  Megoldás: file-scope `__asm__`-ban közvetlenül emittáljuk a `.global`
 *  és `.set` direktívákat, ami a linker-szintű szimbólum-táblába kerül
 *  bypass-olva a C-szintű renaming-et. Így MIND a `open`/`openat`, MIND
 *  a `open64`/`openat64` szimbólumok exportálódnak, mind a my_open_impl /
 *  my_openat_impl-re mutatva.
 * ──────────────────────────────────────────────────────────────────── */
int my_open_impl(const char *path, int flags, ...);
int my_openat_impl(int dirfd, const char *path, int flags, ...);

/* Linker-szintű alias-ok — bypass-olja a glibc asm-rename-et */
__asm__(".globl open\n\t.set open, my_open_impl");
__asm__(".globl openat\n\t.set openat, my_openat_impl");
__asm__(".globl open64\n\t.set open64, my_open_impl");
__asm__(".globl openat64\n\t.set openat64, my_openat_impl");

int my_open_impl(const char *path, int flags, ...)
{
    INIT(open);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    if (!is_lkl_path(path)) {
        int rc = r_open(path, flags, mode);
        if (is_virt_fs_path(path))
            SHIM_DBG("[shim open] path=%s flags=0x%x rc=%d\n", path, flags, rc);
        return rc;
    }

    /* SPECIAL: /dev/kmsg → KMSG control-socket parancs.
     * Egyetlen hívás visszaadja az LKL ring-buffer teljes tartalmát,
     * az in-memory buffer-be tesszük, a fd-t magic-namespace-ben adjuk. */
    if (strcmp(path, "/dev/kmsg") == 0) {
        int kfd = kmsg_open_via_lkl();
        if (kfd >= 0) return kfd;
        errno = ENOENT;
        return -1;
    }

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

int my_openat_impl(int dirfd, const char *path, int flags, ...)
{
    INIT(openat);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    /* Csak az abszolút path-t LKL-routol; relatív path-t a real-libc */
    if (path && path[0] == '/' && is_lkl_path(path)) {
        return my_open_impl(path, flags, mode);
    }
    int rc = r_openat(dirfd, path, flags, mode);
    if (path && is_virt_fs_path(path))
        SHIM_DBG("[shim openat] dirfd=%d path=%s flags=0x%x rc=%d\n",
                dirfd, path, flags, rc);
    return rc;
}

/* ────────────────────────────────────────────────────────────────────
 *  klogctl() — syslog(2) libc-wrapper intercept
 *
 *  A util-linux `dmesg` ALAPÉRTELMEZÉSben a `klogctl()` libc-wrapper-en
 *  át hív (NEM /dev/kmsg-et nyit). Android-kernelen a klogctl syscall
 *  ENOSYS-szel tér vissza (SELinux blokk) → dmesg "Function not
 *  implemented"-tel kilép, mielőtt a /dev/kmsg fallback-re jutna.
 *
 *  Megoldás: a `klogctl` libc-szimbólumot LD_PRELOAD-szal felülírjuk,
 *  és a SYSLOG_ACTION_READ_ALL / SIZE_BUFFER műveleteket az LKL kernel
 *  ring-bufferéből szolgáljuk ki a KMSG control-socket parancson át.
 *
 *  type értékek (Linux sys/klog.h):
 *     3 = SYSLOG_ACTION_READ_ALL    — visszaadja a teljes ring-buffert
 *    10 = SYSLOG_ACTION_SIZE_BUFFER — ring-buffer teljes méretét adja
 *     9 = SYSLOG_ACTION_SIZE_UNREAD — még nem olvasott byte-ok száma
 * ──────────────────────────────────────────────────────────────────── */
int klogctl(int type, char *bufp, int len)
{
    /* Egyetlen kmsg-snapshot az egész klogctl-streamhez (folyamatosan
     * újra lekérhetjük, de a buffer minden hívásnál friss). */
    int kfd = kmsg_open_via_lkl();
    if (kfd < 0) { errno = ENOSYS; return -1; }
    struct kmsg_buf *k = get_kmsg(kfd);
    if (!k || !k->data) { free_kmsg(kfd); errno = ENOSYS; return -1; }

    long rc = -1;
    switch (type) {
    case 3:   /* READ_ALL */
    case 4: { /* READ_CLEAR — clear-t nem implementáljuk, csak read */
        if (!bufp || len <= 0) { errno = EINVAL; break; }
        size_t take = (size_t)len < k->len ? (size_t)len : k->len;
        memcpy(bufp, k->data, take);
        rc = (long)take;
        break;
    }
    case 9:   /* SIZE_UNREAD — egyszerűen a teljes méret */
    case 10:  /* SIZE_BUFFER — ugyanaz */
        rc = (long)k->len;
        break;
    case 0:   /* CLOSE */
    case 1:   /* OPEN */
    case 5:   /* CLEAR — no-op */
    case 6:   /* CONSOLE_OFF */
    case 7:   /* CONSOLE_ON */
    case 8:   /* CONSOLE_LEVEL */
        rc = 0;
        break;
    default:
        errno = EINVAL;
        rc = -1;
        break;
    }
    free_kmsg(kfd);
    return (int)rc;
}

/* MEGJEGYZÉS: a `syslog(3)` user-facing logger (NEM syscall) marad
 * érintetlen — csak a `klogctl` syscall-wrappert override-oljuk. */

/* ────────────────────────────────────────────────────────────────────
 *  read() / pread()
 * ──────────────────────────────────────────────────────────────────── */

ssize_t read(int fd, void *buf, size_t count)
{
    INIT(read);
    if (fd < MAGIC_FD_BASE) return r_read(fd, buf, count);

    /* KMSG-fd: az in-memory buffer-ből szolgálunk. */
    if (is_kmsg_fd(fd)) {
        struct kmsg_buf *k = get_kmsg(fd);
        if (!k || !k->data) { errno = EBADF; return -1; }
        pthread_mutex_lock(&g_kmsg_lock);
        size_t avail = k->len - k->pos;
        if (avail == 0) {
            pthread_mutex_unlock(&g_kmsg_lock);
            return 0;  /* EOF */
        }
        size_t take = count < avail ? count : avail;
        memcpy(buf, k->data + k->pos, take);
        k->pos += take;
        pthread_mutex_unlock(&g_kmsg_lock);
        return (ssize_t)take;
    }

    /* NL-fd: read() = recvfrom() (POSIX equivalent). iproute2/libnl gyakran
     * read()-et hív a netlink fd-n recvfrom helyett. */
    if (is_nl_fd(fd)) {
        return recvfrom(fd, buf, count, 0, NULL, NULL);
    }

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
 *  poll() / select() — netlink-fd-re fake-ready visszaadás.
 *
 *  libnl/iproute2 a netlink-fd-en poll()/ppoll()-t hív, hogy várjon a
 *  response-ra. Mivel a magic-fd nem valódi Linux fd, a kernel POLLNVAL-t
 *  ad. Megoldás: ha a poll-fd-set TARTALMAZZA netlink-magic-fd-t, azt
 *  külön kezeljük: POLLIN ready-t adunk (tudjuk, hogy a következő read
 *  blokkolva fog várni a control-socketen — az LKL kernel maga timeout-ol).
 * ──────────────────────────────────────────────────────────────────── */
static int (*r_poll)(struct pollfd *, nfds_t, int) = NULL;
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (!r_poll) r_poll = dlsym(RTLD_NEXT, "poll");
    int has_magic = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd >= MAGIC_FD_BASE) { has_magic = 1; break; }
    }
    if (!has_magic) return r_poll(fds, nfds, timeout);

    /* Magic-fd-ek: instant POLLIN-ready visszaadunk (recvfrom blokkol majd
     * a control-socketen, ami az LKL kernel-választ várja). Real-fd-eket
     * is benne hagyjuk: ezek-re a poll külön nem fog futni, mert csak az
     * első magic-fd-re reagálunk azonnal. */
    int n_ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd >= MAGIC_FD_BASE) {
            fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
            if (fds[i].revents) n_ready++;
        } else {
            fds[i].revents = 0;
        }
    }
    return n_ready;
}

/* ────────────────────────────────────────────────────────────────────
 *  write() — NL-fd-re sendto() ekvivalens, többi magic-fd-re EBADF.
 * ──────────────────────────────────────────────────────────────────── */
static ssize_t (*r_write)(int, const void *, size_t) = NULL;
ssize_t write(int fd, const void *buf, size_t count)
{
    if (!r_write) r_write = dlsym(RTLD_NEXT, "write");
    if (fd < MAGIC_FD_BASE) return r_write(fd, buf, count);
    if (is_nl_fd(fd)) {
        return sendto(fd, buf, count, 0, NULL, 0);
    }
    /* KMSG és LKL file fd-re a write nem támogatott. */
    errno = EBADF;
    return -1;
}

/* ────────────────────────────────────────────────────────────────────
 *  ioctl() — magic-fd-re: NL-fd-en a netlink-specifikus ioctl-eket
 *  (SIOCGIFCONF, stb.) jelenleg NEM rout-eljuk LKL-be — visszaadunk
 *  EINVAL-t/0-t a libnl-flow folytatásához. A jövőben routerelhető.
 * ──────────────────────────────────────────────────────────────────── */
static int (*r_ioctl)(int, unsigned long, ...) = NULL;
int my_ioctl_impl(int fd, unsigned long request, ...);
__asm__(".globl ioctl\n\t.set ioctl, my_ioctl_impl");

int my_ioctl_impl(int fd, unsigned long request, ...)
{
    if (!r_ioctl) r_ioctl = dlsym(RTLD_NEXT, "ioctl");
    va_list ap; va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (fd < MAGIC_FD_BASE) return r_ioctl(fd, request, arg);
    /* Magic-fd-re: tipikus libnl ioctl (SIOCGSTAMP, FIONREAD) — no-op
     * 0-val, hogy a flow-t ne abort-olja. Strict-ioctl-igényű hívásokra
     * (tty TCGETS, stb.) ENOTTY-t adunk. */
    switch (request) {
    case 0x541b:  /* FIONREAD — bytes available; legbiztonságosabb 0 */
        if (arg) *(int *)arg = 0;
        return 0;
    default:
        errno = ENOTTY;
        return -1;
    }
}

/* ────────────────────────────────────────────────────────────────────
 *  close()
 * ──────────────────────────────────────────────────────────────────── */

int close(int fd)
{
    INIT(close);
    if (fd < MAGIC_FD_BASE) return r_close(fd);

    /* KMSG-fd: csak in-memory buffer-t kell felszabadítani. */
    if (is_kmsg_fd(fd)) {
        free_kmsg(fd);
        return 0;
    }

    /* NL-fd: NLCLOSE parancs az LKL-nek + per-fd socket bezárás. */
    if (is_nl_fd(fd)) {
        struct nl_slot *s = get_nl(fd);
        if (s) {
            char req[64];
            int n = snprintf(req, sizeof(req), "NLCLOSE %ld\n", s->lkl_fd);
            write(s->sock, req, n);
            char resp[64];
            read_line(s->sock, resp, sizeof(resp));
        }
        free_nl_slot(fd);
        return 0;
    }

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

/* Helper: /sys, /proc, /dev path-prefix-ekre fprintf-debug. Ki tudjuk
 * deríteni MELYIK fájl-syscallt hív a libusb a "sysfs not mounted"
 * check-jéhez. */
static int is_virt_fs_path(const char *p)
{
    if (!p || p[0] != '/') return 0;
    if (strncmp(p, "/sys",  4) == 0 && (p[4]  == '\0' || p[4]  == '/')) return 1;
    if (strncmp(p, "/proc", 5) == 0 && (p[5]  == '\0' || p[5]  == '/')) return 1;
    if (strncmp(p, "/dev",  4) == 0 && (p[4]  == '\0' || p[4]  == '/')) return 1;
    return 0;
}

/* stat/lstat/fstat: FILE_OFFSET_BITS=64 a `stat`/`lstat`/`fstat` szimbólumokat
 * a 64-es változatra renamel-i (asm-rename). Explicit asm-alias-szal mindkét
 * névre exportáljuk az implementációt. */
int my_stat_impl(const char *path, struct stat *st);
int my_lstat_impl(const char *path, struct stat *st);
int my_fstat_impl(int fd, struct stat *st);
__asm__(".globl stat\n\t.set stat, my_stat_impl");
__asm__(".globl stat64\n\t.set stat64, my_stat_impl");
__asm__(".globl lstat\n\t.set lstat, my_lstat_impl");
__asm__(".globl lstat64\n\t.set lstat64, my_lstat_impl");
__asm__(".globl fstat\n\t.set fstat, my_fstat_impl");
__asm__(".globl fstat64\n\t.set fstat64, my_fstat_impl");

int my_stat_impl(const char *path, struct stat *st)
{
    INIT(stat);
    int rc = r_stat(path, st);
    if (is_virt_fs_path(path))
        SHIM_DBG("[shim stat] path=%s rc=%d\n", path, rc);
    return rc;
}

int my_lstat_impl(const char *path, struct stat *st)
{
    INIT(lstat);
    int rc = r_lstat(path, st);
    if (is_virt_fs_path(path))
        SHIM_DBG("[shim lstat] path=%s rc=%d\n", path, rc);
    return rc;
}

int my_fstat_impl(int fd, struct stat *st)
{
    INIT(fstat);
    if (fd < MAGIC_FD_BASE) return r_fstat(fd, st);
    /* NL-fd-re: fake-stat (socket-mode). */
    if (is_nl_fd(fd)) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFSOCK | 0666;
        return 0;
    }
    /* LKL-fd-re: fake-stat (regular file). */
    return fake_stat_lkl(NULL, st);
}

/* fcntl() intercept — magic-fd-ekre (LKL-routed netlink, KMSG, file) a kernel
 * EBADF-fel utasítja vissza, mert nem valódi Linux fd. A libnl/iproute2
 * F_GETFD/F_SETFD/F_GETFL/F_SETFL hívásokat csinál a netlink socket-en —
 * EBADF visszaadás esetén "Cannot send dump request: Bad file descriptor".
 * Cache-elt flag-eket adunk vissza per-fd, ami libnl-nek elég.
 *
 * KRITIKUS: a `-D_FILE_OFFSET_BITS=64` glibc <fcntl.h> a `fcntl()`-t
 * `fcntl64`-ként asm-renamel. Az `open`/`openat`-hez hasonlóan explicit
 * `.globl + .set` direktívákkal a `fcntl` szimbólumot is exportáljuk. */
int my_fcntl_impl(int fd, int cmd, ...);
__asm__(".globl fcntl\n\t.set fcntl, my_fcntl_impl");
__asm__(".globl fcntl64\n\t.set fcntl64, my_fcntl_impl");

static int (*r_fcntl)(int, int, ...) = NULL;
int my_fcntl_impl(int fd, int cmd, ...)
{
    if (!r_fcntl) r_fcntl = dlsym(RTLD_NEXT, "fcntl");
    /* arg: F_GETFL/F_GETFD nem vesznek argot, F_SETFL/F_SETFD int-et,
     * F_SETLK struct flock*-ot. Egyetlen va_arg-pattern fed le mindent. */
    va_list ap; va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (fd < MAGIC_FD_BASE) return r_fcntl(fd, cmd, arg);
    /* Magic-fd-re: leggyakoribb fcntl-cmd-eket no-opnak/cached-flag-nek
     * vesszük. A libnl O_NONBLOCK-ot tipikusan beállítja; mi visszaadjuk. */
    switch (cmd) {
    case F_GETFD: return 0;            /* close-on-exec: nem fontos a magic-fd-n */
    case F_SETFD: return 0;
    case F_GETFL: return O_RDWR;       /* alapértelmezett mode */
    case F_SETFL: return 0;            /* no-op (libnl O_NONBLOCK kérése elfogadva) */
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        /* dup-ot nem támogatunk a magic-fd-re — visszaadhat EBADF-et libnl,
         * de tipikus useshez nincs szükség dup-ra. */
        errno = ENOTSUP; return -1;
    default:
        /* Ismeretlen cmd-re csendben sikerrel térünk vissza — biztonságosabb
         * mint EBADF, ami abort-olja a libnl flow-t. */
        return 0;
    }
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
    int rc = r_access(path, mode);
    if (is_virt_fs_path(path))
        SHIM_DBG("[shim access] path=%s mode=0x%x rc=%d\n", path, mode, rc);
    return rc;
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

    SHIM_DBG("[shim opendir LKL] %s\n", path);
    int sock = sock_connect();
    if (sock < 0) { SHIM_DBG("[shim opendir] sock fail → real\n"); return r_opendir(path); }
    char req[1280];
    int rn = snprintf(req, sizeof(req), "LISTDIR %s\n", path);
    if (write(sock, req, rn) != rn) {
        close(sock); return r_opendir(path);
    }
    char first[128];
    if (read_line(sock, first, sizeof(first)) <= 0 ||
        strncmp(first, "OK", 2) != 0) {
        SHIM_DBG("[shim opendir] LKL ERR: %s\n", first);
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

/* DEBUG mód env-flag-re: a stderr-üzenetek alapból CSENDESEK, hogy
 * ne keveredjenek a Kali programok normál outputjába (pl. lsusb-ben).
 * Bekapcsolható: KALITERM_SHIM_DEBUG=1 a launch.sh env-jében.
 * FORWARD-MOVED: a SHIM_DBG-t a többi shim-print elé pakoltuk, lentebb
 * már csak a fake netlink/bind/setsockopt-marad. */

#ifndef NETLINK_GENERIC
#define NETLINK_GENERIC 16
#endif

int socket(int domain, int type, int protocol)
{
    INIT(socket);
    if (domain == AF_NETLINK && protocol == NETLINK_KOBJECT_UEVENT) {
        /* udev-monitor stub — socketpair-rel csendesen blokkolt poll. */
        int sp[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) < 0) return -1;
        SHIM_DBG("[shim socket] fake udev netlink fd=%d\n", sp[0]);
        return sp[0];
    }
    /* MINDEN AF_NETLINK protokoll (kivéve UEVENT/KOBJECT amit fent fake-elünk)
     * → LKL kernel netlink stack. NETLINK_ROUTE (proto 0) = ifconfig/ip/route,
     * NETLINK_GENERIC (16) = iw/nl80211, NETLINK_NETFILTER (12), stb. */
    if (domain == AF_NETLINK) {
        int sock = sock_connect();
        if (sock < 0) {
            SHIM_DBG("[shim socket NL_GENERIC] sock_connect fail → real\n");
            return r_socket(domain, type, protocol);
        }
        char req[64];
        int rn = snprintf(req, sizeof(req), "NLOPEN %d %d\n", type, protocol);
        if (write(sock, req, rn) != rn) { close(sock); errno = EIO; return -1; }
        char resp[128];
        if (read_line(sock, resp, sizeof(resp)) <= 0) {
            close(sock); errno = EIO; return -1;
        }
        long lkl_fd = -1;
        if (sscanf(resp, "OK fd=%ld", &lkl_fd) == 1 && lkl_fd >= 0) {
            int magic = alloc_nl_slot(sock, lkl_fd);
            if (magic > 0) {
                SHIM_DBG("[shim socket NL_GENERIC] magic_fd=%d lkl_fd=%ld\n",
                         magic, lkl_fd);
                return magic;
            }
            close(sock); errno = EMFILE; return -1;
        }
        int err = 0; sscanf(resp, "ERR errno=%d", &err);
        close(sock); errno = err ? err : EIO;
        return -1;
    }
    return r_socket(domain, type, protocol);
}

/* bind() override:
 *   1. LKL-routed magic-fd (NL_GENERIC) → NLBIND control-socket parancs
 *   2. fake udev fd-n AF_NETLINK addr → no-op (socketpair-rel kompat)
 *   3. egyéb → real bind */
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    INIT(bind);
    if (is_nl_fd(sockfd) && addr && addr->sa_family == AF_NETLINK) {
        struct nl_slot *s = get_nl(sockfd);
        if (!s) { errno = EBADF; return -1; }
        /* sockaddr_nl layout: family(2) + pad(2) + pid(4) + groups(4) */
        const struct sockaddr_nl *nla = (const struct sockaddr_nl *)addr;
        char req[80];
        int rn = snprintf(req, sizeof(req), "NLBIND %ld %u %u\n",
                          s->lkl_fd, nla->nl_pid, nla->nl_groups);
        if (write(s->sock, req, rn) != rn) { errno = EIO; return -1; }
        char resp[64];
        if (read_line(s->sock, resp, sizeof(resp)) <= 0) { errno = EIO; return -1; }
        if (strncmp(resp, "OK", 2) == 0) return 0;
        int err = 0; sscanf(resp, "ERR errno=%d", &err);
        errno = err ? err : EIO;
        return -1;
    }
    if (addr && addr->sa_family == AF_NETLINK) {
        /* fake udev netlink fd-n — no-op */
        return 0;
    }
    return r_bind(sockfd, addr, addrlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    INIT(setsockopt);
    if (is_nl_fd(sockfd)) {
        /* SOL_NETLINK-szintű opciók (NETLINK_ADD_MEMBERSHIP, NETLINK_PKTINFO,
         * stb.) → LKL-be route. A SOL_SOCKET-szintű hint-eket (SO_SNDBUF,
         * SO_RCVBUF, SO_PASSCRED, stb.) csendesen accept-eljük — libnl/iw
         * ezek hibájával NEM számol, és az LKL non-priv namespace gyakran
         * EPERM-mel utasítaná el → "ip link" / "iw dev" elhasalna. */
        if (level != SOL_NETLINK) {
            return 0;
        }
        struct nl_slot *s = get_nl(sockfd);
        if (!s) { errno = EBADF; return -1; }
        if (optlen > 256) { errno = EINVAL; return -1; }
        char req[96];
        int rn = snprintf(req, sizeof(req), "NLSETSO %ld %d %d %d\n",
                          s->lkl_fd, level, optname, (int)optlen);
        if (write(s->sock, req, rn) != rn) { errno = EIO; return -1; }
        if (optlen > 0 && write(s->sock, optval, optlen) != (ssize_t)optlen) {
            errno = EIO; return -1;
        }
        char resp[64];
        if (read_line(s->sock, resp, sizeof(resp)) <= 0) { errno = EIO; return -1; }
        if (strncmp(resp, "OK", 2) == 0) return 0;
        int err = 0; sscanf(resp, "ERR errno=%d", &err);
        /* Még a SOL_NETLINK opciókra is megengedőek vagyunk — a NETLINK_*
         * tagsági opciók néha non-fatal-ak az LKL-ben. */
        if (err == ENOPROTOOPT || err == EINVAL || err == EPERM) return 0;
        errno = err ? err : EIO;
        return -1;
    }
    if (level == SOL_NETLINK) {
        /* fake udev fd-n (NETLINK_KOBJECT_UEVENT socketpair) — no-op */
        return 0;
    }
    return r_setsockopt(sockfd, level, optname, optval, optlen);
}

/* sendto / recvfrom intercept — netlink-magic-fd-re LKL-route. */
static ssize_t (*r_sendto)(int, const void *, size_t, int,
                           const struct sockaddr *, socklen_t) = NULL;
static ssize_t (*r_recvfrom)(int, void *, size_t, int,
                             struct sockaddr *, socklen_t *) = NULL;
static ssize_t (*r_sendmsg)(int, const struct msghdr *, int) = NULL;
static ssize_t (*r_recvmsg)(int, struct msghdr *, int) = NULL;
static int     (*r_getsockname)(int, struct sockaddr *, socklen_t *) = NULL;

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen)
{
    if (!r_sendto) r_sendto = dlsym(RTLD_NEXT, "sendto");
    if (is_nl_fd(sockfd)) {
        struct nl_slot *s = get_nl(sockfd);
        if (!s) { errno = EBADF; return -1; }
        if (len > 65536) { errno = EMSGSIZE; return -1; }
        char req[96];
        int rn = snprintf(req, sizeof(req), "NLSEND %ld %zu\n", s->lkl_fd, len);
        if (write(s->sock, req, rn) != rn) { errno = EIO; return -1; }
        if (write(s->sock, buf, len) != (ssize_t)len) { errno = EIO; return -1; }
        char resp[64];
        if (read_line(s->sock, resp, sizeof(resp)) <= 0) { errno = EIO; return -1; }
        long sent = -1;
        if (sscanf(resp, "OK sent=%ld", &sent) == 1) return sent;
        int err = 0; sscanf(resp, "ERR errno=%d", &err);
        errno = err ? err : EIO;
        return -1;
    }
    return r_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    if (!r_recvfrom) r_recvfrom = dlsym(RTLD_NEXT, "recvfrom");
    if (is_nl_fd(sockfd)) {
        struct nl_slot *s = get_nl(sockfd);
        if (!s) { errno = EBADF; return -1; }
        if (len > 65536) len = 65536;
        char req[64];
        int rn = snprintf(req, sizeof(req), "NLRECV %ld %zu\n", s->lkl_fd, len);
        if (write(s->sock, req, rn) != rn) { errno = EIO; return -1; }
        char hdr[64];
        if (read_line(s->sock, hdr, sizeof(hdr)) <= 0) { errno = EIO; return -1; }
        long got = -1;
        if (sscanf(hdr, "OK len=%ld", &got) == 1) {
            INIT(read);
            size_t total = 0;
            while (total < (size_t)got) {
                ssize_t r = r_read(s->sock, (char *)buf + total, (size_t)got - total);
                if (r <= 0) break;
                total += r;
            }
            if (src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_nl)) {
                struct sockaddr_nl *sa = (struct sockaddr_nl *)src_addr;
                memset(sa, 0, sizeof(*sa));
                sa->nl_family = AF_NETLINK;
                *addrlen = sizeof(struct sockaddr_nl);
            }
            return (ssize_t)total;
        }
        int err = 0; sscanf(hdr, "ERR errno=%d", &err);
        errno = err ? err : EIO;
        return -1;
    }
    return r_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

/* sendmsg/recvmsg — netlink-fd-re EGY iovec-et flattenelünk és sendto-zunk.
 * iw / libnl egyetlen iov-ot küld, általában nincs cmsg sem. */
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
    if (!r_sendmsg) r_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
    if (is_nl_fd(sockfd)) {
        if (!msg || msg->msg_iovlen == 0) { errno = EINVAL; return -1; }
        /* Egyszerű eset: 1 iov */
        if (msg->msg_iovlen == 1) {
            return sendto(sockfd, msg->msg_iov[0].iov_base,
                          msg->msg_iov[0].iov_len, flags, NULL, 0);
        }
        /* Több iov: flattenelés egy lokál bufferbe */
        size_t total = 0;
        for (size_t i = 0; i < (size_t)msg->msg_iovlen; i++)
            total += msg->msg_iov[i].iov_len;
        if (total == 0 || total > 65536) { errno = EMSGSIZE; return -1; }
        char *buf = malloc(total);
        if (!buf) { errno = ENOMEM; return -1; }
        size_t off = 0;
        for (size_t i = 0; i < (size_t)msg->msg_iovlen; i++) {
            memcpy(buf + off, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
            off += msg->msg_iov[i].iov_len;
        }
        ssize_t rc = sendto(sockfd, buf, total, flags, NULL, 0);
        free(buf);
        return rc;
    }
    return r_sendmsg(sockfd, msg, flags);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    if (!r_recvmsg) r_recvmsg = dlsym(RTLD_NEXT, "recvmsg");
    if (is_nl_fd(sockfd)) {
        if (!msg || msg->msg_iovlen == 0) { errno = EINVAL; return -1; }
        size_t cap = 0;
        for (size_t i = 0; i < (size_t)msg->msg_iovlen; i++)
            cap += msg->msg_iov[i].iov_len;
        if (cap == 0) { errno = EINVAL; return -1; }
        if (cap > 65536) cap = 65536;
        char *buf = malloc(cap);
        if (!buf) { errno = ENOMEM; return -1; }
        socklen_t addrlen = msg->msg_namelen;
        ssize_t got = recvfrom(sockfd, buf, cap, flags,
                               (struct sockaddr *)msg->msg_name, &addrlen);
        if (got > 0) {
            size_t off = 0;
            for (size_t i = 0; i < (size_t)msg->msg_iovlen && off < (size_t)got; i++) {
                size_t take = msg->msg_iov[i].iov_len;
                if (take > (size_t)got - off) take = (size_t)got - off;
                memcpy(msg->msg_iov[i].iov_base, buf + off, take);
                off += take;
            }
            msg->msg_namelen = addrlen;
            msg->msg_controllen = 0;  /* nincs cmsg-támogatás */
            msg->msg_flags = 0;
        }
        free(buf);
        return got;
    }
    return r_recvmsg(sockfd, msg, flags);
}

/* getsockname — netlink-fd-en a kernel-assigned PID-et libnl várja vissza. */
int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    if (!r_getsockname) r_getsockname = dlsym(RTLD_NEXT, "getsockname");
    if (is_nl_fd(sockfd)) {
        struct nl_slot *s = get_nl(sockfd);
        if (!s) { errno = EBADF; return -1; }
        char req[64];
        int rn = snprintf(req, sizeof(req), "NLGETSN %ld\n", s->lkl_fd);
        if (write(s->sock, req, rn) != rn) { errno = EIO; return -1; }
        char resp[128];
        if (read_line(s->sock, resp, sizeof(resp)) <= 0) { errno = EIO; return -1; }
        unsigned pid = 0, groups = 0;
        if (sscanf(resp, "OK pid=%u groups=%u", &pid, &groups) == 2) {
            if (!addr || !addrlen || *addrlen < sizeof(struct sockaddr_nl)) {
                errno = EINVAL; return -1;
            }
            struct sockaddr_nl *sa = (struct sockaddr_nl *)addr;
            memset(sa, 0, sizeof(*sa));
            sa->nl_family = AF_NETLINK;
            sa->nl_pid    = pid;
            sa->nl_groups = groups;
            *addrlen = sizeof(struct sockaddr_nl);
            return 0;
        }
        int err = 0; sscanf(resp, "ERR errno=%d", &err);
        errno = err ? err : EIO;
        return -1;
    }
    return r_getsockname(sockfd, addr, addrlen);
}

/* statfs() override — libusb a sysfs jelenlétét úgy ellenőrzi, hogy
 * statfs(/sys, ...) → f_type == SYSFS_MAGIC. A proot bind-mountolt /sys
 * azonban a host ext4/f2fs filesystemén van (EXT4_SUPER_MAGIC) → libusb
 * "sysfs not mounted" warningot ad, és a teljes USB-enumeráció elszáll.
 *
 * Fake-eljük a typical /proc /sys /dev path-okra a megfelelő MAGIC-et.
 * A többi mezőt (f_blocks, f_files, stb.) a valódi statfs() hívás után
 * megtartjuk — csak az f_type értékét írjuk át. */
#ifndef SYSFS_MAGIC
#define SYSFS_MAGIC      0x62656572
#endif
#ifndef PROC_SUPER_MAGIC
#define PROC_SUPER_MAGIC 0x9fa0
#endif
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC      0x01021994
#endif
#ifndef DEVPTS_SUPER_MAGIC
#define DEVPTS_SUPER_MAGIC 0x1cd1
#endif

static int (*r_statfs)(const char *, struct statfs *) = NULL;

/* Egy path-prefix-match a tipikus virtual-FS gyökerekre. Visszatérési érték:
 * a megfelelő MAGIC vagy 0 ha a path nem érdekes (valódi statfs eredmény marad). */
static long fake_fs_magic_for_path(const char *path)
{
    if (!path) return 0;
    if (path[0] != '/') return 0;
    /* "/sys" vagy "/sys/..." */
    if (strncmp(path, "/sys", 4) == 0 && (path[4] == '\0' || path[4] == '/'))
        return SYSFS_MAGIC;
    /* "/proc" vagy "/proc/..." */
    if (strncmp(path, "/proc", 5) == 0 && (path[5] == '\0' || path[5] == '/'))
        return PROC_SUPER_MAGIC;
    /* "/dev/pts" — devpts elsőbbség */
    if (strncmp(path, "/dev/pts", 8) == 0 && (path[8] == '\0' || path[8] == '/'))
        return DEVPTS_SUPER_MAGIC;
    /* "/dev" — devtmpfs/tmpfs */
    if (strncmp(path, "/dev", 4) == 0 && (path[4] == '\0' || path[4] == '/'))
        return TMPFS_MAGIC;
    return 0;
}

int my_statfs_impl(const char *path, struct statfs *buf);
__asm__(".globl statfs\n\t.set statfs, my_statfs_impl");
__asm__(".globl statfs64\n\t.set statfs64, my_statfs_impl");

int my_statfs_impl(const char *path, struct statfs *buf)
{
    INIT(statfs);
    int rc = r_statfs(path, buf);
    long magic = fake_fs_magic_for_path(path);
    SHIM_DBG("[shim statfs] path=%s rc=%d magic_override=0x%lx\n",
            path ? path : "(null)", rc, magic);
    if (rc == 0 && buf && magic != 0) {
        buf->f_type = (typeof(buf->f_type))magic;
    }
    return rc;
}

/* aarch64-linux-gnu glibc-ben a statfs64() __asm__("statfs")-tal aliasolt
 * → ugyanaz a szimbólum mint a statfs(). Külön definíció duplicate-symbol
 * linker errort okoz. A statfs() override mindkettőt elkapja. */

/* Constructor — csak DEBUG módban logol (KALITERM_SHIM_DEBUG=1). */
__attribute__((constructor))
static void shim_init(void)
{
    SHIM_DBG("[kali-fuse-shim] LD_PRELOAD aktív (sock=%s) PID=%d\n",
             SOCK_PATH, getpid());
}

/* ────────────────────────────────────────────────────────────────────
 *  libudev REPLACEMENT — LKL-CONNECTED VIRTUAL UDEV
 *
 *  Indok: a Debian libusb-1.0-0 HAVE_LIBUDEV-vel van build-elve, az
 *  enumerate-hoz a libudev1.so-t használja. A systemd-szintű libudev
 *  szigorú belső validációkat (sd-device chase, syspath path-check, ...)
 *  csinál, amik a chroot+proot+materializált-sysfs környezetben FAIL-elnek
 *  még akkor is ha a /sys-ben minden adat ott van.
 *
 *  A shim LD_PRELOAD-szal FELÜL ÍRJA a libudev publikus API-ját egy
 *  egyszerű implementációval ami:
 *   1) udev_enumerate_scan_devices: walk /sys/bus/<subsystem>/devices/
 *   2) udev_device_new_from_syspath: parse uevent + idVendor/idProduct
 *   3) Egyszerű getterek a parsed property-kre
 *
 *  Forrás-adat: a materializált /sys/bus/usb/devices/* (amit a LKL-ből
 *  a populateLklProcMirror tölt fel). Tehát ÉRTELMileg az LKL kernel
 *  adata, csak nem a libudev általi szigorú validáción át.
 * ──────────────────────────────────────────────────────────────────── */

/* Forward declarations of opaque types — same ABI as upstream libudev */
struct udev;
struct udev_enumerate;
struct udev_list_entry;
struct udev_device;

/* === udev_list_entry (linked list of c-strings) === */
struct ku_list_entry {
    char *name;
    char *value;
    struct ku_list_entry *next;
};

static struct ku_list_entry *ku_list_append(struct ku_list_entry **head, const char *name, const char *value)
{
    struct ku_list_entry *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->name = name ? strdup(name) : NULL;
    e->value = value ? strdup(value) : NULL;
    if (!*head) {
        *head = e;
    } else {
        struct ku_list_entry *p = *head;
        while (p->next) p = p->next;
        p->next = e;
    }
    return e;
}

static void ku_list_free(struct ku_list_entry *head)
{
    while (head) {
        struct ku_list_entry *n = head->next;
        free(head->name);
        free(head->value);
        free(head);
        head = n;
    }
}

/* === udev "context" — minimal === */
struct ku_udev {
    int refcount;
};

/* === udev_enumerate === */
struct ku_enum {
    int refcount;
    struct ku_udev *udev;
    char *match_subsystem;
    struct ku_list_entry *devices;  /* syspath list */
};

/* === udev_device — parsed from /sys/<syspath>/uevent + attribute files === */
struct ku_device {
    int refcount;
    struct ku_udev *udev;
    char *syspath;
    char *subsystem;
    char *devnode;
    char *devtype;
    char *sysname;
    dev_t devnum;
    int initialized;
    struct ku_list_entry *properties;
    struct ku_list_entry *sysattrs;
};

/* === Public API === */

struct udev *udev_new(void)
{
    struct ku_udev *u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->refcount = 1;
    return (struct udev *)u;
}

struct udev *udev_ref(struct udev *u)
{
    if (u) ((struct ku_udev *)u)->refcount++;
    return u;
}

struct udev *udev_unref(struct udev *u)
{
    if (!u) return NULL;
    struct ku_udev *ku = (struct ku_udev *)u;
    if (--ku->refcount <= 0) free(ku);
    return NULL;
}

struct udev_enumerate *udev_enumerate_new(struct udev *u)
{
    if (!u) return NULL;
    struct ku_enum *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->refcount = 1;
    e->udev = (struct ku_udev *)udev_ref(u);
    return (struct udev_enumerate *)e;
}

struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *e)
{
    if (e) ((struct ku_enum *)e)->refcount++;
    return e;
}

struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *e)
{
    if (!e) return NULL;
    struct ku_enum *ke = (struct ku_enum *)e;
    if (--ke->refcount <= 0) {
        free(ke->match_subsystem);
        ku_list_free(ke->devices);
        udev_unref((struct udev *)ke->udev);
        free(ke);
    }
    return NULL;
}

int udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char *subsystem)
{
    if (!e || !subsystem) return -EINVAL;
    struct ku_enum *ke = (struct ku_enum *)e;
    free(ke->match_subsystem);
    ke->match_subsystem = strdup(subsystem);
    return 0;
}

/* /sys/bus/<sub>/devices walk. */
int udev_enumerate_scan_devices(struct udev_enumerate *e)
{
    if (!e) return -EINVAL;
    struct ku_enum *ke = (struct ku_enum *)e;
    const char *sub = ke->match_subsystem ? ke->match_subsystem : "usb";

    char dirpath[256];
    snprintf(dirpath, sizeof(dirpath), "/sys/bus/%s/devices", sub);

    INIT(opendir);
    DIR *d = r_opendir(dirpath);
    if (!d) return -errno;

    struct dirent *de;
    INIT(read);  /* not used, but ensure init */
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        /* Interfészek: 1-0:1.0 stb. Ezeket KIHAGYJUK — libudev is így csinálja
         * a usb subsystem enumeráció során (csak DEVICE-okat ad, nem interfész-eket). */
        if (strchr(de->d_name, ':')) continue;

        char syspath[512];
        snprintf(syspath, sizeof(syspath), "%s/%s", dirpath, de->d_name);
        ku_list_append(&ke->devices, syspath, NULL);
    }
    closedir(d);
    return 0;
}

struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *e)
{
    if (!e) return NULL;
    return (struct udev_list_entry *)((struct ku_enum *)e)->devices;
}

const char *udev_list_entry_get_name(struct udev_list_entry *le)
{
    if (!le) return NULL;
    return ((struct ku_list_entry *)le)->name;
}

const char *udev_list_entry_get_value(struct udev_list_entry *le)
{
    if (!le) return NULL;
    return ((struct ku_list_entry *)le)->value;
}

struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *le)
{
    if (!le) return NULL;
    return (struct udev_list_entry *)((struct ku_list_entry *)le)->next;
}

struct udev_list_entry *udev_list_entry_get_by_name(struct udev_list_entry *le, const char *name)
{
    while (le) {
        const char *n = udev_list_entry_get_name(le);
        if (n && strcmp(n, name) == 0) return le;
        le = udev_list_entry_get_next(le);
    }
    return NULL;
}

/* Parse a uevent file: KEY=VALUE pairs. */
static void ku_parse_uevent(struct ku_device *d, const char *syspath)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/uevent", syspath);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line, *value = eq + 1;
        ku_list_append(&d->properties, key, value);
        if (strcmp(key, "DEVNAME") == 0) {
            free(d->devnode);
            /* DEVNAME is relative; libudev returns it prefixed by /dev/ */
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "/dev/%s", value);
            d->devnode = strdup(tmp);
        } else if (strcmp(key, "DEVTYPE") == 0) {
            free(d->devtype);
            d->devtype = strdup(value);
        } else if (strcmp(key, "MAJOR") == 0) {
            d->devnum |= makedev(atoi(value), 0) & ~0xff;
        } else if (strcmp(key, "MINOR") == 0) {
            d->devnum |= atoi(value);
        }
    }
    fclose(f);
}

/* Read a single-line sysfs attribute (busnum, idVendor, ...). */
static char *ku_read_sysattr(const char *syspath, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", syspath, name);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return NULL; }
    fclose(f);
    size_t l = strlen(buf);
    while (l && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = 0;
    return strdup(buf);
}

struct udev_device *udev_device_new_from_syspath(struct udev *u, const char *syspath)
{
    if (!u || !syspath) return NULL;
    struct stat st;
    INIT(stat);
    if (r_stat(syspath, &st) < 0) return NULL;

    struct ku_device *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->refcount = 1;
    d->udev = (struct ku_udev *)udev_ref(u);
    d->syspath = strdup(syspath);
    d->initialized = 1;

    /* sysname = basename(syspath) */
    const char *base = strrchr(syspath, '/');
    d->sysname = strdup(base ? base + 1 : syspath);

    /* subsystem from /sys/bus/<X>/devices path */
    if (strstr(syspath, "/sys/bus/usb/")) d->subsystem = strdup("usb");
    else if (strstr(syspath, "/sys/bus/hid/")) d->subsystem = strdup("hid");
    else d->subsystem = strdup("usb"); /* default for our enumerate-usb-only */

    ku_parse_uevent(d, syspath);
    return (struct udev_device *)d;
}

struct udev_device *udev_device_ref(struct udev_device *dev)
{
    if (dev) ((struct ku_device *)dev)->refcount++;
    return dev;
}

struct udev_device *udev_device_unref(struct udev_device *dev)
{
    if (!dev) return NULL;
    struct ku_device *d = (struct ku_device *)dev;
    if (--d->refcount <= 0) {
        free(d->syspath);
        free(d->subsystem);
        free(d->devnode);
        free(d->devtype);
        free(d->sysname);
        ku_list_free(d->properties);
        ku_list_free(d->sysattrs);
        udev_unref((struct udev *)d->udev);
        free(d);
    }
    return NULL;
}

const char *udev_device_get_syspath(struct udev_device *dev)
{
    return dev ? ((struct ku_device *)dev)->syspath : NULL;
}

const char *udev_device_get_subsystem(struct udev_device *dev)
{
    return dev ? ((struct ku_device *)dev)->subsystem : NULL;
}

const char *udev_device_get_sysname(struct udev_device *dev)
{
    return dev ? ((struct ku_device *)dev)->sysname : NULL;
}

const char *udev_device_get_devnode(struct udev_device *dev)
{
    return dev ? ((struct ku_device *)dev)->devnode : NULL;
}

const char *udev_device_get_devtype(struct udev_device *dev)
{
    return dev ? ((struct ku_device *)dev)->devtype : NULL;
}

dev_t udev_device_get_devnum(struct udev_device *dev)
{
    return dev ? ((struct ku_device *)dev)->devnum : 0;
}

int udev_device_get_is_initialized(struct udev_device *dev)
{
    return dev ? ((struct ku_device *)dev)->initialized : 0;
}

const char *udev_device_get_action(struct udev_device *dev)
{
    return "add";  /* enumeráció kontextusban tradicionálisan "add" */
}

const char *udev_device_get_sysattr_value(struct udev_device *dev, const char *attr)
{
    if (!dev || !attr) return NULL;
    struct ku_device *d = (struct ku_device *)dev;
    /* Check cache */
    struct ku_list_entry *e = d->sysattrs;
    while (e) {
        if (e->name && strcmp(e->name, attr) == 0) return e->value;
        e = e->next;
    }
    /* Read and cache */
    char *val = ku_read_sysattr(d->syspath, attr);
    if (!val) return NULL;
    ku_list_append(&d->sysattrs, attr, val);
    free(val);
    /* Find again in cache (just appended) */
    e = d->sysattrs;
    while (e) {
        if (e->name && strcmp(e->name, attr) == 0) return e->value;
        e = e->next;
    }
    return NULL;
}

const char *udev_device_get_property_value(struct udev_device *dev, const char *key)
{
    if (!dev || !key) return NULL;
    struct ku_list_entry *e = ((struct ku_device *)dev)->properties;
    while (e) {
        if (e->name && strcmp(e->name, key) == 0) return e->value;
        e = e->next;
    }
    return NULL;
}

struct udev_list_entry *udev_device_get_properties_list_entry(struct udev_device *dev)
{
    return dev ? (struct udev_list_entry *)((struct ku_device *)dev)->properties : NULL;
}

/* Hierarchical parent — for libusb we just return NULL (no parent traversal). */
struct udev_device *udev_device_get_parent(struct udev_device *dev) { return NULL; }
struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *dev, const char *s, const char *t) { return NULL; }

/* Monitor — fake (we don't deliver events). Returns same socketpair fd as we
 * already do for AF_NETLINK in the socket() override above. */
struct udev_monitor;
struct udev_monitor *udev_monitor_new_from_netlink(struct udev *u, const char *source)
{
    /* Allocate a dummy handle. The libusb thread polls fd from get_fd. */
    struct ku_udev *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->refcount = 1;
    return (struct udev_monitor *)m;
}
struct udev_monitor *udev_monitor_ref(struct udev_monitor *m) { if (m) ((struct ku_udev*)m)->refcount++; return m; }
struct udev_monitor *udev_monitor_unref(struct udev_monitor *m) {
    if (!m) return NULL;
    struct ku_udev *km = (struct ku_udev *)m;
    if (--km->refcount <= 0) free(km);
    return NULL;
}
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *m, const char *s, const char *t) { return 0; }
int udev_monitor_enable_receiving(struct udev_monitor *m) { return 0; }
int udev_monitor_get_fd(struct udev_monitor *m)
{
    /* Return a never-ready fd: socketpair-szal egy peer, ami sosem küld adatot. */
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) < 0) return -1;
    return sp[0];
}
struct udev_device *udev_monitor_receive_device(struct udev_monitor *m) { return NULL; /* never */ }

/* ────────────────────────────────────────────────────────────────────
 *  udev_hwdb — hardware-database API for vendor/product name lookup.
 *
 *  A modern usbutils (lsusb v015+) a libudev_hwdb-t használja a VID:PID
 *  → "Linux Foundation root hub" típusú név-feloldáshoz. A hwdb adatbázis
 *  a /etc/udev/hwdb.bin fájl — ez normál Linux-on a `systemd-hwdb update`
 *  generálja a /usr/share/hwdata/usb.ids-ből.
 *
 *  Mi nem generálunk hwdb.bin-t (komplex bináris formátum). Helyette:
 *  - udev_hwdb_new visszaad egy dummy handle-t (siker)
 *  - get_properties_list_entry NULL-t ad — lsusb fallback-el a device
 *    saját USB-descriptor 'product'/'manufacturer' string-jeire
 *
 *  Eredmény: NINCS 'unable to initialize usb spec' warning, és a
 *  lsusb a device-saját product-string-jeit használja
 *  (pl. 'Linux 6.12.0-kaliterm+ vhci_hcd ...').
 * ──────────────────────────────────────────────────────────────────── */
struct udev_hwdb;

struct udev_hwdb *udev_hwdb_new(struct udev *u)
{
    if (!u) return NULL;
    struct ku_udev *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->refcount = 1;
    return (struct udev_hwdb *)h;
}

struct udev_hwdb *udev_hwdb_ref(struct udev_hwdb *h)
{
    if (h) ((struct ku_udev *)h)->refcount++;
    return h;
}

struct udev_hwdb *udev_hwdb_unref(struct udev_hwdb *h)
{
    if (!h) return NULL;
    struct ku_udev *kh = (struct ku_udev *)h;
    if (--kh->refcount <= 0) free(kh);
    return NULL;
}

/* hwdb lookup egy modalias (pl. "usb:v1D6Bp0002") alapján — listaként
 * (KEY/VALUE párok) adná a tulajdonságokat. Mi NULL-t (üres) adunk:
 * a libudev kliens (lsusb) fallback-el a device-natív stringekre. */
struct udev_list_entry *udev_hwdb_get_properties_list_entry(
    struct udev_hwdb *h, const char *modalias, unsigned int flags)
{
    (void)h; (void)modalias; (void)flags;
    return NULL;
}
