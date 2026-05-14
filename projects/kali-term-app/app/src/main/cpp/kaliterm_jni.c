/*
 * kaliterm JNI bridge.
 *
 * Phase 2a: stub `nativeHello` + `nativeVersion`.
 * Phase 2b.1: `nativeAcceptUsbDevice` — fd-átvétel + fstat ellenőrzés.
 * Phase 2b.2: libusb 1.0.27 bekötve, `libusb_wrap_sys_device`-szal
 *   csomagoljuk az UsbManager-fd-t, kiolvassuk a device descriptor-t és
 *   logoljuk. Ez bizonyítja hogy a libusb a beágyazott állapotban tudja
 *   használni az Android USB stack-ből származó fd-t — root nélkül.
 * Phase 2b.3: az fd-t a usb-bridge dispatch loop-jának adjuk át egy
 *   worker-szálban, és a vhci_hcd-vel a kernelbe is bekötjük.
 */
#include <jni.h>
#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libusb.h>

#define LOG_TAG "kaliterm-native"
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)

JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeHello(JNIEnv *env, jobject thiz)
{
    LOGI("nativeHello: bridge loaded, JNI working");
    return (*env)->NewStringUTF(env, "kaliterm native online (Phase 2b.2 — libusb wrap)");
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeVersion(JNIEnv *env, jobject thiz)
{
    /* 2 . 2 . 2  →  Phase 2.2.2  */
    return 20202;
}

/*
 * Az UsbManager-től kapott fd-t libusb_wrap_sys_device-szal felcsatoljuk,
 * descriptor-t olvasunk, logolunk, majd elengedjük.
 *
 * Fontos: az fd-t `dup`-oljuk, mielőtt libusb_wrap_sys_device-nak adjuk —
 * a libusb_close belsőleg close()-olja az ott látott fd-t, és nem akarjuk
 * hogy a Kotlin oldali UsbDeviceConnection.close() később egy lezárt
 * fd-re fusson rá.
 */
JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeAcceptUsbDevice(JNIEnv *env, jobject thiz,
                                                       jint fd, jint vid, jint pid,
                                                       jint busnum, jint devnum)
{
    if (fd < 0) {
        LOGE("acceptUsbDevice: érvénytelen fd=%d", fd);
        return -EBADF;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        LOGE("acceptUsbDevice: fstat(%d) hiba: %s", fd, strerror(err));
        return -err;
    }
    LOGI("acceptUsbDevice: fd=%d VID=%04x PID=%04x bus=%d dev=%d mode=0%o",
         fd, vid, pid, busnum, devnum, st.st_mode);

    /* dup: libusb_close-szal a wrap-elt fd-t záratjuk; a Java oldali
     * UsbDeviceConnection.close() az eredetit fogja zárni. */
    int dup_fd = dup(fd);
    if (dup_fd < 0) {
        int err = errno;
        LOGE("acceptUsbDevice: dup(%d) hiba: %s", fd, strerror(err));
        return -err;
    }

    /* Android-friendly libusb opciók — `libusb_init` előtt kell. */
    int rc = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (rc != LIBUSB_SUCCESS) {
        LOGW("libusb_set_option(NO_DEVICE_DISCOVERY): %s (folytatás)",
             libusb_strerror(rc));
    }

    libusb_context *ctx = NULL;
    rc = libusb_init(&ctx);
    if (rc < 0) {
        LOGE("libusb_init: %s", libusb_strerror(rc));
        close(dup_fd);
        return rc;
    }

    libusb_device_handle *handle = NULL;
    rc = libusb_wrap_sys_device(ctx, (intptr_t)dup_fd, &handle);
    if (rc < 0) {
        LOGE("libusb_wrap_sys_device(fd=%d): %s", dup_fd, libusb_strerror(rc));
        close(dup_fd);
        libusb_exit(ctx);
        return rc;
    }

    /* Descriptor probe — itt már a libusb usbfs backend olvas. */
    libusb_device *dev = libusb_get_device(handle);
    if (dev) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(dev, &d) == 0) {
            LOGI("libusb látja: VID=%04x PID=%04x class=%02x subclass=%02x "
                 "protocol=%02x bcdUSB=%04x bcdDevice=%04x numConfigs=%d",
                 d.idVendor, d.idProduct,
                 d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol,
                 d.bcdUSB, d.bcdDevice, d.bNumConfigurations);
        } else {
            LOGW("libusb_get_device_descriptor: sikertelen");
        }

        struct libusb_config_descriptor *cfg = NULL;
        if (libusb_get_active_config_descriptor(dev, &cfg) == 0 && cfg) {
            LOGI("aktív config: bConfigurationValue=%d numInterfaces=%d",
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
                }
            }
            libusb_free_config_descriptor(cfg);
        }
    }

    libusb_close(handle);   /* lezárja a dup_fd-t */
    libusb_exit(ctx);
    return 0;
}
