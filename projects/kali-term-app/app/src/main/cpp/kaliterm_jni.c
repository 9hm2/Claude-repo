/*
 * kaliterm JNI bridge — Phase 2a stub.
 *
 * A `nativeHello` egy egyszerű proof-of-life: a Kotlin oldal meghívja,
 * mi pedig visszaadunk egy stringet. Ezzel ellenőrizzük hogy az NDK
 * toolchain, a libname feloldása, és a JNI-name-mangling rendben van.
 *
 * A következő phase-ben kerül ide:
 *   - LKL .so dlopen + lkl_start_kernel
 *   - usb-bridge sources beégetve (vagy spawn-olt processz)
 *   - socketpair management vhci_hcd ↔ bridge között
 *   - UsbManager fd átvétele a Kotlin oldalról
 */
#include <jni.h>
#include <android/log.h>

#define LOG_TAG "kaliterm-native"
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)

JNIEXPORT jstring JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeHello(JNIEnv *env, jobject thiz)
{
    LOGI("nativeHello: bridge loaded, JNI working");
    return (*env)->NewStringUTF(env, "kaliterm native online (Phase 2a)");
}

JNIEXPORT jint JNICALL
Java_dev_hm_kaliterm_NativeBridge_nativeVersion(JNIEnv *env, jobject thiz)
{
    /* Phase-szám / build-azonosító — most még csak konstans. */
    return 20100;  /* 2 . 1 . 0 (== Phase 2.1, build 0) */
}
