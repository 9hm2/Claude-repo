/*
 * LKL (Linux Kernel Library) runtime hookok.
 *
 * Az LKL-t Bionic-ABI .so formában az APK jniLibs/arm64-v8a/-ja viszi
 * (kali-term-release.yml CI lánc állítja össze). A Kotlin oldal a kaliterm
 * objektum init-jében:
 *
 *     System.loadLibrary("lkl")           // → liblkl.so
 *     System.loadLibrary("kaliterm_native") // → libkaliterm_native.so
 *
 * Android-on a System.loadLibrary RTLD_LOCAL-lal tölt, ezért
 * `dlsym(RTLD_DEFAULT, ...)` nem feltétlenül látja a szimbólumokat. Itt
 * explicit dlopen("liblkl.so", RTLD_NOW | RTLD_GLOBAL)-tel kérünk handle-t
 * (`dlopen` ugyanazt a betöltött példányt adja vissza, és a RTLD_GLOBAL
 * promotálja a globális namespace-be), majd `dlsym(handle, ...)`-ben
 * keressük a szimbólumokat. Ez kétségtelenül megtalálja, ha exportálva
 * vannak.
 *
 * Az LKL API:
 *   int lkl_init(struct lkl_host_operations *ops);   // host-ops regisztráció
 *   int lkl_start_kernel(const char *cmd_line, ...); // varargs printf-stílus
 *   int lkl_sys_halt(void);
 *   long lkl_sys_open(...), lkl_sys_read(...), ...
 *   struct lkl_host_operations lkl_host_ops;         // posix-host.c-ben def.
 */
#define _GNU_SOURCE
#include <jni.h>
#include <android/log.h>

#include <arpa/inet.h>   /* htonl, ntohl — USBIP wire-format byte order */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <pty.h>         /* forkpty — :lkl-process shell-host */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>

#define LOG_TAG "kaliterm-lkl"
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)

typedef int  (*fn_lkl_init)(void *ops);
typedef int  (*fn_lkl_start_kernel)(const char *cmd_line, ...);
typedef void (*fn_lkl_cleanup)(void);
typedef long (*fn_lkl_sys_halt)(void);
typedef long (*fn_lkl_syscall)(long no, long *params);  /* generic dispatcher */

/* Linux "generic" syscall ABI — same constants for aarch64, riscv64, lkl,
 * defined in include/uapi/asm-generic/unistd.h. We hard-code instead of
 * pulling lkl_autoconf.h, because those numbers are stable across versions. */
#define LKL_NR_mkdirat       34
#define LKL_NR_mount         40
#define LKL_NR_openat        56
#define LKL_NR_close         57
#define LKL_NR_read          63
#define LKL_NR_write         64
#define LKL_NR_socket       198
#define LKL_NR_socketpair   199
#define LKL_NR_getdents64    61

#define LKL_AT_FDCWD         (-100)
#define LKL_O_RDONLY         0
#define LKL_O_WRONLY         1
#define LKL_O_NONBLOCK       04000
#define LKL_EBUSY            16
#define LKL_ENOENT           2
#define LKL_EAGAIN           11

#define LKL_AF_UNIX           1
#define LKL_AF_INET           2
#define LKL_SOCK_STREAM       1

static struct {
    pthread_mutex_t lock;
    int             resolved;        /* dlsym próbálta-e már */
    int             available;       /* lkl_init + start_kernel + host_ops megvolt-e */
    int             running;         /* lkl_init + lkl_start_kernel sikerült-e */
    int             terminated;      /* halt+cleanup után — re-start a kernelben
                                        upstream NEM támogatott, app-restart kell */
    void           *dl_handle;       /* dlopen("liblkl.so") visszaértéke */
    fn_lkl_init         init_fn;
    fn_lkl_start_kernel start_kernel;
    fn_lkl_cleanup      cleanup_fn;
    fn_lkl_sys_halt     sys_halt;
    fn_lkl_syscall      syscall_fn;
    void               *host_ops;    /* lkl_host_ops szimbólum (struct címe) */
    char                status_buf[1024];
} g_lkl = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void *resolve(const char *name)
{
    void *p = dlsym(g_lkl.dl_handle, name);
    if (!p) {
        /* fallback: RTLD_DEFAULT (ha a runtime-loader esetleg másképp
         * látja, ez talál) */
        p = dlsym(RTLD_DEFAULT, name);
    }
    return p;
}

/* dlsym minden szimbólumra; idempotens. */
static void lkl_resolve_locked(void)
{
    if (g_lkl.resolved) return;
    g_lkl.resolved = 1;

    /* Próbáljuk meg dlopen-nel — ez visszaadja a System.loadLibrary által
     * már betöltött példányt, és RTLD_GLOBAL-lal globalizálja. */
    if (!g_lkl.dl_handle) {
        g_lkl.dl_handle = dlopen("liblkl.so", RTLD_NOW | RTLD_GLOBAL);
        if (!g_lkl.dl_handle) {
            /* Nem található vagy nem tölthető — pl. nincs az APK-ban. */
            const char *err = dlerror();
            snprintf(g_lkl.status_buf, sizeof(g_lkl.status_buf),
                     "UNAVAILABLE — dlopen(\"liblkl.so\") sikertelen:\n"
                     "  %s\n"
                     "Ok: a libfile nincs az APK jniLibs/arm64-v8a/-ja alatt.\n"
                     "Megoldás: futtasd a kali-term-release CI workflow-t úgy\n"
                     "hogy a `kali-term-release` job-lánca (LKL build → APK)\n"
                     "sikeresen lefusson.",
                     err ? err : "(nincs dlerror)");
            LOGW("dlopen liblkl.so: %s", err ? err : "(nincs dlerror)");
            return;
        }
        LOGI("dlopen liblkl.so OK, handle=%p", g_lkl.dl_handle);
    }

    g_lkl.init_fn      = (fn_lkl_init)        resolve("lkl_init");
    g_lkl.start_kernel = (fn_lkl_start_kernel)resolve("lkl_start_kernel");
    g_lkl.cleanup_fn   = (fn_lkl_cleanup)     resolve("lkl_cleanup");
    g_lkl.sys_halt     = (fn_lkl_sys_halt)    resolve("lkl_sys_halt");
    g_lkl.syscall_fn   = (fn_lkl_syscall)     resolve("lkl_syscall");
    g_lkl.host_ops     =                       resolve("lkl_host_ops");

    /* available = a minimum bekapcsoláshoz szükséges szimbólumok megvannak */
    g_lkl.available = (g_lkl.init_fn      != NULL &&
                       g_lkl.start_kernel != NULL &&
                       g_lkl.host_ops     != NULL);

    /* Részletes diagnosztika MINDIG (jó/rossz esetre is), per-szimbólum-sor.
     * Megjegyzés: `lkl_sys_open` és `lkl_sys_read` szándékosan NINCS itt —
     * azok a `tools/lkl/include/lkl.h` static inline wrapperjei, amelyek
     * compilation-unit szintjén inline-olódnak a `lkl_syscall()` köré, és
     * SOSEM kerülnek a .so szimbólumtáblájába. A valódi belépési pont a
     * generic `lkl_syscall(no, params)` dispatcher. */
    snprintf(g_lkl.status_buf, sizeof(g_lkl.status_buf),
             "%s — szimbólumok (dlsym):\n"
             "  lkl_init         = %p%s\n"
             "  lkl_start_kernel = %p%s\n"
             "  lkl_host_ops     = %p%s\n"
             "  lkl_sys_halt     = %p%s\n"
             "  lkl_cleanup      = %p%s\n"
             "  lkl_syscall      = %p%s\n"
             "  dl_handle        = %p",
             g_lkl.available ? "AVAILABLE" : "UNAVAILABLE",
             (void*)g_lkl.init_fn,      g_lkl.init_fn      ? "" : "  ← MISSING",
             (void*)g_lkl.start_kernel, g_lkl.start_kernel ? "" : "  ← MISSING",
             g_lkl.host_ops,            g_lkl.host_ops     ? "" : "  ← MISSING",
             (void*)g_lkl.sys_halt,     g_lkl.sys_halt     ? "" : "  (opcionális)",
             (void*)g_lkl.cleanup_fn,   g_lkl.cleanup_fn   ? "" : "  (opcionális)",
             (void*)g_lkl.syscall_fn,   g_lkl.syscall_fn   ? "" : "  (opcionális)",
             g_lkl.dl_handle);

    LOGI("LKL resolve: available=%d", g_lkl.available);
}

/* ── LKL syscall helperek ───────────────────────────────────────────────
 * Az lkl_syscall(no, params) az LKL host-side ABI generikus belépési
 * pontja. A params egy 6-elemű long-tömb, amibe sorrendben pakoljuk
 * a syscall argumentumait (long-ra cast-olva). Lépéseket helper-rel
 * tisztábban olvashatóvá tesszük.
 *
 * Megjegyzés: pointer-argumentumokat (long)(intptr_t) cast-tal adunk
 * át. Az LKL userspace-be nem ír át pointer-translation-t — a process
 * VM-jét közvetlenül használja, így a sima C string-pointer működik.
 */

static long lkl_call(long nr, long a, long b, long c, long d, long e)
{
    long p[6] = { a, b, c, d, e, 0 };
    return g_lkl.syscall_fn(nr, p);
}

static long lkl_mkdir(const char *path, long mode)
{
    return lkl_call(LKL_NR_mkdirat, LKL_AT_FDCWD,
                    (long)(intptr_t)path, mode, 0, 0);
}
static long lkl_mount(const char *source, const char *target,
                      const char *fstype, long flags, const char *data)
{
    return lkl_call(LKL_NR_mount,
                    (long)(intptr_t)source, (long)(intptr_t)target,
                    (long)(intptr_t)fstype, flags, (long)(intptr_t)data);
}
static long lkl_open(const char *path, long flags)
{
    return lkl_call(LKL_NR_openat, LKL_AT_FDCWD,
                    (long)(intptr_t)path, flags, 0, 0);
}
static long lkl_read(long fd, void *buf, unsigned long count)
{
    return lkl_call(LKL_NR_read, fd, (long)(intptr_t)buf, (long)count, 0, 0);
}
static long lkl_close(long fd)
{
    return lkl_call(LKL_NR_close, fd, 0, 0, 0, 0);
}

/* Egyszeri mount-elés idempotens módon. */
static long lkl_mount_once(const char *source, const char *target,
                           const char *fstype)
{
    lkl_mkdir(target, 0755);   /* best-effort; -EEXIST is OK */
    long m = lkl_mount(source, target, fstype, 0, NULL);
    if (m == 0 || m == -LKL_EBUSY) return 0;
    return m;
}

/* Beolvas egy fájlt a LKL kernel fájlrendszeréből egy felhasználói pufferbe.
 * `out_len` a hasznos bájtok száma a NUL terminálás nélkül. */
static long lkl_read_file(const char *path, char *out, size_t out_sz, size_t *out_len)
{
    long fd = lkl_open(path, LKL_O_RDONLY);
    if (fd < 0) return fd;
    long n = lkl_read(fd, out, out_sz - 1);
    lkl_close(fd);
    if (n < 0) return n;
    out[n] = '\0';
    /* trailing newline-eket levágunk az olvasható kiíráshoz */
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' ')) {
        out[--n] = '\0';
    }
    if (out_len) *out_len = (size_t)n;
    return n;
}

/* Linux dirent64 — minden arch-on ugyanaz a layout (asm-generic). */
struct lkl_linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

/* Egy LKL-belső könyvtár tartalmának listázása getdents64-gyel a megadott
 * bufferbe. NEM rekurzív, csak az első szintű entry-ket írja ki — egy soros
 * formátumban, "." és ".." nélkül. */
static void lkl_list_dir_into(char **pp, char *end, const char *path)
{
    char *p = *pp;
    #define A(...) do { if (p < end) p += snprintf(p, end - p, __VA_ARGS__); } while(0)
    long fd = lkl_open(path, LKL_O_RDONLY);
    if (fd < 0) {
        A("  (open %s rc=%ld)\n", path, fd);
        *pp = p;
        return;
    }
    char dirbuf[2048];
    int empty = 1;
    int loops = 0;
    /* getdents64 visszahívható amíg nem 0 — egy 4K-s könyvtárban ez 1-2 hívás. */
    while (loops++ < 8) {
        long bytes = lkl_call(LKL_NR_getdents64, fd,
                              (long)(intptr_t)dirbuf, (long)sizeof(dirbuf), 0, 0);
        if (bytes < 0) {
            A("  (getdents64 rc=%ld)\n", bytes);
            break;
        }
        if (bytes == 0) break;
        long off = 0;
        while (off < bytes) {
            struct lkl_linux_dirent64 *d =
                (struct lkl_linux_dirent64 *)(dirbuf + off);
            const char *name = d->d_name;
            /* "." és ".." kihagyása. */
            if (!(name[0] == '.' && (name[1] == '\0' ||
                  (name[1] == '.' && name[2] == '\0')))) {
                A("  %s\n", name);
                empty = 0;
            }
            off += d->d_reclen;
            if (d->d_reclen == 0) break;  /* safety */
        }
    }
    if (empty) A("  (üres)\n");
    lkl_close(fd);
    #undef A
    *pp = p;
}

/* Mint a fenti, de csak azokat az entryket írja ki, amelyek valamelyik
 * `prefixes[]` (NULL-terminated) prefix-szel kezdődnek. A többit megszámolja
 * és összesítve mutatja, hogy a háttér-zaj (pl. legacy BSD pty-k) ne öntse
 * el a kimenetet. */
static void lkl_list_dir_filtered(char **pp, char *end, const char *path,
                                   const char *const prefixes[])
{
    char *p = *pp;
    #define A(...) do { if (p < end) p += snprintf(p, end - p, __VA_ARGS__); } while(0)
    long fd = lkl_open(path, LKL_O_RDONLY);
    if (fd < 0) { A("  (open %s rc=%ld)\n", path, fd); *pp = p; return; }
    char dirbuf[2048];
    int matched = 0, skipped = 0, loops = 0;
    while (loops++ < 16) {
        long bytes = lkl_call(LKL_NR_getdents64, fd,
                              (long)(intptr_t)dirbuf, (long)sizeof(dirbuf), 0, 0);
        if (bytes <= 0) break;
        long off = 0;
        while (off < bytes) {
            struct lkl_linux_dirent64 *d =
                (struct lkl_linux_dirent64 *)(dirbuf + off);
            const char *name = d->d_name;
            if (!(name[0] == '.' && (name[1] == '\0' ||
                  (name[1] == '.' && name[2] == '\0')))) {
                int hit = 0;
                for (const char *const *q = prefixes; *q; q++) {
                    size_t plen = strlen(*q);
                    if (strncmp(name, *q, plen) == 0) { hit = 1; break; }
                }
                if (hit) { A("  %s\n", name); matched++; }
                else     { skipped++; }
            }
            off += d->d_reclen;
            if (d->d_reclen == 0) break;
        }
    }
    if (matched == 0) A("  (egyetlen érdekes entry sem)\n");
    if (skipped > 0)  A("  (+ %d további — pl. legacy pty)\n", skipped);
    lkl_close(fd);
    #undef A
    *pp = p;
}

/* URB-bridge globális állapot: pillanatnyilag csak egy eszközt támogatunk.
 *
 * Forward-declarjuk itt (a probe_kernel_into ezt használja a Frissít-en
 * megjelenő státusz-sorhoz). A struct teljes definícióját a Phase 2c.5d
 * URB-dispatch blokkban tartjuk lentebb — ez a deklaráció csak a state
 * tárolásához kell. */
static struct {
    pthread_mutex_t lock;
    int             active;        /* 1 = worker fut */
    pthread_t       thread;
    libusb_context       *ctx;
    libusb_device_handle *handle;
    int             dup_fd;        /* a libusb_wrap_sys_device-nak adott fd */
    int             sv_kern;       /* sv[0] — a vhci_hcd kernel-threadé */
    int             sv_user;       /* sv[1] — a saját oldalunk (LKL-fd) */
    uint32_t        devid;
    uint32_t        n_urbs;        /* feldolgozott URB-ek (diag) */
    uint32_t        n_errors;
    /* Endpoint type cache: index = (addr & 0x0F) | ((addr & 0x80) >> 3),
     * 0..31. Érték: LIBUSB_TRANSFER_TYPE_* (0=control, 1=iso, 2=bulk, 3=int). */
    uint8_t         ep_type[32];
} g_bridge = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* A futó LKL kernel "életjeleinek" összeszedése: /proc/version,
 * /sys/bus/usb/devices tartalom, vhci_hcd port-státusz. Minden press-elt
 * Frissít-en visszacsekkolódik a kernel-állapot, és a counter változik —
 * így a UI vizuálisan is mutatja hogy az probe lefutott. */
static unsigned g_probe_counter = 0;

static void probe_kernel_into(char *buf, size_t bufsz)
{
    char *p = buf;
    char *end = buf + bufsz;
    #define APPEND(...) do { if (p < end) p += snprintf(p, end - p, __VA_ARGS__); } while(0)

    if (!g_lkl.syscall_fn) {
        APPEND("lkl_syscall not resolved");
        return;
    }

    g_probe_counter++;
    APPEND("probe #%u\n", g_probe_counter);

    /* URB bridge állapot (Phase 2c.5d). */
    pthread_mutex_lock(&g_bridge.lock);
    if (g_bridge.active) {
        APPEND("URB bridge: AKTÍV — devid=%08x  URBs=%u  errs=%u\n",
               g_bridge.devid, g_bridge.n_urbs, g_bridge.n_errors);
    } else {
        APPEND("URB bridge: leállt (nincs aktív device)\n");
    }
    pthread_mutex_unlock(&g_bridge.lock);

    /* /proc mount (idempotens). */
    long m = lkl_mount_once("proc", "/proc", "proc");
    if (m < 0) {
        APPEND("mount(proc): rc=%ld\n", m);
    }
    /* /sys mount (idempotens). */
    long s = lkl_mount_once("sysfs", "/sys", "sysfs");
    if (s < 0) {
        APPEND("mount(sysfs): rc=%ld\n", s);
    }

    /* /proc/version */
    char tmp[512];
    size_t tlen = 0;
    long n = lkl_read_file("/proc/version", tmp, sizeof(tmp), &tlen);
    if (n < 0) {
        APPEND("read(/proc/version): rc=%ld\n", n);
    } else {
        APPEND("/proc/version:\n  %s\n", tmp);
    }

    /* /sys/bus/usb/devices/ — könyvtár tartalom. Sikeres URB-dispatch
     * után itt megjelenik a vhci_hcd által enumerált eszköz (pl. "1-1"). */
    APPEND("/sys/bus/usb/devices/:\n");
    lkl_list_dir_into(&p, end, "/sys/bus/usb/devices");

    /* Minden 1-* device-interface (nem a root hub) `uevent` fájljából
     * kiszedjük a DRIVER=... sort — látszani fog hogy melyik mainline
     * driver kötődött be (ftdi_sio, ch341, usbhid, btusb, stb.). */
    APPEND("USB interface driver-bindok:\n");
    {
        long dfd = lkl_open("/sys/bus/usb/devices", LKL_O_RDONLY);
        if (dfd >= 0) {
            char dirbuf[2048];
            int any = 0;
            while (1) {
                long bytes = lkl_call(LKL_NR_getdents64, dfd,
                                      (long)(intptr_t)dirbuf, (long)sizeof(dirbuf), 0, 0);
                if (bytes <= 0) break;
                long off = 0;
                while (off < bytes) {
                    struct lkl_linux_dirent64 *de =
                        (struct lkl_linux_dirent64 *)(dirbuf + off);
                    /* csak az interface-ek (pl. "1-1:1.0"), ne a hub-ok */
                    if (strchr(de->d_name, ':')) {
                        char path[160], evt[768];
                        size_t el = 0;
                        snprintf(path, sizeof(path),
                                 "/sys/bus/usb/devices/%s/uevent", de->d_name);
                        long r = lkl_read_file(path, evt, sizeof(evt), &el);
                        const char *drv = NULL;
                        if (r > 0) {
                            const char *q = strstr(evt, "DRIVER=");
                            if (q) drv = q + 7;
                        }
                        if (drv) {
                            /* drv sor végén newline lehet */
                            const char *nl = strchr(drv, '\n');
                            int dlen = nl ? (int)(nl - drv) : (int)strlen(drv);
                            APPEND("  %s → %.*s\n", de->d_name, dlen, drv);
                        } else {
                            APPEND("  %s → (nincs driver bekötve)\n", de->d_name);
                        }
                        any = 1;
                    }
                    off += de->d_reclen;
                    if (de->d_reclen == 0) break;
                }
            }
            if (!any) APPEND("  (egy interface sem)\n");
            lkl_close(dfd);
        }
    }

    /* /sys/class/tty/ — ha ftdi_sio/ch341/cp210x/pl2303 bekötődött,
     * ttyUSB0, ttyUSB1, stb. jelennek meg itt. A legacy BSD pty-ket (ttya0,
     * ptyc1, stb. — több száz darab van belőlük) kiszűrjük; csak a valódi
     * érdekes serial node-okat mutatjuk. */
    {
        static const char *const tty_prefixes[] = {
            "ttyUSB",   /* USB serial (ftdi_sio, ch341, cp210x, pl2303) */
            "ttyACM",   /* USB CDC-ACM (modem-class) */
            "ttyS",     /* hagyományos serial port (8250, 16550 — LKL-ben nincs) */
            "console",  /* kernel console */
            NULL,
        };
        APPEND("/sys/class/tty/:\n");
        lkl_list_dir_filtered(&p, end, "/sys/class/tty", tty_prefixes);
    }

    /* /sys/class/hidraw/ — ha usbhid + hidraw bekötődött (HID device-okhoz). */
    {
        long fd = lkl_open("/sys/class/hidraw", LKL_O_RDONLY);
        if (fd >= 0) {
            APPEND("/sys/class/hidraw/:\n");
            lkl_close(fd);
            lkl_list_dir_into(&p, end, "/sys/class/hidraw");
        }
    }

    /* vhci_hcd port-státusz: a kernel-thread futása + port-állapot. */
    n = lkl_read_file("/sys/devices/platform/vhci_hcd.0/status",
                      tmp, sizeof(tmp), &tlen);
    if (n >= 0) {
        APPEND("vhci_hcd.0/status:\n");
        /* Több soros file; ne csonkoljuk, csak indentáljuk. */
        const char *s2 = tmp;
        while (*s2) {
            const char *nl = strchr(s2, '\n');
            int len = nl ? (int)(nl - s2) : (int)strlen(s2);
            APPEND("  %.*s\n", len, s2);
            if (!nl) break;
            s2 = nl + 1;
        }
    }
    #undef APPEND
}

/* ── Phase 2c.5d: host-oldali URB-dispatch (USB/IP protokoll) ──────────
 *
 * A vhci_hcd a kernelben USB/IP protokollt beszél a `sv[0]` socket-en;
 * mi `sv[1]`-en (LKL fd) keresztül olvasunk/írunk lkl_syscall-okkal.
 * Minden URB-kérés egy 48-byte header (`struct usbip_header`, mind
 * BIG-ENDIAN a wire-on) + opcionális adat:
 *
 *   OUT (host → device):  CMD_SUBMIT + payload_bytes
 *                          → libusb_*_transfer   → RET_SUBMIT
 *   IN  (device → host):  CMD_SUBMIT
 *                          → libusb_*_transfer  → RET_SUBMIT + payload_bytes
 *
 * Sync libusb-API-t használunk (libusb_control_transfer / libusb_bulk_transfer
 * / libusb_interrupt_transfer). Egyszerre egy URB van folyamatban — soros
 * feldolgozás. HID/serial/BT/low-bandwidth bulk működik; folyamatos
 * monitor-mode Wi-Fi-hez aszinkron submit kellene (későbbi fejlesztés).
 *
 * SET_ADDRESS fake-elés: az Android USB stack már enumerálta az eszközt és
 * címet adott neki. A vhci_hcd újra-enumerál és SET_ADDRESS-t küld — ezt
 * mi NEM továbbítjuk az eszköznek (különben Android oldal megzavarodik),
 * csak success-status-szal válaszolunk vissza. Ezt az stub_dev.c (usbip
 * server) is így csinálja.
 */

#define USBIP_CMD_SUBMIT   0x00000001u
#define USBIP_CMD_UNLINK   0x00000002u
#define USBIP_RET_SUBMIT   0x00000003u
#define USBIP_RET_UNLINK   0x00000004u

#define USBIP_DIR_OUT      0u
#define USBIP_DIR_IN       1u

/* 48-byte fix header. Minden multi-byte mező BIG-ENDIAN a sv[1] wire-on.
 * Layout pontos egyezésben a kernelbeli `struct usbip_header`-rel
 * (drivers/usb/usbip/usbip_common.h). */
#pragma pack(push, 1)
struct usbip_hdr {
    /* base — 20 byte */
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    /* union — 28 byte (header total = 48) */
    union {
        struct {
            uint32_t transfer_flags;
            int32_t  transfer_buffer_length;
            int32_t  start_frame;
            int32_t  number_of_packets;
            int32_t  interval;
            uint8_t  setup[8];   /* USB setup packet — little-endian per USB spec */
        } cmd_submit;
        struct {
            int32_t status;
            int32_t actual_length;
            int32_t start_frame;
            int32_t number_of_packets;
            int32_t error_count;
            uint8_t padding[8];
        } ret_submit;
        struct {
            uint32_t target_seqnum;
            uint8_t  padding[24];
        } cmd_unlink;
        struct {
            int32_t  status;
            uint8_t  padding[24];
        } ret_unlink;
    } u;
};
#pragma pack(pop)
_Static_assert(sizeof(struct usbip_hdr) == 48, "USB/IP header must be 48 bytes");

/* g_bridge (URB worker state) declaráció feljebb a fájlban, a
 * probe_kernel_into előtt — innentől használjuk a struct mezőit. */

static int ep_index(uint8_t addr)
{
    /* OUT (0x00..0x0F) → 0..15, IN (0x80..0x8F) → 16..31 */
    return (addr & 0x0F) | ((addr & 0x80) >> 3);
}

/* Felépíti az endpoint-típus táblát a device aktív config-descriptor-ából,
 * és claimol-ja az összes interface-t a libusb-handle-on. Bulk/interrupt
 * transferhez a libusb-nek claim_interface kell — control mindig megy. */
static void cache_endpoints(libusb_device_handle *h)
{
    memset(g_bridge.ep_type, LIBUSB_TRANSFER_TYPE_BULK, sizeof(g_bridge.ep_type));
    /* EP0 mindkét irányban control. */
    g_bridge.ep_type[ep_index(0x00)] = LIBUSB_TRANSFER_TYPE_CONTROL;
    g_bridge.ep_type[ep_index(0x80)] = LIBUSB_TRANSFER_TYPE_CONTROL;

    /* Best-effort: ha a kernel-driver lefogta volna az interface-eket, a
     * libusb maga detach-olja a claim előtt. Android stock kernelnél ezt
     * usbfs nem feltétlenül engedi — innen jöhet hiba; csak loggoljuk. */
    libusb_set_auto_detach_kernel_driver(h, 1);

    libusb_device *dev = libusb_get_device(h);
    struct libusb_config_descriptor *cfg = NULL;
    if (!dev || libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) return;

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        int crc = libusb_claim_interface(h, i);
        if (crc < 0) LOGW("claim_interface(%d): %s", i, libusb_strerror(crc));

        const struct libusb_interface *iface = &cfg->interface[i];
        for (int j = 0; j < iface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *intf = &iface->altsetting[j];
            for (int k = 0; k < intf->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor *ep = &intf->endpoint[k];
                int idx = ep_index(ep->bEndpointAddress);
                g_bridge.ep_type[idx] = ep->bmAttributes & 0x03;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

/* Pontosan n byte-ot olvas egy LKL fd-ről. partial-read short-pollon kívül
 * újrahív; 0 vagy negatív rc-re hibát ad vissza. */
static int lkl_read_exact(int fd, void *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        long r = lkl_call(LKL_NR_read, fd,
                          (long)(intptr_t)((char *)buf + off),
                          (long)(n - off), 0, 0);
        if (r == 0) return -1;          /* EOF — kernel zárta sv[0]-t */
        if (r < 0) return (int)r;
        off += (size_t)r;
    }
    return 0;
}

static int lkl_write_exact(int fd, const void *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        long r = lkl_call(LKL_NR_write, fd,
                          (long)(intptr_t)((const char *)buf + off),
                          (long)(n - off), 0, 0);
        if (r <= 0) return (int)(r ? r : -1);
        off += (size_t)r;
    }
    return 0;
}

/* USB/IP header byte-swap (network → host) — a base + a parancstól függő
 * union mezők. setup[8] változatlan (USB spec szerint little-endian). */
static void hdr_ntoh(struct usbip_hdr *h)
{
    h->command   = ntohl(h->command);
    h->seqnum    = ntohl(h->seqnum);
    h->devid     = ntohl(h->devid);
    h->direction = ntohl(h->direction);
    h->ep        = ntohl(h->ep);
    if (h->command == USBIP_CMD_SUBMIT) {
        h->u.cmd_submit.transfer_flags         = ntohl(h->u.cmd_submit.transfer_flags);
        h->u.cmd_submit.transfer_buffer_length = (int32_t)ntohl((uint32_t)h->u.cmd_submit.transfer_buffer_length);
        h->u.cmd_submit.start_frame            = (int32_t)ntohl((uint32_t)h->u.cmd_submit.start_frame);
        h->u.cmd_submit.number_of_packets      = (int32_t)ntohl((uint32_t)h->u.cmd_submit.number_of_packets);
        h->u.cmd_submit.interval               = (int32_t)ntohl((uint32_t)h->u.cmd_submit.interval);
    } else if (h->command == USBIP_CMD_UNLINK) {
        h->u.cmd_unlink.target_seqnum = ntohl(h->u.cmd_unlink.target_seqnum);
    }
}

static void build_ret_submit(struct usbip_hdr *out, const struct usbip_hdr *cmd,
                             int status, int actual_length)
{
    memset(out, 0, sizeof(*out));
    out->command   = htonl(USBIP_RET_SUBMIT);
    out->seqnum    = htonl(cmd->seqnum);
    out->devid     = htonl(cmd->devid);
    out->direction = htonl(cmd->direction);
    out->ep        = htonl(cmd->ep);
    out->u.ret_submit.status        = (int32_t)htonl((uint32_t)status);
    out->u.ret_submit.actual_length = (int32_t)htonl((uint32_t)actual_length);
}

static void build_ret_unlink(struct usbip_hdr *out, const struct usbip_hdr *cmd,
                             int status)
{
    memset(out, 0, sizeof(*out));
    out->command   = htonl(USBIP_RET_UNLINK);
    out->seqnum    = htonl(cmd->seqnum);
    out->devid     = htonl(cmd->devid);
    out->direction = htonl(cmd->direction);
    out->ep        = htonl(cmd->ep);
    out->u.ret_unlink.status = (int32_t)htonl((uint32_t)status);
}

/* libusb hiba → Linux errno (negatív). A vhci_hcd ezt az URB->status-ba teszi. */
static int libusb_err_to_errno(int rc)
{
    switch (rc) {
        case LIBUSB_SUCCESS:             return 0;
        case LIBUSB_ERROR_TIMEOUT:       return -110; /* -ETIMEDOUT */
        case LIBUSB_ERROR_PIPE:          return -32;  /* -EPIPE (STALL) */
        case LIBUSB_ERROR_NO_DEVICE:     return -19;  /* -ENODEV */
        case LIBUSB_ERROR_OVERFLOW:      return -75;  /* -EOVERFLOW */
        case LIBUSB_ERROR_INTERRUPTED:   return -4;   /* -EINTR */
        case LIBUSB_ERROR_NO_MEM:        return -12;  /* -ENOMEM */
        case LIBUSB_ERROR_ACCESS:        return -13;  /* -EACCES */
        case LIBUSB_ERROR_NOT_FOUND:     return -2;   /* -ENOENT */
        case LIBUSB_ERROR_BUSY:          return -16;  /* -EBUSY */
        case LIBUSB_ERROR_IO:            return -5;   /* -EIO */
        default:                         return -71;  /* -EPROTO */
    }
}

/* Egy USB/IP URB feldolgozása. Visszaad: 0 = OK, <0 = fatal (worker exit). */
static int dispatch_one_urb(struct usbip_hdr *cmd, libusb_device_handle *h)
{
    const int is_out = (cmd->direction == USBIP_DIR_OUT);
    const uint8_t ep_addr = (cmd->ep & 0x0F) | (is_out ? 0 : 0x80);
    const int idx = ep_index(ep_addr);
    const int xfer_type = g_bridge.ep_type[idx];
    const int xfer_len = cmd->u.cmd_submit.transfer_buffer_length;

    /* Sanity — max 1 MB egy URB, hogy egy hibás kérés ne raballion memóriát. */
    if (xfer_len < 0 || xfer_len > (1 << 20)) return -1;

    uint8_t *buf = NULL;
    if (xfer_len > 0) {
        buf = (uint8_t *)malloc((size_t)xfer_len);
        if (!buf) return -1;
        if (is_out) {
            if (lkl_read_exact(g_bridge.sv_user, buf, (size_t)xfer_len) < 0) {
                free(buf); return -1;
            }
        }
    }

    int status = 0, actual = 0, rc;

    if (xfer_type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        const uint8_t *s = cmd->u.cmd_submit.setup;
        uint8_t  bmRequestType = s[0];
        uint8_t  bRequest      = s[1];
        uint16_t wValue        = (uint16_t)(s[2] | ((uint16_t)s[3] << 8));
        uint16_t wIndex        = (uint16_t)(s[4] | ((uint16_t)s[5] << 8));
        uint16_t wLength       = (uint16_t)(s[6] | ((uint16_t)s[7] << 8));

        /* SET_ADDRESS és SET_CONFIGURATION fake-elés — Android USB stack már
         * enumerálta és konfigurálta a device-t. A vhci_hcd újra-enumeráláskor
         * mindkettőt elküldi; ha mi valóban továbbítanánk, az Android-oldali
         * state megszakadna (vagy a libusb -EBUSY-val visszadobná, mert az
         * interface-eket már claim-eltük). bRequest=5 = SET_ADDRESS,
         * bRequest=9 = SET_CONFIGURATION; mindkettő bmRequestType=0x00 (std,
         * host→dev, recipient=device). Success no-op a sztub-szerű hozzáállás
         * — pont ezt csinálja a stub_dev.c is a usbip-szerveren. */
        if (bmRequestType == 0x00 && (bRequest == 0x05 || bRequest == 0x09)) {
            status = 0; actual = 0;
        } else {
            rc = libusb_control_transfer(h, bmRequestType, bRequest,
                                          wValue, wIndex, buf, wLength, 5000);
            if (rc >= 0) { status = 0; actual = rc; }
            else         { status = libusb_err_to_errno(rc); actual = 0; }
        }
    } else if (xfer_type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
        rc = libusb_interrupt_transfer(h, ep_addr, buf, xfer_len, &actual, 5000);
        status = (rc == 0) ? 0 : libusb_err_to_errno(rc);
    } else {
        /* BULK (és default fallback) */
        rc = libusb_bulk_transfer(h, ep_addr, buf, xfer_len, &actual, 5000);
        status = (rc == 0) ? 0 : libusb_err_to_errno(rc);
    }

    /* Válasz: RET_SUBMIT header + (IN esetén) actual byte adat. */
    struct usbip_hdr ret;
    build_ret_submit(&ret, cmd, status, actual);
    if (lkl_write_exact(g_bridge.sv_user, &ret, sizeof(ret)) < 0) {
        free(buf); return -1;
    }
    if (!is_out && actual > 0) {
        if (lkl_write_exact(g_bridge.sv_user, buf, (size_t)actual) < 0) {
            free(buf); return -1;
        }
    }
    free(buf);
    return 0;
}

static void *urb_worker(void *arg)
{
    (void)arg;
    int sv1 = g_bridge.sv_user;
    libusb_device_handle *h = g_bridge.handle;
    LOGI("URB worker started: sv_user=%d devid=%08x", sv1, g_bridge.devid);

    while (1) {
        struct usbip_hdr cmd;
        if (lkl_read_exact(sv1, &cmd, sizeof(cmd)) < 0) break;
        hdr_ntoh(&cmd);

        if (cmd.command == USBIP_CMD_SUBMIT) {
            if (dispatch_one_urb(&cmd, h) < 0) {
                g_bridge.n_errors++;
                break;
            }
            g_bridge.n_urbs++;
        } else if (cmd.command == USBIP_CMD_UNLINK) {
            /* Sync model: az URB már lefutott (vagy futtatás alatt blokkol);
             * sikeres unlink-et jelzünk vissza. A status -2/-ENOENT = "az
             * URB már nem található", ez a stub_dev semantika. */
            struct usbip_hdr ret;
            build_ret_unlink(&ret, &cmd, -2);
            if (lkl_write_exact(sv1, &ret, sizeof(ret)) < 0) break;
        } else {
            LOGW("URB worker: ismeretlen command=%u, exit", cmd.command);
            break;
        }
    }

    LOGI("URB worker exit: n_urbs=%u n_errors=%u", g_bridge.n_urbs, g_bridge.n_errors);

    /* Cleanup — a bridge thread tulajdona a libusb resources. */
    pthread_mutex_lock(&g_bridge.lock);
    if (g_bridge.handle) { libusb_close(g_bridge.handle); g_bridge.handle = NULL; }
    if (g_bridge.ctx)    { libusb_exit(g_bridge.ctx);     g_bridge.ctx    = NULL; }
    g_bridge.dup_fd  = -1;
    g_bridge.sv_user = -1;
    g_bridge.sv_kern = -1;
    g_bridge.active  = 0;
    pthread_mutex_unlock(&g_bridge.lock);
    return NULL;
}

/* Az attach végén meghívva: átveszi a libusb erőforrásokat és spawnol egy
 * detached worker thread-et. Sikerre 0-t ad, hiba esetén negatív értéket
 * (és a hívó NEM-transferred erőforrásokat ő zárja). */
static int start_urb_bridge(libusb_context *ctx, libusb_device_handle *h,
                            int dup_fd, int sv_kern, int sv_user, uint32_t devid)
{
    pthread_mutex_lock(&g_bridge.lock);
    if (g_bridge.active) {
        pthread_mutex_unlock(&g_bridge.lock);
        return -16; /* -EBUSY — már fut egy bridge */
    }
    g_bridge.ctx      = ctx;
    g_bridge.handle   = h;
    g_bridge.dup_fd   = dup_fd;
    g_bridge.sv_kern  = sv_kern;
    g_bridge.sv_user  = sv_user;
    g_bridge.devid    = devid;
    g_bridge.n_urbs   = 0;
    g_bridge.n_errors = 0;
    g_bridge.active   = 1;

    cache_endpoints(h);

    pthread_t tid;
    int prc = pthread_create(&tid, NULL, urb_worker, NULL);
    if (prc != 0) {
        g_bridge.active = 0;
        pthread_mutex_unlock(&g_bridge.lock);
        return -prc;
    }
    g_bridge.thread = tid;
    pthread_detach(tid);
    pthread_mutex_unlock(&g_bridge.lock);
    return 0;
}

/* ── JNI ─────────────────────────────────────────────────────────────── */

/* nativeLklKernelRelease — a futó LKL kernel `/proc/sys/kernel/osrelease`-jét
 * olvassa egy stringként. Üres stringgel tér vissza ha az LKL nem fut.
 * A proot launch.sh ezzel az értékkel hívja `--kernel-release`-t, így a
 * chrooted `uname -r` az LKL Linux-verzióját mutatja. */
JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklKernelRelease(JNIEnv *env, jobject thiz)
{
    char buf[128] = {0};
    pthread_mutex_lock(&g_lkl.lock);
    lkl_resolve_locked();
    if (g_lkl.running && g_lkl.syscall_fn) {
        size_t len = 0;
        long rc = lkl_read_file("/proc/sys/kernel/osrelease", buf, sizeof(buf) - 1, &len);
        if (rc < 0 || len == 0) {
            buf[0] = '\0';
        } else {
            buf[len] = '\0';
            /* trailing \n trimming */
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
                buf[--len] = '\0';
            }
        }
    }
    pthread_mutex_unlock(&g_lkl.lock);
    return (*env)->NewStringUTF(env, buf);
}

/* nativeLklListDir — egy LKL-belső könyvtár tartalmát adja vissza newline-
 * separated stringként ("." és ".." nélkül). Empty string ha a kernel nem fut
 * vagy a path nem létezik. A KaliShellService a /dev-listinghez használja, hogy
 * a chrooted /dev az LKL kernel device-fájljait tükrözze. */
JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklListDir(JNIEnv *env, jobject thiz, jstring jpath)
{
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return (*env)->NewStringUTF(env, "");

    char *buf = malloc(65536);
    if (!buf) {
        (*env)->ReleaseStringUTFChars(env, jpath, path);
        return (*env)->NewStringUTF(env, "");
    }
    buf[0] = '\0';

    pthread_mutex_lock(&g_lkl.lock);
    lkl_resolve_locked();
    if (g_lkl.running && g_lkl.syscall_fn) {
        long fd = lkl_open(path, LKL_O_RDONLY);
        if (fd >= 0) {
            char dirbuf[2048];
            char *out = buf;
            char *end = buf + 65535;
            int loops = 0;
            while (loops++ < 256) {
                long bytes = lkl_call(LKL_NR_getdents64, fd,
                                      (long)(intptr_t)dirbuf, (long)sizeof(dirbuf), 0, 0);
                if (bytes <= 0) break;
                long off = 0;
                while (off < bytes) {
                    struct lkl_linux_dirent64 *d =
                        (struct lkl_linux_dirent64 *)(dirbuf + off);
                    const char *name = d->d_name;
                    if (!(name[0] == '.' && (name[1] == '\0' ||
                          (name[1] == '.' && name[2] == '\0')))) {
                        size_t nlen = strlen(name);
                        if (out + nlen + 1 < end) {
                            memcpy(out, name, nlen);
                            out += nlen;
                            *out++ = '\n';
                        }
                    }
                    if (d->d_reclen == 0) { loops = 256; break; }
                    off += d->d_reclen;
                }
            }
            *out = '\0';
            lkl_close(fd);
        }
    }
    pthread_mutex_unlock(&g_lkl.lock);

    (*env)->ReleaseStringUTFChars(env, jpath, path);
    jstring result = (*env)->NewStringUTF(env, buf);
    free(buf);
    return result;
}

/* nativeLklReadFile — általános read-only fájl-olvasás az LKL fájlrendszeréből.
 * Empty stringgel tér vissza ha a kernel nem fut, vagy a fájl nem létezik.
 * Maximum 64KB-ig (Binder Parcel-friendly méret). */
JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklReadFile(JNIEnv *env, jobject thiz, jstring jpath)
{
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return (*env)->NewStringUTF(env, "");

    char *buf = malloc(65536);
    if (!buf) {
        (*env)->ReleaseStringUTFChars(env, jpath, path);
        return (*env)->NewStringUTF(env, "");
    }
    buf[0] = '\0';

    pthread_mutex_lock(&g_lkl.lock);
    lkl_resolve_locked();
    if (g_lkl.running && g_lkl.syscall_fn) {
        size_t len = 0;
        long rc = lkl_read_file(path, buf, 65535, &len);
        if (rc < 0 || len == 0) {
            buf[0] = '\0';
        } else {
            buf[len] = '\0';
        }
    }
    pthread_mutex_unlock(&g_lkl.lock);

    (*env)->ReleaseStringUTFChars(env, jpath, path);
    jstring result = (*env)->NewStringUTF(env, buf);
    free(buf);
    return result;
}

JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklStatus(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_lkl.lock);
    lkl_resolve_locked();
    const char *running_state =
        g_lkl.terminated ? "TERMINATED (app-restart kell)" :
        g_lkl.running    ? "YES"                            :
                           "no";

    char out[6144];
    int n = snprintf(out, sizeof(out), "%s\nrunning = %s",
                     g_lkl.status_buf, running_state);

    /* Ha fut a kernel, csatoljunk hozzá egy in-kernel életjel-probe-ot:
     * mount /proc, mount /sys, read /proc/version, /sys/bus/usb létezés.
     * Ezzel látszik a UI-ban, hogy a Linux kernel valóban fut és
     * filesystem-syscalls működnek a `lkl_syscall` ABI-n keresztül. */
    if (g_lkl.running && g_lkl.syscall_fn && n < (int)sizeof(out) - 256) {
        char probe[4096];
        probe_kernel_into(probe, sizeof(probe));
        snprintf(out + n, sizeof(out) - n, "\n\n── kernel probe ──\n%s", probe);
    }

    pthread_mutex_unlock(&g_lkl.lock);
    return (*env)->NewStringUTF(env, out);
}

/* ── LKL CONTROL SOCKET ──────────────────────────────────────────────
 *
 * A `:lkl` process-en egy unix-domain-socket szerver hallgat egy
 * konfigurálható path-on. A chrooted Kali bash-ban LD_PRELOAD-szal
 * betöltött `libkali_fuse_shim.so` connecte-el ide, és OPEN/READ/STAT/
 * LISTDIR/CLOSE szöveges parancsokkal kvázi-FUSE-fa-ként éri el az LKL
 * fájlrendszerét. Ezzel a /sys, /proc, /dev path-okra valódi LKL-fd-k
 * jönnek létre, és pl. a libusb USBDEVFS_* ioctl-jei is route-olhatók.
 *
 * Protokoll (text, newline-terminated):
 *   OPEN <path> <flags>        →  OK fd=<N>     | ERR errno=<N>
 *   READ <fd> <maxlen>         →  OK len=<N>\n<bytes>  | ERR errno=<N>
 *   CLOSE <fd>                 →  OK            | ERR errno=<N>
 *   STAT <path>                →  OK mode=<M> size=<S> ino=<I>  | ERR errno=<N>
 *   LISTDIR <path>             →  OK\n<name1>\n<name2>\n\n
 */
static struct {
    int       listen_fd;
    pthread_t thread;
    int       running;
    char      sock_path[256];
} g_ctrl = { -1, 0, 0, "" };

static void ctrl_handle_command(int conn, char *line)
{
    char op[16];
    /* op:1st token */
    char *sp = strchr(line, ' ');
    if (!sp) { write(conn, "ERR badcmd\n", 11); return; }
    *sp = 0;
    strncpy(op, line, sizeof(op) - 1);
    op[sizeof(op) - 1] = 0;
    char *rest = sp + 1;

    if (strcmp(op, "OPEN") == 0) {
        char *space2 = strchr(rest, ' ');
        if (!space2) { write(conn, "ERR badarg\n", 11); return; }
        *space2 = 0;
        const char *path = rest;
        int flags = atoi(space2 + 1);
        long fd = lkl_open(path, flags);
        char resp[64];
        int n = snprintf(resp, sizeof(resp), "OK fd=%ld\n", fd);
        if (fd < 0) n = snprintf(resp, sizeof(resp), "ERR errno=%ld\n", -fd);
        write(conn, resp, n);
    } else if (strcmp(op, "READ") == 0) {
        int fd; int maxlen;
        if (sscanf(rest, "%d %d", &fd, &maxlen) != 2) {
            write(conn, "ERR badarg\n", 11); return;
        }
        if (maxlen > 65536) maxlen = 65536;
        char *buf = malloc(maxlen);
        if (!buf) { write(conn, "ERR nomem\n", 10); return; }
        long n = lkl_read(fd, buf, maxlen);
        char hdr[64];
        if (n < 0) {
            int hn = snprintf(hdr, sizeof(hdr), "ERR errno=%ld\n", -n);
            write(conn, hdr, hn);
        } else {
            int hn = snprintf(hdr, sizeof(hdr), "OK len=%ld\n", n);
            write(conn, hdr, hn);
            if (n > 0) write(conn, buf, n);
        }
        free(buf);
    } else if (strcmp(op, "CLOSE") == 0) {
        int fd = atoi(rest);
        long n = lkl_close(fd);
        char resp[64];
        int rn = n == 0 ? snprintf(resp, sizeof(resp), "OK\n")
                        : snprintf(resp, sizeof(resp), "ERR errno=%ld\n", -n);
        write(conn, resp, rn);
    } else if (strcmp(op, "STAT") == 0) {
        const char *path = rest;
        /* Linux struct stat layout — ARM64 aarch64; az LKL és a Kali glibc
         * is ugyanazt használja. lkl_sys_newfstatat: dirfd=AT_FDCWD,
         * path, statbuf, flag=0. */
        /* Linux ARM64 struct stat layout — field-nevek renamelve, mert a
         * Bionic libc-header st_atime/mtime/ctime-t macro-ként define. */
        struct lkl_stat_buf {
            unsigned long  k_dev, k_ino;
            unsigned int   k_mode, k_nlink;
            unsigned int   k_uid, k_gid;
            unsigned long  k_rdev;
            unsigned long  __pad1;
            long           k_size;
            int            k_blksize, __pad2;
            long           k_blocks;
            long           k_atime, k_atime_nsec;
            long           k_mtime, k_mtime_nsec;
            long           k_ctime, k_ctime_nsec;
            int            __unused4, __unused5;
        } st;
        memset(&st, 0, sizeof(st));
        /* LKL_NR_newfstatat = 79 (ARM64) */
        long rc = lkl_call(79, LKL_AT_FDCWD,
                           (long)(intptr_t)path, (long)(intptr_t)&st, 0, 0);
        char resp[160];
        if (rc < 0) {
            int n = snprintf(resp, sizeof(resp), "ERR errno=%ld\n", -rc);
            write(conn, resp, n);
        } else {
            int n = snprintf(resp, sizeof(resp),
                "OK mode=%u size=%ld ino=%lu\n",
                (unsigned)st.k_mode,
                (long)st.k_size,
                (unsigned long)st.k_ino);
            write(conn, resp, n);
        }
    } else if (strcmp(op, "LISTDIR") == 0) {
        const char *path = rest;
        long fd = lkl_open(path, LKL_O_RDONLY);
        if (fd < 0) {
            char resp[64];
            int n = snprintf(resp, sizeof(resp), "ERR errno=%ld\n", -fd);
            write(conn, resp, n);
            return;
        }
        write(conn, "OK\n", 3);
        char dirbuf[2048];
        int loops = 0;
        while (loops++ < 64) {
            long bytes = lkl_call(LKL_NR_getdents64, fd,
                                  (long)(intptr_t)dirbuf, (long)sizeof(dirbuf), 0, 0);
            if (bytes <= 0) break;
            long off = 0;
            while (off < bytes) {
                struct lkl_linux_dirent64 *d =
                    (struct lkl_linux_dirent64 *)(dirbuf + off);
                const char *name = d->d_name;
                if (!(name[0] == '.' && (name[1] == '\0' ||
                      (name[1] == '.' && name[2] == '\0')))) {
                    write(conn, name, strlen(name));
                    write(conn, "\n", 1);
                }
                if (d->d_reclen == 0) { loops = 64; break; }
                off += d->d_reclen;
            }
        }
        lkl_close(fd);
        write(conn, "\n", 1);  /* empty line = end */
    } else {
        write(conn, "ERR unknown_op\n", 15);
    }
}

static void *ctrl_per_conn(void *arg)
{
    int conn = (int)(intptr_t)arg;
    char buf[2048];
    while (1) {
        ssize_t n = read(conn, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        char *line = buf;
        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = 0;
            if (*line) ctrl_handle_command(conn, line);
            line = nl + 1;
        }
    }
    close(conn);
    return NULL;
}

static void *ctrl_thread_fn(void *arg)
{
    (void)arg;
    while (g_ctrl.running) {
        int conn = accept(g_ctrl.listen_fd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* Per-conn detached thread — egyetlen lassú/dead client NEM blokkolja
         * a többi connect()-et. Detached, mert join-olást nem várunk. */
        pthread_t pt;
        if (pthread_create(&pt, NULL, ctrl_per_conn, (void *)(intptr_t)conn) == 0) {
            pthread_detach(pt);
        } else {
            close(conn);
        }
    }
    return NULL;
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklStartControlSocket(
    JNIEnv *env, jobject thiz, jstring jpath)
{
    if (g_ctrl.running) return 0;  /* idempotent */
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    snprintf(g_ctrl.sock_path, sizeof(g_ctrl.sock_path), "%s", path);
    (*env)->ReleaseStringUTFChars(env, jpath, path);

    g_ctrl.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ctrl.listen_fd < 0) {
        LOGE("ctrl-socket socket() failed: %s", strerror(errno));
        return -errno;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_ctrl.sock_path, sizeof(addr.sun_path) - 1);
    unlink(g_ctrl.sock_path);
    if (bind(g_ctrl.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("ctrl-socket bind(%s): %s", g_ctrl.sock_path, strerror(errno));
        close(g_ctrl.listen_fd);
        g_ctrl.listen_fd = -1;
        return -errno;
    }
    chmod(g_ctrl.sock_path, 0666);
    if (listen(g_ctrl.listen_fd, 5) < 0) {
        LOGE("ctrl-socket listen: %s", strerror(errno));
        close(g_ctrl.listen_fd);
        g_ctrl.listen_fd = -1;
        return -errno;
    }
    g_ctrl.running = 1;
    if (pthread_create(&g_ctrl.thread, NULL, ctrl_thread_fn, NULL) != 0) {
        LOGE("ctrl-socket pthread_create failed");
        g_ctrl.running = 0;
        close(g_ctrl.listen_fd);
        g_ctrl.listen_fd = -1;
        return -1;
    }
    LOGI("LKL control socket listening at %s", g_ctrl.sock_path);
    return 0;
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklStart(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_lkl.lock);
    lkl_resolve_locked();
    if (!g_lkl.available) {
        pthread_mutex_unlock(&g_lkl.lock);
        return -ENOENT;
    }
    if (g_lkl.running) {
        pthread_mutex_unlock(&g_lkl.lock);
        return -EALREADY;
    }
    if (g_lkl.terminated) {
        /* Az LKL kernel egyetlen processzben one-shot: a `lkl_sys_halt` +
         * `lkl_cleanup` után a globális kernel state irreverzibilisen
         * shutdown. Új lkl_init/lkl_start_kernel valószínűleg crash.
         * Megakadályozzuk a re-start próbát — a UI kéri az app-restart-ot. */
        pthread_mutex_unlock(&g_lkl.lock);
        LOGW("lkl re-start blokkolva: a kernel már termin­álva (app-restart kell)");
        return -EHOSTDOWN;
    }

    /* Új LKL API: lkl_init(host_ops) → lkl_start_kernel(cmd_line) */
    LOGI("lkl_init(host_ops=%p)", g_lkl.host_ops);
    int rc = g_lkl.init_fn(g_lkl.host_ops);
    if (rc != 0) {
        LOGE("lkl_init rc=%d", rc);
        pthread_mutex_unlock(&g_lkl.lock);
        return rc;
    }

    LOGI("lkl_start_kernel(\"mem=64M loglevel=8\")");
    rc = g_lkl.start_kernel("mem=64M loglevel=8");
    if (rc == 0) {
        g_lkl.running = 1;
        LOGI("lkl_start_kernel OK");

        /* devtmpfs mount /dev-re: a kernel által auto-kreált device-nodok
         * (null, zero, urandom, console, tty, ptmx, …) az LKL-en belül a
         * /dev-ben kell hogy megjelenjenek, hogy a chrooted-mirror lássa
         * őket. Az LKL CONFIG_DEVTMPFS_MOUNT NEM aktív build-time, ezért
         * user-mode-on mountoljuk. */
        long mr = lkl_mkdir("/dev", 0755);
        (void)mr;  /* EEXIST OK */
        long mt = lkl_mount("none", "/dev", "devtmpfs", 0, NULL);
        if (mt == 0) {
            LOGI("devtmpfs mounted on /dev");
        } else {
            LOGI("devtmpfs mount rc=%ld (CONFIG_DEVTMPFS hiányozhat)", mt);
        }

        /* devpts mount /dev/pts-re — pty-knek kell. */
        long mp = lkl_mkdir("/dev/pts", 0755);
        (void)mp;
        long mpt = lkl_mount("devpts", "/dev/pts", "devpts", 0, NULL);
        if (mpt == 0) LOGI("devpts mounted on /dev/pts");

        /* sysfs auto-mountolva általában; explicit fallback. */
        long ms = lkl_mkdir("/sys", 0755);
        (void)ms;
        long mss = lkl_mount("sysfs", "/sys", "sysfs", 0, NULL);
        if (mss == 0) LOGI("sysfs explicit re-mount on /sys");

        /* proc szintén — biztos ami biztos. */
        long mpp = lkl_mkdir("/proc", 0755);
        (void)mpp;
        long mpps = lkl_mount("proc", "/proc", "proc", 0, NULL);
        if (mpps == 0) LOGI("proc explicit re-mount on /proc");
    } else {
        LOGE("lkl_start_kernel rc=%d", rc);
    }
    pthread_mutex_unlock(&g_lkl.lock);
    return rc;
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklStop(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_lkl.lock);
    if (!g_lkl.running) {
        pthread_mutex_unlock(&g_lkl.lock);
        return -ENOTCONN;
    }
    if (!g_lkl.sys_halt) {
        g_lkl.running = 0;
        g_lkl.terminated = 1;
        pthread_mutex_unlock(&g_lkl.lock);
        return -ENOSYS;
    }
    long rc = g_lkl.sys_halt();
    LOGI("lkl_sys_halt rc=%ld", rc);
    /* `lkl_cleanup` az LKL docs szerint a halt utáni tisztogatáshoz —
     * KASAN, host-allokált struct-ok stb. (best-effort). */
    if (g_lkl.cleanup_fn) {
        g_lkl.cleanup_fn();
        LOGI("lkl_cleanup hívva");
    }
    g_lkl.running = 0;
    /* Az LKL kernel a halt + cleanup után IRREVERZIBILIS shutdown state-ben
     * van. Új lkl_init/lkl_start_kernel hívás ugyanezen processzben
     * upstream NEM támogatott — kernel-globális struct-ok (cmd_line buffer,
     * percpu, IRQ state) reset nélkül maradnak, második start crash-eli az
     * appot. Ezért terminated flag-et állítunk; nativeLklStart -EHOSTDOWN-t
     * ad, a UI kéri az app-restart-ot. */
    g_lkl.terminated = 1;
    pthread_mutex_unlock(&g_lkl.lock);
    return (jint)rc;
}

/* ── Phase 2c.5b/c: USB fd átvétele + vhci_hcd attach kísérlet ─────────
 *
 * A main process Binder-en át (ParcelFileDescriptor) átadja a friss-dup-olt
 * fd-t a `:lkl` process-nek; ott libusb_wrap_sys_device-szal megnyitjuk,
 * descriptor-t olvasunk, majd — ha a kernel fut — megpróbáljuk a usbip
 * vhci_hcd-hez attach-olni:
 *   1. lkl_sys_socketpair(AF_UNIX, SOCK_STREAM) → sv[0] (kernel-side),
 *      sv[1] (kernel-side; URB-fluxhoz későbbi iterációban)
 *   2. lkl_sys_open "/sys/devices/platform/vhci_hcd.0/attach"
 *   3. lkl_sys_write "port_id sockfd devid speed"
 *
 * MEGJEGYZÉS — a teljes URB dispatch loop (sv[1] olvasás/írás lkl_sys_-vel +
 * libusb_submit_transfer + RET_SUBMIT) ennek az iterációnak nem része. Az
 * attach után a vhci_hcd KÖZBEN várni fog adatra; ha a kernel-thread
 * timeout-ra fut, a /sys/bus/usb/devices nem fog megjelenni. Ezt a 2c.5d
 * iteráció hozza össze.
 *
 * Cél most: bizonyítani hogy a Binder fd-passzás a `:lkl` processzig megy,
 * hogy libusb a wrap-elt fd-n olvasni tud a kernelben, és hogy a sysfs
 * attach útvonal írható (= vhci_hcd modul ÉL és kész fogadni).
 */
JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklAttachUsbDevice(JNIEnv *env, jobject thiz,
                                                          jint fd, jint vid, jint pid,
                                                          jint busnum, jint devnum)
{
    char out[3072];
    char *p = out, *e = out + sizeof(out);
    #define APPEND(...) do { if (p < e) p += snprintf(p, (size_t)(e - p), __VA_ARGS__); } while(0)

    APPEND("USB attach a :lkl process-ben (pid=%d)\n", getpid());
    APPEND("Bemenet: fd=%d VID=%04x PID=%04x bus=%d dev=%d\n",
           fd, (unsigned)vid, (unsigned)pid, busnum, devnum);

    /* 1) dup — a Binder már egyszer dup-olta nálunk; még egy dup-pal libusb
     *    saját ownership-t kap, és nem ütközünk a Java oldali PFD close-jával. */
    int dup_fd = dup(fd);
    if (dup_fd < 0) {
        APPEND("dup(%d) hiba: %s\n", fd, strerror(errno));
        return (*env)->NewStringUTF(env, out);
    }
    APPEND("dup(%d) → %d\n", fd, dup_fd);

    /* 2) libusb wrap-elés (root nélkül, NO_DEVICE_DISCOVERY-vel). */
    int rc = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (rc != LIBUSB_SUCCESS) {
        APPEND("figyelmeztetés: libusb_set_option(NO_DEVICE_DISCOVERY): %s\n",
               libusb_strerror(rc));
    }
    libusb_context *ctx = NULL;
    rc = libusb_init(&ctx);
    if (rc < 0) {
        APPEND("libusb_init: %s\n", libusb_strerror(rc));
        close(dup_fd);
        return (*env)->NewStringUTF(env, out);
    }
    libusb_device_handle *handle = NULL;
    rc = libusb_wrap_sys_device(ctx, (intptr_t)dup_fd, &handle);
    if (rc < 0) {
        APPEND("libusb_wrap_sys_device(fd=%d): %s\n", dup_fd, libusb_strerror(rc));
        close(dup_fd);
        libusb_exit(ctx);
        return (*env)->NewStringUTF(env, out);
    }
    APPEND("libusb_wrap_sys_device: OK\n");

    /* Descriptor diagnostics — bizonyítja hogy libusb tud olvasni a wrap-elt
     * fd-ről (Android usbfs sysfs-en át). */
    libusb_device *dev = libusb_get_device(handle);
    if (dev) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(dev, &d) == 0) {
            APPEND("device descriptor:\n"
                   "  VID=%04x PID=%04x  class=%02x.%02x.%02x  bcdUSB=%04x\n",
                   d.idVendor, d.idProduct,
                   d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol,
                   d.bcdUSB);
        }
    }

    /* 3) vhci_hcd attach kísérlet — csak ha az LKL kernel fut. */
    pthread_mutex_lock(&g_lkl.lock);
    int kernel_alive = (g_lkl.running && g_lkl.syscall_fn != NULL);
    pthread_mutex_unlock(&g_lkl.lock);

    if (!kernel_alive) {
        APPEND("\n(LKL kernel nem fut — vhci_hcd attach kihagyva.)\n");
    } else {
        APPEND("\n── vhci_hcd attach (LKL kernel-szintű) ──\n");
        /* /proc + /sys mount-elés (idempotens). */
        lkl_mount_once("proc", "/proc", "proc");
        long m = lkl_mount_once("sysfs", "/sys", "sysfs");
        if (m < 0) {
            APPEND("mount(/sys): rc=%ld\n", m);
        }

        /* DIAGNOSZTIKA: registered network protocols. */
        char protos[768];
        size_t plen = 0;
        long pr = lkl_read_file("/proc/net/protocols", protos, sizeof(protos), &plen);
        if (pr > 0) {
            APPEND("/proc/net/protocols (regisztrált AF-ek):\n%s\n", protos);
        } else {
            APPEND("/proc/net/protocols: rc=%ld\n", pr);
        }

        /* DIAGNOSZTIKA: AF_INET vs AF_UNIX próba külön socket()-tel. */
        long inet_fd = lkl_call(LKL_NR_socket, LKL_AF_INET, LKL_SOCK_STREAM, 0, 0, 0);
        APPEND("socket(AF_INET, SOCK_STREAM): rc=%ld%s\n", inet_fd,
               inet_fd >= 0 ? " ✓" : "");
        if (inet_fd >= 0) lkl_close(inet_fd);

        long unix_fd = lkl_call(LKL_NR_socket, LKL_AF_UNIX, LKL_SOCK_STREAM, 0, 0, 0);
        APPEND("socket(AF_UNIX, SOCK_STREAM): rc=%ld%s\n", unix_fd,
               unix_fd >= 0 ? " ✓" : "");
        if (unix_fd >= 0) lkl_close(unix_fd);

        /* socketpair LKL-belső fd-kkel. Ezek a kernel current_task fd
         * táblájában jönnek létre — ugyanaz a tábla amit sockfd_lookup
         * használ az attach-implementációban.
         *
         * MEGJEGYZÉS — a kernel socketpair() szignatúrája:
         *   SYSCALL_DEFINE4(socketpair, int, fam, int, type, int, proto,
         *                   int __user *, usockvec)
         * A kernel 2 db `int`-et ír (8 byte), tehát a buffert is `int[2]`-ként
         * kell deklarálni. Régebben `long[2]`-ként volt — az 16 byte, a kernel
         * csak az alsó 8 byte-ot tölti ki, ami azt jelenti hogy
         * sv[0] = (fd1 << 32) | fd0 (értelmetlen érték, pl. 4294967296),
         * sv[1] = uninicializált. A vhci_hcd attach ezt a hibás fd-t kapta,
         * amit sockfd_lookup nem talált meg → silent fail. */
        int sv[2] = { -1, -1 };
        long sp_rc = lkl_call(LKL_NR_socketpair, LKL_AF_UNIX, LKL_SOCK_STREAM,
                              0, (long)(intptr_t)sv, 0);
        if (sp_rc < 0) {
            APPEND("lkl socketpair(AF_UNIX): rc=%ld — AF_UNIX nem támogatott\n", sp_rc);
        } else {
            APPEND("lkl socketpair → sv[0]=%d sv[1]=%d\n", sv[0], sv[1]);

            /* Megnyitjuk a vhci_hcd attach sysfs-fájlt írásra. */
            long sysfs = lkl_open("/sys/devices/platform/vhci_hcd.0/attach",
                                  LKL_O_WRONLY);
            if (sysfs < 0) {
                APPEND("lkl open(/sys/devices/platform/vhci_hcd.0/attach): "
                       "rc=%ld\n", sysfs);
                APPEND("(esetleg másik útvonal? lehetséges: "
                       "/sys/bus/platform/drivers/vhci_hcd/attach)\n");
            } else {
                APPEND("attach sysfs nyitva: fd=%ld\n", sysfs);

                /* Format: "<port_id> <sockfd> <devid> <speed>"
                 *   port_id = 0 (az első virtuális port)
                 *   sockfd  = sv[0]  (kernel-side socket fd)
                 *   devid   = (busnum << 16) | devnum  (USB/IP konvenció)
                 *   speed   = 3  (USB_SPEED_HIGH; USB 2.0)
                 */
                uint32_t devid = ((uint32_t)busnum << 16) | (uint32_t)devnum;
                char cmd[80];
                int n = snprintf(cmd, sizeof(cmd), "0 %d %u 3",
                                 sv[0], (unsigned)devid);
                long w = lkl_call(LKL_NR_write, sysfs, (long)(intptr_t)cmd,
                                  (long)n, 0, 0);
                APPEND("attach write \"%s\" (%d byte) → rc=%ld\n", cmd, n, w);
                lkl_close(sysfs);
                if (w == n) {
                    APPEND("✓ vhci_hcd kernel-thread elindítva (URB-eket vár).\n");
                    /* Phase 2c.5d — host-oldali URB-dispatch worker indítása.
                     * Innentől a worker birtokolja a libusb-erőforrásokat;
                     * NE zárjuk a függvény végén. */
                    int brc = start_urb_bridge(ctx, handle, dup_fd,
                                                sv[0], sv[1], devid);
                    if (brc == 0) {
                        APPEND("✓ URB dispatch worker elindítva (sync mode)\n");
                        ctx = NULL;       /* tulajdonjog a worker-é */
                        handle = NULL;
                        dup_fd = -1;
                    } else {
                        APPEND("URB worker indítás hiba: rc=%d\n", brc);
                    }
                }
            }
        }
    }

    /* Cleanup — csak akkor, ha NEM adtuk át az erőforrásokat a worker-nek
     * (= a bridge start nem sikerült, vagy az attach valami korábbi
     * lépésen elbukott). A `dup_fd`-t a `libusb_wrap_sys_device` óta a
     * libusb birtokolja; libusb_close zárja. */
    if (handle) libusb_close(handle);
    if (ctx)    libusb_exit(ctx);

    #undef APPEND
    return (*env)->NewStringUTF(env, out);
}

/* ────────────────────────────────────────────────────────────────────
 *  Phase 3 — proot+bash spawn a :lkl process-ben.
 *
 *  Arch-refaktor indok: a Samsung BBA (és általában minden agresszív
 *  Android task-killer) UID-szintű kill-t végez, ezért a main process
 *  halálával a vele futó TerminalSession (bash) is meghal — még akkor
 *  is ha a `:lkl` foreground notification-nel él. Megoldás: a bash
 *  futtatása költözzön a `:lkl` process-be, így a `:lkl` foreground
 *  notification mind a kernel-t mind a shell-t együtt védi.
 *
 *  Flow:
 *    1) main hívja: iface.startKaliShell(shellPath, cwd, args, env, cols, rows)
 *    2) AIDL átviszi a `:lkl` process-be, ami `forkpty()`-vel létrehoz
 *       egy PTY-pair-t. A child execve(shellPath, args, env) → launch.sh
 *       → proot → bash. A parent (`:lkl`) megtartja a master fd-t.
 *    3) A Binder válasz visszaviszi a master fd-t main-re ParcelFileDescriptor-
 *       szel (Binder kernel-szinten dup-ol). Main-on a TerminalSession
 *       attach-eli ezt a fd-t (external-pty mode).
 *    4) Ha main meghal Samsung-BBA miatt, a `:lkl` és benne a bash él
 *       tovább. Új main spawn-jakor a TerminalSession ismét csatolható
 *       a meglévő fd-re (`nativeLklGetShellFd`).
 * ──────────────────────────────────────────────────────────────────── */

static struct {
    int master_fd;     /* -1 ha nincs aktív shell */
    pid_t pid;         /* a forkpty child pid-je */
    pthread_mutex_t lock;
} g_shell = { -1, 0, PTHREAD_MUTEX_INITIALIZER };

/* Helper — Java String → C string (caller frees). */
static char *jstr_dup(JNIEnv *env, jstring js)
{
    if (!js) return NULL;
    const char *s = (*env)->GetStringUTFChars(env, js, NULL);
    char *d = s ? strdup(s) : NULL;
    if (s) (*env)->ReleaseStringUTFChars(env, js, s);
    return d;
}

/* Helper — Java String[] → char** NULL-terminated (caller frees with free_argv). */
static char **jstr_array_dup(JNIEnv *env, jobjectArray jarr)
{
    if (!jarr) {
        char **a = calloc(1, sizeof(char *));
        return a;
    }
    jsize n = (*env)->GetArrayLength(env, jarr);
    char **a = calloc((size_t)n + 1, sizeof(char *));
    if (!a) return NULL;
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, jarr, i);
        a[i] = jstr_dup(env, s);
        if (s) (*env)->DeleteLocalRef(env, s);
    }
    return a;
}

static void free_argv(char **a)
{
    if (!a) return;
    for (size_t i = 0; a[i]; i++) free(a[i]);
    free(a);
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklSpawnShell(
    JNIEnv *env, jobject thiz,
    jstring jshellPath, jstring jcwd, jobjectArray jargs, jobjectArray jenv,
    jint cols, jint rows)
{
    pthread_mutex_lock(&g_shell.lock);
    if (g_shell.master_fd > 0) {
        /* már van futó shell — idempotens: a meglévő master_fd-t adjuk vissza
         * (a hívó dup-olja PFD-vel a saját process-ébe). */
        int fd = g_shell.master_fd;
        pthread_mutex_unlock(&g_shell.lock);
        LOGI("nativeLklSpawnShell: már van shell (pid=%d), reusing fd=%d", g_shell.pid, fd);
        return fd;
    }
    pthread_mutex_unlock(&g_shell.lock);

    char *shell_path = jstr_dup(env, jshellPath);
    char *cwd = jstr_dup(env, jcwd);
    char **args = jstr_array_dup(env, jargs);
    char **envp = jstr_array_dup(env, jenv);

    if (!shell_path || !args || !envp) {
        free(shell_path); free(cwd); free_argv(args); free_argv(envp);
        return -ENOMEM;
    }

    struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols,
                          .ws_xpixel = 0, .ws_ypixel = 0 };
    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    tio.c_iflag = ICRNL | IXON;
    tio.c_oflag = OPOST | ONLCR;
    tio.c_cflag = CS8 | CREAD;
    tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE;
    cfsetispeed(&tio, B38400);
    cfsetospeed(&tio, B38400);

    int master_fd = -1;
    pid_t pid = forkpty(&master_fd, NULL, &tio, &ws);
    if (pid < 0) {
        int e = errno;
        LOGE("forkpty failed: %s", strerror(e));
        free(shell_path); free(cwd); free_argv(args); free_argv(envp);
        return -e;
    }

    if (pid == 0) {
        /* CHILD — execve into shellPath */
        if (cwd && cwd[0]) {
            if (chdir(cwd) != 0) {
                fprintf(stderr, "[lkl-shell-child] chdir(%s) failed: %s\n", cwd, strerror(errno));
            }
        }
        execve(shell_path, args, envp);
        /* execve csak hiba esetén tér vissza */
        fprintf(stderr, "[lkl-shell-child] execve(%s) failed: %s\n", shell_path, strerror(errno));
        _exit(127);
    }

    /* PARENT — szülő :lkl process */
    free(shell_path); free(cwd); free_argv(args); free_argv(envp);

    /* close-on-exec, hogy a master fd ne lyukadjon ki ha későbbi
     * forkpty-k lennének. */
    int flags = fcntl(master_fd, F_GETFD);
    if (flags >= 0) fcntl(master_fd, F_SETFD, flags | FD_CLOEXEC);

    pthread_mutex_lock(&g_shell.lock);
    g_shell.master_fd = master_fd;
    g_shell.pid = pid;
    pthread_mutex_unlock(&g_shell.lock);

    LOGI("nativeLklSpawnShell OK: pid=%d, master_fd=%d, %dx%d", pid, master_fd, cols, rows);
    return master_fd;
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklGetShellFd(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_shell.lock);
    int fd = g_shell.master_fd;
    pthread_mutex_unlock(&g_shell.lock);
    return fd;
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklGetShellPid(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_shell.lock);
    int pid = (int)g_shell.pid;
    pthread_mutex_unlock(&g_shell.lock);
    return pid;
}

JNIEXPORT void JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklSetShellSize(
    JNIEnv *env, jobject thiz, jint cols, jint rows)
{
    pthread_mutex_lock(&g_shell.lock);
    int fd = g_shell.master_fd;
    pthread_mutex_unlock(&g_shell.lock);
    if (fd <= 0) return;
    struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols, 0, 0 };
    ioctl(fd, TIOCSWINSZ, &ws);
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklKillShell(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_shell.lock);
    int fd = g_shell.master_fd;
    pid_t pid = g_shell.pid;
    g_shell.master_fd = -1;
    g_shell.pid = 0;
    pthread_mutex_unlock(&g_shell.lock);

    if (pid > 0) {
        kill(pid, SIGTERM);
        /* nem várjuk meg — SIGCHLD reaper kell, de :lkl process death
         * mindenképp megöli a zombie-t. */
    }
    if (fd > 0) close(fd);
    return 0;
}

/* ────────────────────────────────────────────────────────────────────
 *  Phase 4 — bulk readTree: az ÖSSZES LKL /proc /sys /dev fájlt EGY
 *  Binder hívásban szerializáljuk és a main process valódi diszkre
 *  materalizálja. A proot ezeket bind-mountolja valódi mappákként.
 *  Sem LD_PRELOAD shim sem control-socket nem kell file-op-okhoz —
 *  minden libc/libsystemd hívás valódi fd-vel megy.
 *
 *  Format (NUL-free a kódolásban):
 *    'D' '\n' <path> '\n'            → directory
 *    'F' '\n' <path> '\n' <size> '\n' <bytes> '\n'  → file
 *    'L' '\n' <path> '\n' <target> '\n'   → symlink (későbbi)
 *    'E' '\n'                        → end
 *
 *  A <content_bytes> tartalmazhat bármilyen byte-ot (NUL is) — a
 *  méretet a <size> dönti el (decimal ASCII), nem a NUL.
 * ──────────────────────────────────────────────────────────────────── */

static int is_skip_entry(const char *name)
{
    /* KRITIKUS: a /proc-ban van NÉHÁNY fájl, ami olvasáskor BLOKKOL
     * (várja a kernel-event-et, pl. /proc/kmsg). Mivel a walk_lkl
     * a g_lkl.lock-ot tartja, az egész :lkl process megáll a Binder-
     * hívásig. Ezeket explicit skipeljük. */
    static const char *skip[] = {
        /* sysfs symlink-loop, slab-explosion, runtime-PM stb. */
        "subsystem", "driver", "module", "of_node",
        "cwd", "exe", "root", "fd", "fdinfo", "task",
        "slab", "cache", "power", "efi",
        "self", "thread-self",
        /* /proc blokkoló read-ek (várnak kernel-eventre v. lapozásra) */
        "kmsg",                /* várja a kernel-üzenetet */
        "kpagecount", "kpageflags", "kpagecgroup",  /* lapozási scan, lassú */
        "pagetypeinfo",        /* lassú */
        "sysrq-trigger",       /* triggers magic-key */
        "interrupts",          /* read-OK de lassú nagy CPU-szám esetén */
        /* sysfs lassú scan-jek (NOTE: 'uevent' KIVÉVE — a libudev a
         * device discovery-hez ENUMERÁCIÓKOR olvassa, hiányában 0 device.) */
        "trace_pipe",          /* blokkoló stream */
        "trace_pipe_raw",
        "trigger", "trigger0", /* tracing-trigger-ek */
        NULL
    };
    for (int i = 0; skip[i]; i++) {
        if (strcmp(name, skip[i]) == 0) return 1;
    }
    /* all-digit nevek: /proc/<PID> process-mappák, slab számok */
    if (name[0] >= '0' && name[0] <= '9') {
        int ad = 1;
        for (const char *p = name; *p; p++)
            if (*p < '0' || *p > '9') { ad = 0; break; }
        if (ad) return 1;
    }
    return 0;
}

/* Helper — append varargs printf-style into out buffer; returns -1 if cap full. */
#define APPENDF(...) do {                                                \
    int _n = snprintf(out + *pos, cap - *pos, __VA_ARGS__);              \
    if (_n < 0 || (size_t)_n >= cap - *pos) return -1;                   \
    *pos += (size_t)_n;                                                  \
} while (0)

/* Globális deadline timer a walk_lkl-hez. Hard-limit 5 sec /walk-call,
 * hogy a UI-thread NE várhassa fél-percig a Binder-választ. */
static long long g_walk_deadline_ms = 0;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int walk_lkl(const char *path, char *out, size_t *pos, size_t cap,
                    int depth, int max_per_dir);

static int walk_lkl(const char *path, char *out, size_t *pos, size_t cap,
                    int depth, int max_per_dir)
{
    /* BUG-FIX: korábban `if (depth <= 0) return 0;` itt volt → a leaf-fájlok
     * (pl. /sys/bus/usb/devices/usb1/busnum) NEM lettek beolvasva. A depth
     * csak a RECURSION-ra vonatkozik (mappákba lemenni); a fájlt akkor is
     * be kell olvasni, ha depth=0. */
    if (*pos + 256 > cap) return -1;  /* nincs hely a metadatra sem */
    if (now_ms() > g_walk_deadline_ms) {
        LOGW("walk_lkl: deadline expired at %s", path);
        return -1;
    }

    /* O_NONBLOCK: a blokkoló /proc fájlok (kmsg, trace_pipe stb. — ha
     * valami elkerülte az is_skip_entry check-et) EAGAIN-nel visszatérnek
     * ahelyett hogy a walk-ot befagyasztanák. */
    long fd = lkl_open(path, LKL_O_RDONLY | LKL_O_NONBLOCK);
    if (fd < 0) {
        /* Nem létezik vagy nincs jogosultság — kihagyjuk. */
        return 0;
    }

    /* Próbáljuk getdents-szel — ha működik, directory.
     * Ha ENOTDIR (-20), file. */
    char dirbuf[2048];
    long bytes = lkl_call(LKL_NR_getdents64, fd,
                          (long)(intptr_t)dirbuf, (long)sizeof(dirbuf), 0, 0);
    if (bytes < 0) {
        /* File — beolvassuk a tartalmat (max 64KB-ig).
         * MAX 8 olvasási iteráció és EAGAIN-re KIESÜNK, hogy egy lassú
         * vagy nem-poll-elhető fájl ne fogja vissza a walk-ot. */
        char *content = malloc(65536);
        if (!content) { lkl_close(fd); return -1; }
        long total = 0;
        int iter = 0;
        while (total < 65536 && iter++ < 16) {
            long r = lkl_read(fd, content + total, 65536 - total);
            if (r == -LKL_EAGAIN) break;  /* nem-blokkoló: nincs adat most */
            if (r <= 0) break;            /* EOF v. hiba */
            total += r;
        }
        lkl_close(fd);

        /* Output: F\n<path>\n<size>\n<bytes>\n */
        if (*pos + 32 + strlen(path) + (size_t)total > cap) {
            free(content);
            return -1;
        }
        APPENDF("F\n%s\n%ld\n", path, total);
        memcpy(out + *pos, content, (size_t)total);
        *pos += (size_t)total;
        if (*pos + 1 > cap) { free(content); return -1; }
        out[(*pos)++] = '\n';
        free(content);
        return 0;
    }

    /* Directory — emit + collect children, then recurse.
     * depth <= 0 esetén: csak az üres directory-bejegyzést emit-eljük,
     * a tartalmába NEM megyünk le. */
    APPENDF("D\n%s\n", path);
    if (depth <= 0) {
        lkl_close(fd);
        return 0;
    }

    /* Gyűjtsük be a gyermek-neveket */
    char *names[128];
    int nn = 0;
    while (bytes > 0 && nn < max_per_dir) {
        long off = 0;
        while (off < bytes && nn < max_per_dir) {
            struct lkl_linux_dirent64 *d =
                (struct lkl_linux_dirent64 *)(dirbuf + off);
            if (d->d_reclen == 0) goto dirdone;
            const char *name = d->d_name;
            if (!(name[0] == '.' && (name[1] == '\0' ||
                  (name[1] == '.' && name[2] == '\0'))) &&
                !is_skip_entry(name))
            {
                names[nn++] = strdup(name);
            }
            off += d->d_reclen;
        }
        if (nn >= max_per_dir) break;
        bytes = lkl_call(LKL_NR_getdents64, fd,
                         (long)(intptr_t)dirbuf, (long)sizeof(dirbuf), 0, 0);
    }
dirdone:
    lkl_close(fd);

    /* Recurse — az allokált gyermek-neveket lépésenként szabadítjuk fel */
    for (int i = 0; i < nn; i++) {
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, names[i]);
        free(names[i]);
        if (walk_lkl(child, out, pos, cap, depth - 1, max_per_dir) < 0) {
            /* cap-túlcsordulás — felszabadítjuk a maradékot */
            for (int j = i + 1; j < nn; j++) free(names[j]);
            return -1;
        }
    }
    return 0;
}

JNIEXPORT jbyteArray JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklReadTree(
    JNIEnv *env, jobject thiz, jstring jroot,
    jint max_depth, jint max_bytes, jint max_per_dir)
{
    const char *root = (*env)->GetStringUTFChars(env, jroot, NULL);
    if (!root) return NULL;

    size_t cap = (max_bytes > 0) ? (size_t)max_bytes : (4 * 1024 * 1024);
    char *out = malloc(cap);
    if (!out) {
        (*env)->ReleaseStringUTFChars(env, jroot, root);
        return NULL;
    }
    size_t pos = 0;

    pthread_mutex_lock(&g_lkl.lock);
    lkl_resolve_locked();
    if (g_lkl.running && g_lkl.syscall_fn) {
        int mpd = (max_per_dir > 0) ? max_per_dir : 64;
        /* Hard deadline: 5 sec a teljes walk-ra, függetlenül mit talál. */
        g_walk_deadline_ms = now_ms() + 5000;
        LOGI("walk_lkl START root=%s depth=%d", root, max_depth);
        walk_lkl(root, out, &pos, cap, max_depth > 0 ? max_depth : 5, mpd);
        LOGI("walk_lkl END root=%s emitted=%zu bytes", root, pos);
    }
    pthread_mutex_unlock(&g_lkl.lock);

    /* Terminate */
    if (pos + 2 <= cap) {
        out[pos++] = 'E';
        out[pos++] = '\n';
    }

    (*env)->ReleaseStringUTFChars(env, jroot, root);
    jbyteArray arr = (*env)->NewByteArray(env, (jsize)pos);
    if (arr) (*env)->SetByteArrayRegion(env, arr, 0, (jsize)pos, (jbyte *)out);
    free(out);
    return arr;
}
