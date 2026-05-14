/*
 * kaliterm JNI bridge.
 *
 * Phase 2a: stub `nativeHello` + `nativeVersion`.
 * Phase 2b.1: `nativeAcceptUsbDevice` — fd-átvétel + diagnosztikai logolás.
 *   fstat-tal ellenőrizzük, hogy a beérkező fd valódi karakter-eszköz
 *   (`/dev/bus/usb/...`-szerű).
 * Phase 2b.2: libusb_wrap_sys_device + usb-bridge dispatch indítása.
 */
#include <jni.h>
#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "kaliterm-native"
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)

JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeHello(JNIEnv *env, jobject thiz)
{
    LOGI("nativeHello: bridge loaded, JNI working");
    return (*env)->NewStringUTF(env, "kaliterm native online (Phase 2b.1)");
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeVersion(JNIEnv *env, jobject thiz)
{
    /* 2 . 2 . 1  →  Phase 2.2.1  */
    return 20201;
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeAcceptUsbDevice(JNIEnv *env, jobject thiz,
                                                       jint fd, jint vid, jint pid,
                                                       jint busnum, jint devnum)
{
    if (fd < 0) {
        LOGE("acceptUsbDevice: érvénytelen fd=%d", fd);
        return -EBADF;
    }

    /* Az UsbManager-ből kapott fd egy nyitott karakter-eszközre mutat
     * (Android usbfs). fstat-tal ellenőrizzük. */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        LOGE("acceptUsbDevice: fstat(%d) hiba: %s", fd, strerror(err));
        return -err;
    }
    if (!S_ISCHR(st.st_mode)) {
        LOGW("acceptUsbDevice: fd=%d nem karaktereszköz (mode=0%o)", fd, st.st_mode);
        /* nem fatális — egyes Android verziók más típust adnak; folytatjuk */
    }

    LOGI("acceptUsbDevice: fd=%d VID=%04x PID=%04x bus=%d dev=%d mode=0%o",
         fd, vid, pid, busnum, devnum, st.st_mode);

    /* Phase 2b.2-ben itt megy tovább a bridge dispatch loop felé.
     * Most a fd-t bezárjuk (a Kotlin oldali UsbDeviceConnection-t úgyis
     * elejti az GC), nehogy fd-leak legyen futás közben. */
    if (close(fd) < 0) {
        LOGW("acceptUsbDevice: close(%d) hiba: %s", fd, strerror(errno));
    }
    return 0;
}
