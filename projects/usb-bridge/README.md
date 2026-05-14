# usb-bridge — host-oldali USB/IP server

A `kaliterm-usb-bridge` egy kicsi C daemon, ami az **Android UsbManager-től
kapott USB file descriptor-okat** köti rá az LKL kernelben futó
**`vhci_hcd` (USB/IP) driver-re**, USB/IP wire protokoll-on keresztül egy
Unix sockettel.

## Hol illeszkedik

```
Android app (UsbManager.openDevice → fd)
      │
      ▼ JNI / Unix socket + SCM_RIGHTS
┌──────────────────────────┐
│ kaliterm-usb-bridge      │  ← ez a komponens
│ - Unix socket szerver    │
│ - libusb_wrap_sys_device │
│ - USB/IP wire encoder    │
└──────────────────────────┘
      │ Unix socket (USB/IP wire)
      ▼
LKL kernel (liblkl-host-lib.so)
  └─ drivers/usb/usbip/vhci_hcd.c
       └─ drivers/usb/serial/ftdi_sio.c, hid-generic, btusb, ...
```

A `vhci_hcd` a mainline kernel része — pontosan azt csinálja, hogy egy
socket-FD-n érkező URB-eket lekezel, mintha helyi USB busz lenne. A mi
daemonunk a socket másik végén ül, és libusb-vel a fizikai eszközre
fordítja a URB-eket.

## Fejlesztési állapot

**Fázis 1b — skeleton (jelen állapot):**
- Unix socket szerver működik (listen / accept).
- SCM_RIGHTS-szal érkező USB fd fogadás működik.
- `libusb_wrap_sys_device` megnyitja az eszközt, kiolvassa a deszkriptort,
  logolja a VID/PID-et.
- **A tényleges USB/IP URB-loop még TODO** — az `bridge_session_serve()`
  ide kerül a következő commit-ban.

**Fázis 1c — USB/IP wire loop:**
- CMD_SUBMIT pdu → libusb_submit_transfer (control/bulk/interrupt/iso)
- completion → RET_SUBMIT pdu
- CMD_UNLINK → libusb_cancel_transfer
- vhci_hcd-hez attach-protokol szerinti devid kezelés

## Build

```bash
make                # host build (Linux)
make verbose=1      # parancsok megjelenítése
make clean
```

Az artifact: `out/kaliterm-usb-bridge`.

Függőség: `libusb-1.0-0-dev` (Ubuntu/Debian).

Az Android (NDK) cross-compile-t később külön logikán keresztül oldjuk meg
— a `kali-term-app` Gradle/JNI build hozza a saját toolchainjét és prebuilt
libusb-jét.

## Felépítés

```
src/
  main.c             — entry point, signalek, libusb_init, listen loop
  bridge.h           — közös típusok (bridge_ctx_t, bridge_session_t)
  socket_server.c    — Unix socket bind/listen, accept, SCM_RIGHTS fd-recv
  usbip_proto.h      — USB/IP wire protokoll headerek (kernel-ből)
  usb_dispatch.c     — libusb_wrap_sys_device + URB-loop (skeleton)
Makefile             — build rendszer (pkg-config libusb-1.0)
```

## Futtatás

```bash
./out/kaliterm-usb-bridge --socket=/tmp/kaliterm-usb.sock --verbose
```

A daemon `SIGTERM`-re tisztán kilép és levesz a socket fájlt.
