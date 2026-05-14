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

**Fázis 1b — alap socket + libusb wrap:** ✅
- Unix socket szerver (listen / accept).
- SCM_RIGHTS-szal érkező USB fd fogadás.
- `libusb_wrap_sys_device` megnyitás, descriptor probe, logolás.

**Fázis 1c — USB/IP wire loop (jelen állapot):** ✅
- CMD_SUBMIT pdu → libusb async transfer:
  - endpoint 0 → `libusb_fill_control_transfer` (8 bájt setup csonk + adat)
  - bulk endpoint → `libusb_fill_bulk_transfer`
  - interrupt endpoint → `libusb_fill_interrupt_transfer`
  - isochronous → `-EOPNOTSUPP` (Fázis 2 témája)
- Endpoint típus cache: a `libusb_get_active_config_descriptor`-ból a
  `bmAttributes`-t indexeljük a `(addr & 0x0F) | (dir<<4)` táblába.
- Interface claim minden interface-re a session indulásakor
  (`libusb_detach_kernel_driver` best-effort, majd `libusb_claim_interface`).
- Completion callback → RET_SUBMIT a vhci_hcd felé. IN-payload a status
  után. Kontroll transfer esetén a setup-csonkot levágjuk a válaszról.
- CMD_UNLINK → `libusb_cancel_transfer`, RET_UNLINK `-ECONNRESET`-tel
  (vagy 0-val ha már befejeződött). A cancelt URB callbackje
  RET_SUBMIT-tel `-ECONNRESET` státusszal megy.
- Egy dedikált event-thread polloz `libusb_handle_events_timeout`-tal,
  hogy a completion-ök elérjenek. A socket írás `pthread_mutex`-szel
  sorosítva — több párhuzamos transfer sem keveri össze a PDU-kat.
- Shutdown: a read-loop kilépésekor minden pending transfert cancellünk,
  az event-thread join-olódik, interface-ek release-elve, dispatch állapot
  felszabadítva.

**Korlátok / TODO:**
- Iso transfer: most `-EOPNOTSUPP`. Iso packet descriptor-ok parse/serialize
  szabványos USB/IP-ben, megéri majd implementálni a UVC/audio kedvéért.
- USB/IP attach-handshake (`OP_REQ_IMPORT` stb.): jelenleg feltételezzük,
  hogy a kliens (Android-side) a vhci_hcd-vel közvetlenül egy
  pre-attach-elt socket-et használ — a handshake-et az Android oldalon
  vagy egy attach-segéd lokálisan futtatja.
- Egyidejűleg max **256 in-flight URB / eszköz** (`MAX_PENDING`).
- URB-enként max **256 KiB** payload (`MAX_XFER_BYTES`).

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
  bridge.h           — közös típusok (bridge_ctx_t, bridge_session_t, dispatch fwd)
  socket_server.c    — Unix socket bind/listen, accept, SCM_RIGHTS fd-recv
  usbip_proto.h      — USB/IP wire protokoll headerek + I/O helper API
  usbip_proto.c      — PDU read/write + ntoh/hton konverzió
  usb_dispatch.c     — libusb_wrap_sys_device, transfer table, event thread,
                       CMD_SUBMIT / CMD_UNLINK / RET_SUBMIT / RET_UNLINK
Makefile             — build rendszer (pkg-config libusb-1.0, -pthread)
```

## Futtatás

```bash
./out/kaliterm-usb-bridge --socket=/tmp/kaliterm-usb.sock --verbose
```

A daemon `SIGTERM`-re tisztán kilép és levesz a socket fájlt.
