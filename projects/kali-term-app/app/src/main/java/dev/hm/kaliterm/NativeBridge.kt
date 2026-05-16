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
        // liblkl.so betöltését megkíséreljük; ha hiányzik a jniLibs-ből
        // (pl. a CI artifact-letöltése sikertelen volt), nem dobunk fatal-t —
        // a Phase 2c JNI a `dlopen` paton kezeli a fallback-et.
        runCatching { System.loadLibrary("lkl") }
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

    /* ── Phase 2c — LKL runtime ─────────────────────────────────────── */

    /**
     * Az LKL .so jelenléte és resolve-állapota (`lkl_start_kernel`,
     * `lkl_host_ops`, stb. dlsym-mel feloldva).
     */
    external fun nativeLklStatus(): String

    /**
     * Phase 2c.5d — a futó LKL kernel `/proc/sys/kernel/osrelease`-jét adja
     * vissza (pl. "5.18.0"). Üres string ha az LKL nem fut. A proot
     * launch.sh-t ezzel az értékkel hívjuk `--kernel-release` arg-szal,
     * hogy a chrooted `uname -r` az LKL-szerű verziót mutassa, NE az
     * Android-host kernel verzióját.
     */
    external fun nativeLklKernelRelease(): String

    /**
     * Phase 2c.5e — általános read-only fájl-olvasás az LKL fájlrendszeréből
     * (`/proc/version`, `/proc/cpuinfo`, `/sys/...`). Maximum 64 KB. Üres
     * string ha a kernel nem fut vagy a fájl nem létezik.
     *
     * A `LklService` ezt expozálja Binder-en át a fő process-nek
     * (`ILklService.readLklFile`), és a `RootfsManager` ezeket a tartalmakat
     * fájlokba menti hogy a proot launch.sh bind-mountolhassa a chrooted
     * `/proc` helyettesítőjeként.
     */
    external fun nativeLklReadFile(path: String): String

    /**
     * Phase 2c.5f — egy LKL-belső könyvtár entry-listája (getdents64-szel).
     * Newline-separated nevek, "." és ".." nélkül. Üres string ha a kernel
     * nem fut vagy a path nem létezik. A KaliShellService a /dev mirror-hez
     * használja: az LKL `/dev`-jét listázza és host-fájl-bind-okkal a chroot
     * /dev-jébe pakolja a fontosabb device-okat.
     */
    external fun nativeLklListDir(path: String): String

    /**
     * `lkl_start_kernel(lkl_host_ops, "mem=64M loglevel=8")` meghívása.
     * Visszaadás: 0 = ok, `-ENOENT` = nincs LKL .so, `-EALREADY` = már fut.
     */
    external fun nativeLklStart(): Int

    /** `lkl_sys_halt()` — tisztán leállítja a futó LKL kernelt. */
    external fun nativeLklStop(): Int

    /**
     * Phase 2c.5b/c — a `:lkl` process-ben fut, a Binder-en érkezett
     * USB fd-t libusb_wrap_sys_device-szal megnyitja, descriptor-t olvas,
     * és ha az LKL kernel él, vhci_hcd attach kísérletet tesz a sysfs-en át.
     *
     * Visszaad: diagnosztikai szöveg (minden lépés rc-je + descriptor).
     */
    external fun nativeLklAttachUsbDevice(
        fd: Int, vid: Int, pid: Int, busnum: Int, devnum: Int,
    ): String
}
