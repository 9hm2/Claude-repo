package dev.hm.kaliterm

/**
 * A natív (C) réteg felé vezető bridge.
 *
 * Phase 2a: ellenőrző stubok (nativeHello, nativeVersion).
 * Phase 2b.1: `nativeAcceptUsbDevice` egy UsbManager-fd-t fogad — egyelőre
 *   csak logol, hogy a Kotlin → JNI → ARM64 C natív rétegig megérkezik az
 *   fd, és tartalma érvényes file descriptor.
 * Phase 2b.2: az fd-t libusb_wrap_sys_device-szal a usb-bridge dispatch
 *   loop-jának adjuk át.
 */
object NativeBridge {

    init {
        System.loadLibrary("kaliterm_native")
    }

    /** Egyszerű ellenőrző hívás — visszaad egy diagnosztikai stringet. */
    external fun nativeHello(): String

    /** Build/phase azonosító — a natív rétegben kódolva. */
    external fun nativeVersion(): Int

    /**
     * Átveszi az UsbManager-től kapott file descriptor-t.
     *
     * @param fd     az UsbDeviceConnection.fileDescriptor értéke
     * @param vid    USB vendor ID (info / log célra)
     * @param pid    USB product ID
     * @param busnum / devnum  Android UsbDevice.deviceId felső/alsó 16 bit
     * @return 0 ha az átvétel sikeres, negatív errno ha nem
     */
    external fun nativeAcceptUsbDevice(
        fd: Int, vid: Int, pid: Int, busnum: Int, devnum: Int,
    ): Int

    /**
     * Az utolsó `nativeAcceptUsbDevice` futás emberi-olvasásra formázott
     * diagnosztikai szövege (libusb device + config descriptor dump,
     * vagy hibaüzenet). UI-ban megjelenítve logcat nélkül is látszik
     * az eredmény.
     */
    external fun nativeLastDescription(): String

    /**
     * Beindítja a usb-bridge URB-dispatch worker-szálát erre az fd-re.
     * Egyszerre csak egy bridge futhat — ha már fut, `-EBUSY`-t ad.
     * A worker `socketpair` egyik végén vár USB/IP PDU-kra; ezt a
     * másik végét a JNI nyitva tartja, és Phase 2c-ben adjuk át a
     * vhci_hcd-nek (LKL `usbip_sockfd_store`).
     */
    external fun nativeStartBridge(
        fd: Int, vid: Int, pid: Int, busnum: Int, devnum: Int,
    ): Int

    /** Tisztán leállítja a futó bridge-példányt. Idempotens. */
    external fun nativeStopBridge(): Int

    /**
     * Pillanatnyi bridge-állapot:
     *   "STOPPED"
     *   "RUNNING vid=… pid=… devid=… peer_sock=…"
     */
    external fun nativeBridgeStatus(): String
}
