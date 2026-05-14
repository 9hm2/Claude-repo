/*
 * LKL (Linux Kernel Library) runtime hookok.
 *
 * Két oka van hogy `dlsym(RTLD_DEFAULT, ...)`-ot használunk a közvetlen
 * linkelés helyett:
 *   1) Az LKL .so jelenleg nincs Android-ARM64-re fordítva (a kernel-build
 *      Bionic-ABI targetet még nem támogat — ld. STATUS.md). Az APK
 *      ezért enélkül épül, és a JNI-réteg "graceful degrade" módban fut:
 *      a Kotlin oldal megpróbálja betölteni System.loadLibrary("lkl-host-lib")-vel,
 *      ha sikerül, a szimbólumok itt resolve-olhatóak; ha nem, mindent
 *      "UNAVAILABLE" jelez vissza.
 *   2) Még akkor is, ha a .so megvan, a CMakeLists.txt nem köti hozzá
 *      build-time, mert a target csak akkor létezik a fájlrendszerben,
 *      ha a kali-term-app CI-step letöltötte a kernel-build artifact-ot.
 *      A dlsym(RTLD_DEFAULT) ezért robusztusabb.
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

/* Az LKL ABI-jának dlsym-alapú leírása.
 * A signatúrák a tools/lkl/include/lkl.h-ban definiált deklarációknak
 * felelnek meg. Pointer-szintű placeholder-eket használunk a `void *`
 * helyett, hogy a fordító ne nyögjön a header hiánya miatt. */
typedef int  (*fn_lkl_start_kernel)(void *ops, const char *cmd_line);
typedef long (*fn_lkl_sys_halt)(void);
typedef long (*fn_lkl_sys_open)(const char *pathname, int flags, int mode);
typedef long (*fn_lkl_sys_read)(int fd, void *buf, unsigned long count);

static struct {
    pthread_mutex_t lock;
    int             resolved;        /* dlsym próbálta-e már */
    int             available;       /* lkl_host_ops + start_kernel megvolt-e */
    int             running;         /* lkl_start_kernel sikerült-e */
    fn_lkl_start_kernel start_kernel;
    fn_lkl_sys_halt     sys_halt;
    fn_lkl_sys_open     sys_open;
    fn_lkl_sys_read     sys_read;
    void               *host_ops;    /* lkl_host_ops szimbólum */
    char                status_buf[768];
} g_lkl = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* dlsym minden szimbólumra; idempotens. */
static void lkl_resolve_locked(void)
{
    if (g_lkl.resolved) return;
    g_lkl.resolved = 1;

    g_lkl.start_kernel = (fn_lkl_start_kernel)dlsym(RTLD_DEFAULT, "lkl_start_kernel");
    g_lkl.sys_halt     = (fn_lkl_sys_halt)    dlsym(RTLD_DEFAULT, "lkl_sys_halt");
    g_lkl.sys_open     = (fn_lkl_sys_open)    dlsym(RTLD_DEFAULT, "lkl_sys_open");
    g_lkl.sys_read     = (fn_lkl_sys_read)    dlsym(RTLD_DEFAULT, "lkl_sys_read");
    g_lkl.host_ops     =                       dlsym(RTLD_DEFAULT, "lkl_host_ops");

    g_lkl.available = (g_lkl.start_kernel != NULL && g_lkl.host_ops != NULL);

    if (g_lkl.available) {
        snprintf(g_lkl.status_buf, sizeof(g_lkl.status_buf),
                 "AVAILABLE\n"
                 "  lkl_start_kernel = %p\n"
                 "  lkl_host_ops     = %p\n"
                 "  lkl_sys_halt     = %p\n"
                 "  lkl_sys_open     = %p\n"
                 "  lkl_sys_read     = %p",
                 (void*)g_lkl.start_kernel, g_lkl.host_ops,
                 (void*)g_lkl.sys_halt,
                 (void*)g_lkl.sys_open, (void*)g_lkl.sys_read);
        LOGI("LKL szimbólumok feloldva");
    } else {
        snprintf(g_lkl.status_buf, sizeof(g_lkl.status_buf),
                 "UNAVAILABLE — nincs liblkl-host-lib.so vagy hiányos.\n"
                 "Erre lesz szükség a Phase 2c.2-höz: NDK toolchainnel\n"
                 "fordított LKL library az `app/src/main/jniLibs/arm64-v8a/`\n"
                 "alá. Részletek: projects/kernel-build/STATUS.md.");
        LOGW("LKL szimbólumok nem találhatóak (nincs .so)");
    }
}

/* ── JNI ─────────────────────────────────────────────────────────────── */

JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLklStatus(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_lkl.lock);
    lkl_resolve_locked();
    char out[1024];
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
    LOGI("lkl_start_kernel meghívás (cmd_line: mem=64M loglevel=8)");
    int rc = g_lkl.start_kernel(g_lkl.host_ops, "mem=64M loglevel=8");
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
