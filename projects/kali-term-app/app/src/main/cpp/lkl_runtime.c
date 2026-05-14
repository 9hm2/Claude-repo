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
typedef long (*fn_lkl_sys_halt)(void);
typedef long (*fn_lkl_sys_open)(const char *pathname, int flags, int mode);
typedef long (*fn_lkl_sys_read)(int fd, void *buf, unsigned long count);

static struct {
    pthread_mutex_t lock;
    int             resolved;        /* dlsym próbálta-e már */
    int             available;       /* lkl_host_ops + start_kernel megvolt-e */
    int             running;         /* lkl_init + lkl_start_kernel sikerült-e */
    void           *dl_handle;       /* dlopen("liblkl.so") visszaértéke */
    fn_lkl_init         init_fn;
    fn_lkl_start_kernel start_kernel;
    fn_lkl_sys_halt     sys_halt;
    fn_lkl_sys_open     sys_open;
    fn_lkl_sys_read     sys_read;
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
    g_lkl.sys_halt     = (fn_lkl_sys_halt)    resolve("lkl_sys_halt");
    g_lkl.sys_open     = (fn_lkl_sys_open)    resolve("lkl_sys_open");
    g_lkl.sys_read     = (fn_lkl_sys_read)    resolve("lkl_sys_read");
    g_lkl.host_ops     =                       resolve("lkl_host_ops");

    /* available = a minimum bekapcsoláshoz szükséges szimbólumok megvannak */
    g_lkl.available = (g_lkl.init_fn      != NULL &&
                       g_lkl.start_kernel != NULL &&
                       g_lkl.host_ops     != NULL);

    /* Részletes diagnosztika MINDIG (jó/rossz esetre is), per-szimbólum-sor */
    snprintf(g_lkl.status_buf, sizeof(g_lkl.status_buf),
             "%s — szimbólumok (dlsym):\n"
             "  lkl_init         = %p%s\n"
             "  lkl_start_kernel = %p%s\n"
             "  lkl_host_ops     = %p%s\n"
             "  lkl_sys_halt     = %p%s\n"
             "  lkl_sys_open     = %p%s\n"
             "  lkl_sys_read     = %p%s\n"
             "  dl_handle        = %p",
             g_lkl.available ? "AVAILABLE" : "UNAVAILABLE",
             (void*)g_lkl.init_fn,      g_lkl.init_fn      ? "" : "  ← MISSING",
             (void*)g_lkl.start_kernel, g_lkl.start_kernel ? "" : "  ← MISSING",
             g_lkl.host_ops,            g_lkl.host_ops     ? "" : "  ← MISSING",
             (void*)g_lkl.sys_halt,     g_lkl.sys_halt     ? "" : "  (opcionális)",
             (void*)g_lkl.sys_open,     g_lkl.sys_open     ? "" : "  (opcionális)",
             (void*)g_lkl.sys_read,     g_lkl.sys_read     ? "" : "  (opcionális)",
             g_lkl.dl_handle);

    LOGI("LKL resolve: available=%d", g_lkl.available);
}

/* ── JNI ─────────────────────────────────────────────────────────────── */

JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklStatus(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_lkl.lock);
    lkl_resolve_locked();
    char out[1280];
    snprintf(out, sizeof(out), "%s\nrunning = %s",
             g_lkl.status_buf, g_lkl.running ? "YES" : "no");
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
        pthread_mutex_unlock(&g_lkl.lock);
        return -ENOSYS;
    }
    long rc = g_lkl.sys_halt();
    g_lkl.running = 0;
    pthread_mutex_unlock(&g_lkl.lock);
    LOGI("lkl_sys_halt rc=%ld", rc);
    return (jint)rc;
}
