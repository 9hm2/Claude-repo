package dev.hm.kaliterm

/**
 * A natív (C) réteg felé vezető bridge.
 *
 * Phase 2a: csak ellenőrző stubok, hogy a NDK toolchain és a JNI híd
 * megvan-e.
 *
 * Phase 2b: ide kerülnek a valódi belépési pontok —
 *   - `startLkl()`: betölti a liblkl-host-lib.so-t, lkl_start_kernel-t hív
 *   - `attachUsbDevice(fd: Int)`: SCM_RIGHTS-szal továbbít az usb-bridge-nek
 *   - `attachVhci(socketFd: Int)`: a vhci_hcd usbip_sockfd_store-jára ír
 */
object NativeBridge {

    init {
        System.loadLibrary("kaliterm_native")
    }

    /** Egyszerű ellenőrző hívás — visszaad egy diagnosztikai stringet. */
    external fun nativeHello(): String

    /** Build/phase azonosító — a natív rétegben kódolva. */
    external fun nativeVersion(): Int
}
