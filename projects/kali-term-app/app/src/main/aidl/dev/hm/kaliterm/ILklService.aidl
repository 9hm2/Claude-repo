// SPDX-License-Identifier: GPL-2.0
//
// LKL Service IPC interface — a `:lkl` process-ben futó LklService és a
// fő (UI) process közötti Binder-átjáró. AIDL → AGP auto-generál Stub
// (service oldal) és Proxy (kliens oldal) Java osztályokat, így a hívások
// szinkron metódusoknak látszanak a kliens oldalon, miközben a Binder
// kernel-IPC viszi át a process-határt.
package dev.hm.kaliterm;

interface ILklService {
    /** A natív rétegtől visszakapott állapot-szöveg (dlsym addresses + running). */
    String getStatus();

    /** lkl_init(host_ops) + lkl_start_kernel("mem=64M loglevel=8").
     *  Visszaad: 0 = OK, negatív errno hiba esetén. */
    int startKernel();

    /** lkl_sys_halt + lkl_cleanup, majd a `:lkl` process öli magát.
     *  Visszaad: a halt rc-je (a kill előtt). A kliens onServiceDisconnected-en
     *  veszi észre hogy a service process eltűnt. */
    int stopKernel();

    /**
     * USB eszköz fd átadása a `:lkl` process-be:
     * 1. dup → libusb_wrap_sys_device (root nélkül), descriptor probe
     * 2. ha az LKL kernel fut: sysfs alatt vhci_hcd attach kísérlet
     *    (`/sys/devices/platform/vhci_hcd.0/attach`)
     *
     * Visszaad: diagnosztikai szöveg minden lépésről (dup, wrap, descriptor,
     * attach rc-k) — a UI ezt mutatja egy `Másol`-ható dobozban.
     */
    String attachUsbDevice(in ParcelFileDescriptor fd,
                           int vid, int pid, int busnum, int devnum);

    /**
     * Read-only fájl-olvasás a futó LKL kernel fájlrendszeréből.
     * Pl. `/proc/version`, `/proc/sys/kernel/osrelease`, `/proc/cpuinfo`,
     * `/proc/meminfo`. Üres stringgel tér vissza ha a kernel nem fut, vagy
     * a fájl nem létezik / hibásan olvasható.
     *
     * Phase 2c.5e: ezeket a fájlokat a launch.sh bind-mountolja a chrooted
     * /proc helyettesítőjeként — így a chrooted Kali bash a `uname -r`,
     * `cat /proc/cpuinfo` stb. az LKL kernelből kap választ, NEM az
     * Android-host kernelből.
     */
    String readLklFile(String path);

    /** Egy LKL-belső könyvtár tartalmát adja vissza newline-separated stringként
     *  ("." és ".." nélkül). Üres string ha a kernel nem fut vagy a path nem
     *  létezik. A KaliShellService a `/dev`-listinghez használja, hogy a
     *  chrooted /dev az LKL kernel device-fáját mutassa. */
    String listLklDir(String path);

    /** Phase 2c.5g — control-socket szerver indítása a `:lkl` process-en
     *  egy unix-domain-socket-en. A chrooted libkali_fuse_shim.so connectel
     *  ide az LKL FS valódi (élő) eléréséhez. Idempotens.
     *  @return 0 ha OK, negatív errno hiba esetén */
    int startLklControlSocket(String path);

    /**
     * Phase 3 — proot+bash spawn a `:lkl` process-ben. forkpty + execve.
     * A `:lkl` megtartja a PTY-master fd-t (process-state); a hívó (main)
     * a ParcelFileDescriptor-on át dup-ot kap. Ha main meghal Samsung-BBA
     * miatt, a shell tovább él, és a new main re-kötheti.
     *
     * Idempotens: ha már van futó shell, a meglévő master fd-t adja vissza
     * (új PFD dup-ja).
     *
     * @return PFD a PTY master fd-vel, vagy null hiba esetén.
     */
    ParcelFileDescriptor startKaliShell(String shellPath, String cwd,
                                        in String[] args, in String[] env,
                                        int cols, int rows);

    /** Az aktuális (cached) PTY master fd új PFD-dup-ja, vagy null ha nincs
     *  futó shell. A new-main használja reconnect-re a meglévő bash-hez. */
    ParcelFileDescriptor getCurrentShell();

    /** PID-je az aktuálisan futó shell-nek (`:lkl` scope-ban), 0 ha nincs. */
    int getCurrentShellPid();

    /** PTY window-size resize. A child SIGWINCH-et kap. */
    void resizeShell(int cols, int rows);

    /** Aktuális shell megölése + master fd close. A reconnect-fa törlődik. */
    int killShell();
}
