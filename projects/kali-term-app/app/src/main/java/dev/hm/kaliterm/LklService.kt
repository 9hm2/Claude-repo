package dev.hm.kaliterm

import android.app.Service
import android.content.Intent
import android.os.Handler
import android.os.IBinder
import android.os.Looper
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
         * Halt + self-destruct.
         *
         * 1) NativeBridge.nativeLklStop() — `lkl_sys_halt` + `lkl_cleanup`.
         * 2) stopSelf() — engedi a service-t leállítani.
         * 3) Process.killProcess(myPid) ~150 ms múlva — bízzunk hogy a
         *    Binder a halt-rc-t addig vissza tudta küldeni a kliensnek.
         *
         * A fő process onServiceDisconnected-en érzékeli a kapcsolat-szakadást.
         */
        override fun stopKernel(): Int {
            val rc = try {
                NativeBridge.nativeLklStop()
            } catch (t: Throwable) {
                Log.e(tag, "stopKernel hiba", t)
                -1
            }
            Log.i(tag, "stopKernel rc=$rc; :lkl process self-destruct 150 ms múlva")
            stopSelf()
            Handler(Looper.getMainLooper()).postDelayed({
                Log.i(tag, "Process.killProcess(myPid)")
                Process.killProcess(Process.myPid())
            }, 150)
            return rc
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
