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

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

#define LKL_AT_FDCWD         (-100)
#define LKL_O_RDONLY         0
#define LKL_EBUSY            16
#define LKL_ENOENT           2

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

/* A futó LKL kernel "életjeleinek" összeszedése: /proc/version + a
 * /sys/bus/usb/devices könyvtár jelenléte. Mind tisztán LKL-on belül
 * fut, lkl_syscall hívásokkal. */
static void probe_kernel_into(char *buf, size_t bufsz)
{
    char *p = buf;
    char *end = buf + bufsz;
    #define APPEND(...) do { if (p < end) p += snprintf(p, end - p, __VA_ARGS__); } while(0)

    if (!g_lkl.syscall_fn) {
        APPEND("lkl_syscall not resolved");
        return;
    }

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

    /* /sys/bus/usb/devices — listázzuk, hogy lássuk: van-e vhci_hcd
     * által kreált root hub. Ehhez open + getdents kellene; egyszerűbb
     * csak az exist-checket csinálni open()-nel. */
    long fd = lkl_open("/sys/bus/usb/devices", LKL_O_RDONLY);
    if (fd >= 0) {
        APPEND("/sys/bus/usb/devices: létezik (vhci_hcd jelen)\n");
        lkl_close(fd);
    } else {
        APPEND("/sys/bus/usb/devices: rc=%ld (vhci_hcd még nincs attach-elve)\n", fd);
    }
    #undef APPEND
}

/* ── JNI ─────────────────────────────────────────────────────────────── */

JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklStatus(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_lkl.lock);
    lkl_resolve_locked();
    const char *running_state =
        g_lkl.terminated ? "TERMINATED (app-restart kell)" :
        g_lkl.running    ? "YES"                            :
                           "no";

    char out[3072];
    int n = snprintf(out, sizeof(out), "%s\nrunning = %s",
                     g_lkl.status_buf, running_state);

    /* Ha fut a kernel, csatoljunk hozzá egy in-kernel életjel-probe-ot:
     * mount /proc, mount /sys, read /proc/version, /sys/bus/usb létezés.
     * Ezzel látszik a UI-ban, hogy a Linux kernel valóban fut és
     * filesystem-syscalls működnek a `lkl_syscall` ABI-n keresztül. */
    if (g_lkl.running && g_lkl.syscall_fn && n < (int)sizeof(out) - 256) {
        char probe[1536];
        probe_kernel_into(probe, sizeof(probe));
        snprintf(out + n, sizeof(out) - n, "\n\n── kernel probe ──\n%s", probe);
    }

    pthread_mutex_unlock(&g_lkl.lock);
    return (*env)->NewStringUTF(env, out);
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
