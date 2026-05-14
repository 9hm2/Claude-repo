/*
 * kaliterm JNI bridge.
 *
 * Phase 2a: stub `nativeHello` + `nativeVersion`.
 * Phase 2b.1: `nativeAcceptUsbDevice` — fd-átvétel + fstat ellenőrzés.
 * Phase 2b.2: libusb 1.0.27 bekötve, `libusb_wrap_sys_device`-szal
 *   csomagoljuk az UsbManager-fd-t, kiolvassuk a device descriptor-t és
 *   logoljuk.
 * Phase 2b.2.1: a descriptor-info-t a logcat-en kívül **az UI-ban is**
 *   közzétesszük egy belső pufferbe írva, amit a Kotlin `nativeLastDescription`
 *   hívással kérhet le.
 * Phase 2b.3: az fd-t a usb-bridge dispatch loop-jának adjuk át egy
 *   worker-szálban, és a vhci_hcd-vel a kernelbe is bekötjük.
 */
#include <jni.h>
#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <libusb.h>

#include "bridge.h"

#define LOG_TAG "kaliterm-native"
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)

/* ── In-app diagnostic buffer ───────────────────────────────────────────
 *
 * Az `nativeAcceptUsbDevice` az aktuális futás során emberi olvasásra
 * formázott descriptor-snapshot-ot épít fel ebben a pufferben. A Kotlin
 * oldal `nativeLastDescription`-nel olvassa ki és a Compose UI-ban
 * megjeleníti — így logcat nélkül is látszik az eredmény.
 */
#define KT_DESC_BUF_SIZE  8192
static pthread_mutex_t kt_desc_mutex = PTHREAD_MUTEX_INITIALIZER;
static char            kt_desc_buf[KT_DESC_BUF_SIZE];

static void desc_clear(void)
{
    pthread_mutex_lock(&kt_desc_mutex);
    kt_desc_buf[0] = '\0';
    pthread_mutex_unlock(&kt_desc_mutex);
}

__attribute__((format(printf, 1, 2)))
static void desc_appendf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&kt_desc_mutex);
    size_t cur = strnlen(kt_desc_buf, KT_DESC_BUF_SIZE);
    if (cur + 1 < KT_DESC_BUF_SIZE) {
        vsnprintf(kt_desc_buf + cur, KT_DESC_BUF_SIZE - cur, fmt, ap);
    }
    pthread_mutex_unlock(&kt_desc_mutex);
    va_end(ap);
}

/* ── JNI ────────────────────────────────────────────────────────────── */

JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeHello(JNIEnv *env, jobject thiz)
{
    LOGI("nativeHello: bridge loaded, JNI working");
    return (*env)->NewStringUTF(env, "kaliterm native online (Phase 2b.3 — bridge embedded)");
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeVersion(JNIEnv *env, jobject thiz)
{
    /* 2 . 3 . 0  → Phase 2.3.0 */
    return 20300;
}

JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeLastDescription(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&kt_desc_mutex);
    /* NewStringUTF a hívásnál másol — a mutexen belül is biztonságos. */
    jstring out = (*env)->NewStringUTF(env, kt_desc_buf);
    pthread_mutex_unlock(&kt_desc_mutex);
    return out;
}

/*
 * Az UsbManager-től kapott fd-t libusb_wrap_sys_device-szal felcsatoljuk,
 * descriptor-t olvasunk, logoljuk és a `kt_desc_buf`-be összeszedjük az
 * UI-nak megjelenítendő riportot. Az fd-t `dup`-oljuk: a libusb_close
 * záratja a wrap-elt példányt, a Kotlin oldali eredetit nem érintjük.
 */
JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeAcceptUsbDevice(JNIEnv *env, jobject thiz,
                                                       jint fd, jint vid, jint pid,
                                                       jint busnum, jint devnum)
{
    desc_clear();

    if (fd < 0) {
        LOGE("acceptUsbDevice: érvénytelen fd=%d", fd);
        desc_appendf("HIBA: érvénytelen fd=%d\n", fd);
        return -EBADF;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        LOGE("acceptUsbDevice: fstat(%d) hiba: %s", fd, strerror(err));
        desc_appendf("HIBA: fstat(%d): %s\n", fd, strerror(err));
        return -err;
    }
    LOGI("acceptUsbDevice: fd=%d VID=%04x PID=%04x bus=%d dev=%d mode=0%o",
         fd, vid, pid, busnum, devnum, st.st_mode);
    desc_appendf("fd=%d VID=%04x PID=%04x bus=%d dev=%d (mode=0%o)\n",
                 fd, vid, pid, busnum, devnum, st.st_mode);

    int dup_fd = dup(fd);
    if (dup_fd < 0) {
        int err = errno;
        LOGE("acceptUsbDevice: dup(%d) hiba: %s", fd, strerror(err));
        desc_appendf("HIBA: dup(%d): %s\n", fd, strerror(err));
        return -err;
    }

    int rc = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (rc != LIBUSB_SUCCESS) {
        LOGW("libusb_set_option(NO_DEVICE_DISCOVERY): %s (folytatás)",
             libusb_strerror(rc));
        desc_appendf("figyelmeztetés: NO_DEVICE_DISCOVERY: %s\n",
                     libusb_strerror(rc));
    }

    libusb_context *ctx = NULL;
    rc = libusb_init(&ctx);
    if (rc < 0) {
        LOGE("libusb_init: %s", libusb_strerror(rc));
        desc_appendf("HIBA: libusb_init: %s\n", libusb_strerror(rc));
        close(dup_fd);
        return rc;
    }

    libusb_device_handle *handle = NULL;
    rc = libusb_wrap_sys_device(ctx, (intptr_t)dup_fd, &handle);
    if (rc < 0) {
        LOGE("libusb_wrap_sys_device(fd=%d): %s", dup_fd, libusb_strerror(rc));
        desc_appendf("HIBA: libusb_wrap_sys_device: %s\n", libusb_strerror(rc));
        close(dup_fd);
        libusb_exit(ctx);
        return rc;
    }
    desc_appendf("libusb_wrap_sys_device: OK\n");

    libusb_device *dev = libusb_get_device(handle);
    if (dev) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(dev, &d) == 0) {
            LOGI("libusb látja: VID=%04x PID=%04x class=%02x subclass=%02x "
                 "protocol=%02x bcdUSB=%04x bcdDevice=%04x numConfigs=%d",
                 d.idVendor, d.idProduct,
                 d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol,
                 d.bcdUSB, d.bcdDevice, d.bNumConfigurations);
            desc_appendf(
                "device descriptor:\n"
                "  idVendor       %04x\n"
                "  idProduct      %04x\n"
                "  bDeviceClass    %02x\n"
                "  bDeviceSubClass %02x\n"
                "  bDeviceProtocol %02x\n"
                "  bcdUSB         %04x\n"
                "  bcdDevice      %04x\n"
                "  numConfigs     %d\n",
                d.idVendor, d.idProduct,
                d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol,
                d.bcdUSB, d.bcdDevice, d.bNumConfigurations);
        } else {
            LOGW("libusb_get_device_descriptor: sikertelen");
            desc_appendf("figyelmeztetés: libusb_get_device_descriptor sikertelen\n");
        }

        struct libusb_config_descriptor *cfg = NULL;
        if (libusb_get_active_config_descriptor(dev, &cfg) == 0 && cfg) {
            LOGI("aktív config: bConfigurationValue=%d numInterfaces=%d",
                 cfg->bConfigurationValue, cfg->bNumInterfaces);
            desc_appendf("active config: bConfigurationValue=%d numInterfaces=%d\n",
                         cfg->bConfigurationValue, cfg->bNumInterfaces);
            for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
                for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
                    const struct libusb_interface_descriptor *id =
                        &cfg->interface[i].altsetting[a];
                    LOGI("  if[%d].alt[%d]: class=%02x subclass=%02x protocol=%02x "
                         "endpoints=%d",
                         id->bInterfaceNumber, id->bAlternateSetting,
                         id->bInterfaceClass, id->bInterfaceSubClass,
                         id->bInterfaceProtocol, id->bNumEndpoints);
                    desc_appendf(
                        "  if[%d].alt[%d]: class=%02x subclass=%02x protocol=%02x eps=%d\n",
                        id->bInterfaceNumber, id->bAlternateSetting,
                        id->bInterfaceClass, id->bInterfaceSubClass,
                        id->bInterfaceProtocol, id->bNumEndpoints);
                    for (uint8_t e = 0; e < id->bNumEndpoints; e++) {
                        const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                        const char *typ =
                            (ep->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_CONTROL     ? "CTRL" :
                            (ep->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS ? "ISO"  :
                            (ep->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK        ? "BULK" :
                            (ep->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_INTERRUPT   ? "INT"  : "?";
                        const char *dir = (ep->bEndpointAddress & 0x80) ? "IN " : "OUT";
                        desc_appendf("    ep 0x%02x  %s %s  maxPkt=%u\n",
                                     ep->bEndpointAddress, dir, typ,
                                     ep->wMaxPacketSize);
                    }
                }
            }
            libusb_free_config_descriptor(cfg);
        } else {
            desc_appendf("figyelmeztetés: nincs aktív config descriptor\n");
        }
    }

    libusb_close(handle);   /* lezárja a dup_fd-t */
    libusb_exit(ctx);
    desc_appendf("DONE.\n");
    return 0;
}

/* ── Bridge runtime state (Phase 2b.3) ───────────────────────────────── */

static struct {
    pthread_mutex_t   lock;
    int               running;
    pthread_t         thread;
    bridge_ctx_t      ctx;
    bridge_session_t  session;
    int               peer_sock;       /* az LKL-felé eső socketpair-vég */
    uint16_t          vid, pid;
} g_bridge = {
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .running   = 0,
    .peer_sock = -1,
};

static void *bridge_worker_main(void *arg)
{
    bridge_session_t *s = (bridge_session_t *)arg;
    LOGI("bridge worker spawn: socket_fd=%d", s->socket_fd);
    int rc = bridge_session_serve(s);
    LOGI("bridge worker exit: rc=%d", rc);
    return NULL;
}

/*
 * Beindítja a usb-bridge URB-dispatch loop-ját a kapott UsbManager-fd-re.
 * Egy `socketpair`-t hoz létre: az egyik vég a worker-szálba megy
 * (`session.socket_fd` — innen olvas USB/IP PDU-kat), a másik vég
 * `g_bridge.peer_sock`-ban marad, és a Phase 2c-ben jön majd ide a
 * vhci_hcd. Most még csak nyitva tartjuk, hogy a worker tudjon várni.
 *
 * Egyszerre **egy** bridge-példány fut — ha már fut, -EBUSY.
 */
JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeStartBridge(JNIEnv *env, jobject thiz,
                                                   jint fd, jint vid, jint pid,
                                                   jint busnum, jint devnum)
{
    pthread_mutex_lock(&g_bridge.lock);
    if (g_bridge.running) {
        pthread_mutex_unlock(&g_bridge.lock);
        LOGW("startBridge: már fut egy bridge — előbb stop kell");
        return -EBUSY;
    }

    if (fd < 0) {
        pthread_mutex_unlock(&g_bridge.lock);
        return -EBADF;
    }

    int dup_fd = dup(fd);
    if (dup_fd < 0) {
        int err = errno;
        pthread_mutex_unlock(&g_bridge.lock);
        LOGE("startBridge: dup hiba: %s", strerror(err));
        return -err;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        int err = errno;
        close(dup_fd);
        pthread_mutex_unlock(&g_bridge.lock);
        LOGE("startBridge: socketpair hiba: %s", strerror(err));
        return -err;
    }

    /* libusb context — Android-friendly opciók kötelezően előbb. */
    int rc = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (rc != LIBUSB_SUCCESS) {
        LOGW("startBridge: NO_DEVICE_DISCOVERY: %s (folytatás)",
             libusb_strerror(rc));
    }
    libusb_context *uctx = NULL;
    rc = libusb_init(&uctx);
    if (rc < 0) {
        close(dup_fd); close(sv[0]); close(sv[1]);
        pthread_mutex_unlock(&g_bridge.lock);
        LOGE("startBridge: libusb_init: %s", libusb_strerror(rc));
        return rc;
    }

    libusb_device_handle *handle = NULL;
    rc = libusb_wrap_sys_device(uctx, (intptr_t)dup_fd, &handle);
    if (rc < 0) {
        libusb_exit(uctx);
        close(dup_fd); close(sv[0]); close(sv[1]);
        pthread_mutex_unlock(&g_bridge.lock);
        LOGE("startBridge: libusb_wrap_sys_device: %s", libusb_strerror(rc));
        return rc;
    }

    /* bridge_ctx_t + bridge_session_t feltöltés. */
    memset(&g_bridge.ctx, 0, sizeof(g_bridge.ctx));
    g_bridge.ctx.usb_ctx     = uctx;
    g_bridge.ctx.socket_path = NULL;
    g_bridge.ctx.listen_fd   = -1;

    memset(&g_bridge.session, 0, sizeof(g_bridge.session));
    g_bridge.session.ctx        = &g_bridge.ctx;
    g_bridge.session.socket_fd  = sv[1];    /* worker olvas/ír itt */
    g_bridge.session.usb_fd     = -1;       /* már wrap-elve van */
    g_bridge.session.usb_handle = handle;
    g_bridge.session.devid      = ((uint32_t)busnum << 16) | (uint32_t)devnum;

    g_bridge.peer_sock = sv[0];             /* mi tartjuk ezt a véget */
    g_bridge.vid = (uint16_t)vid;
    g_bridge.pid = (uint16_t)pid;

    int prc = pthread_create(&g_bridge.thread, NULL,
                             bridge_worker_main, &g_bridge.session);
    if (prc != 0) {
        libusb_close(handle);
        libusb_exit(uctx);
        close(sv[0]); close(sv[1]);
        pthread_mutex_unlock(&g_bridge.lock);
        LOGE("startBridge: pthread_create: %s", strerror(prc));
        return -prc;
    }

    g_bridge.running = 1;
    pthread_mutex_unlock(&g_bridge.lock);

    desc_appendf("BRIDGE STARTED — peer_sock=%d worker tid spawned\n", sv[0]);
    LOGI("bridge started: peer_sock=%d worker_fd=%d devid=%08x",
         sv[0], sv[1], g_bridge.session.devid);
    return 0;
}

/*
 * Lezárja a futó bridge-példányt:
 *  1) A peer_sock close-jával EOF-ot generálunk a worker socket_fd-jén,
 *     ezért bridge_session_serve loopja kilép.
 *  2) pthread_join-nal megvárjuk.
 *  3) bridge_session_close zárja a worker-fd-t és a libusb handle-t,
 *     végül libusb_exit a kontextusra.
 */
JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeStopBridge(JNIEnv *env, jobject thiz)
{
    pthread_mutex_lock(&g_bridge.lock);
    if (!g_bridge.running) {
        pthread_mutex_unlock(&g_bridge.lock);
        return 0;
    }

    if (g_bridge.peer_sock >= 0) {
        shutdown(g_bridge.peer_sock, SHUT_RDWR);
        close(g_bridge.peer_sock);
        g_bridge.peer_sock = -1;
    }
    pthread_t t = g_bridge.thread;
    pthread_mutex_unlock(&g_bridge.lock);

    pthread_join(t, NULL);

    pthread_mutex_lock(&g_bridge.lock);
    bridge_session_close(&g_bridge.session);
    if (g_bridge.ctx.usb_ctx) {
        libusb_exit(g_bridge.ctx.usb_ctx);
        g_bridge.ctx.usb_ctx = NULL;
    }
    g_bridge.running = 0;
    pthread_mutex_unlock(&g_bridge.lock);

    LOGI("bridge stopped cleanly");
    return 0;
}

/*
 * Egyszerű állapot-lekérdezés a UI-nak.
 * "STOPPED" — nincs bridge.
 * "RUNNING vid=… pid=… peer_sock=…" — fut, ezzel a VID/PID-del.
 */
JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeBridgeStatus(JNIEnv *env, jobject thiz)
{
    char buf[128];
    pthread_mutex_lock(&g_bridge.lock);
    if (!g_bridge.running) {
        snprintf(buf, sizeof(buf), "STOPPED");
    } else {
        snprintf(buf, sizeof(buf),
                 "RUNNING vid=%04x pid=%04x devid=%08x peer_sock=%d",
                 g_bridge.vid, g_bridge.pid,
                 g_bridge.session.devid, g_bridge.peer_sock);
    }
    pthread_mutex_unlock(&g_bridge.lock);
    return (*env)->NewStringUTF(env, buf);
}
