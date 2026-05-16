package dev.hm.kaliterm

import android.app.Service
import android.content.Intent
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.Process
import android.util.Log

/**
 * Az LKL kernelt egy SAJÁT OS process-ben (`android:process=":lkl"`) futtatjuk.
 *
 * Indok: az LKL upstream egy-shot kernel: `lkl_sys_halt` + `lkl_cleanup` után
 * a globális kernel-state (cmd_line, percpu, init_sem, …) irreverzibilisen
 * shutdown-ben. Új `lkl_init` ugyanabban a processzben crash-eli az appot.
 *
 * Megoldás: a Service külön process-ben él. **Stop = process death** →
 * az OS minden state-et felszabadít. **Start = új process spawn-ja** →
 * Android friss memóriaképpel hozza vissza a Service-t → fresh kernel.
 *
 * IPC: AIDL (`ILklService`) — szinkron metódushívások Binder-en át.
 */
class LklService : Service() {

    private val tag = "kaliterm-lkl-svc"

    private val binder = object : ILklService.Stub() {

        override fun getStatus(): String =
            try {
                NativeBridge.nativeLklStatus()
            } catch (t: Throwable) {
                "ERROR getStatus: ${t.message}"
            }

        override fun startKernel(): Int =
            try {
                NativeBridge.nativeLklStart()
            } catch (t: Throwable) {
                Log.e(tag, "startKernel hiba", t)
                -1
            }

        /**
         * Halt + self-destruct — deadlock-rezisztens.
         *
         * Probléma: a `lkl_sys_halt` MEG TUD AKADNI, ha egy LKL kernel-thread
         * elakadt (pl. vhci_hcd egy hibás sockfd-re vár). Ha közvetlenül
         * hívnánk és blokkolunk rá, a `Process.killProcess` ütemezésig sem
         * jutunk → az egész :lkl process holtmaradna, a UI-n a Stop befagy.
         *
         * Megoldás:
         *   1) ELŐSZÖR ütemezünk egy feltétlen `killProcess`-t 300 ms múlva
         *      — ez akkor is megöli a process-t ha minden más bedöglik.
         *   2) Egy háttérszálon próbáljuk a `nativeLklStop()`-ot — ha
         *      sikerül a kill előtt, tiszta shutdown; ha nem, a kill úgy is
         *      megérkezik.
         *   3) A Binder hívás azonnal visszatér 0-val a kliensnek — nem
         *      blokkoljuk a fő process UI-szálát.
         *
         * A fő process az `onServiceDisconnected`-en érzékeli a process-halált.
         */
        override fun stopKernel(): Int {
            // 1) Feltétlen self-destruct ütemezés — ez ÉL akkor is ha minden
            //    más bedöglik (halt deadlock, JNI crash, stb.).
            Handler(Looper.getMainLooper()).postDelayed({
                Log.w(tag, "Process.killProcess(myPid) — stop deadline")
                Process.killProcess(Process.myPid())
            }, 300)
            // 2) Háttér-halt próbálkozás. Ha végez < 300 ms-ben, tiszta
            //    shutdown; ha túllóg, a fenti timer úgyis megöli a procit.
            Thread({
                try {
                    val rc = NativeBridge.nativeLklStop()
                    Log.i(tag, "nativeLklStop rc=$rc (háttér)")
                } catch (t: Throwable) {
                    Log.e(tag, "nativeLklStop hiba (háttér)", t)
                }
            }, "lkl-halt").start()
            stopSelf()
            return 0
        }

        override fun attachUsbDevice(
            pfd: ParcelFileDescriptor,
            vid: Int, pid: Int, busnum: Int, devnum: Int,
        ): String {
            // ParcelFileDescriptor: a Binder marshalling automatikusan dup-ot
            // csinált a sender oldali fd-ből, és itt a `:lkl` process saját
            // fd-jét kapjuk. A natív még egy dup-ot csinál, így a libusb
            // saját fd-t kap, a PFD-ben tartottat lezárja a PFD `use`-ja.
            return try {
                pfd.use { p ->
                    NativeBridge.nativeLklAttachUsbDevice(
                        p.fd, vid, pid, busnum, devnum)
                }
            } catch (t: Throwable) {
                Log.e(tag, "attachUsbDevice hiba", t)
                "ERROR attachUsbDevice: ${t.message}"
            }
        }

        /**
         * Read-only fájl az LKL kernel fájlrendszeréből (lkl_sys_openat/read).
         * Empty stringgel tér vissza ha a kernel nem fut, vagy a fájl nem
         * létezik. A Binder-átvitel nagyfájlokra nem alkalmas (1MB-os
         * Parcel-limit), de `/proc` és `/sys` fájlok mind <64KB.
         */
        override fun readLklFile(path: String): String =
            try {
                NativeBridge.nativeLklReadFile(path)
            } catch (t: Throwable) {
                Log.e(tag, "readLklFile($path) hiba", t)
                ""
            }
    }

    override fun onBind(intent: Intent?): IBinder {
        Log.i(tag, "onBind a :lkl process-ben (pid=${Process.myPid()})")
        return binder
    }

    override fun onDestroy() {
        Log.i(tag, "onDestroy (pid=${Process.myPid()})")
        super.onDestroy()
    }
}
