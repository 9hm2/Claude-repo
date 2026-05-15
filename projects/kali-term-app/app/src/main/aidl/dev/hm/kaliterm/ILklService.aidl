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
}
