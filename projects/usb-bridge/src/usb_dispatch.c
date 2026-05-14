/*
 * USB-oldali kezelés a kaliterm USB bridge-ben.
 *
 * Az Android UsbManager-től kapott file descriptor-t libusb_wrap_sys_device()
 * köré csomagoljuk, és a vhci_hcd-től érkező USB/IP CMD_SUBMIT pdu-kat
 * libusb async transfer-ekké fordítjuk. A válasz pdu-kat RET_SUBMIT
 * formában visszaküldjük a socket-en.
 *
 * Fázis 1b — skeleton: most még csak megnyitjuk az eszközt, kiolvassuk
 * a device descriptor-t, logoljuk, és bezárunk. Az URB-loop a következő
 * commit-tal jön.
 */
#define _GNU_SOURCE
#include "bridge.h"
#include "usbip_proto.h"

#include <errno.h>
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int bridge_session_open_usb(bridge_session_t *s)
{
    if (!s || !s->ctx || s->usb_fd < 0) return -EINVAL;

    /*
     * libusb_wrap_sys_device a kulcs trükk root-mentes Android USB-hez:
     * az UsbManager-től kapott raw fd-t fogadjuk el, libusb regisztrálja
     * mintha saját ENUMER-rel találta volna meg.
     *
     * Megjegyzés: az fd ownership a libusb-é lesz; mi nem zárjuk be
     * külön — bridge_session_close() libusb_close() hív.
     */
    int rc = libusb_wrap_sys_device(s->ctx->usb_ctx,
                                    (intptr_t)s->usb_fd,
                                    &s->usb_handle);
    if (rc < 0) {
        fprintf(stderr, "[bridge] libusb_wrap_sys_device: %s\n",
                libusb_strerror(rc));
        return rc;
    }

    /* Diagnosztika: kiolvassuk a device descriptor-t. */
    struct libusb_device *dev = libusb_get_device(s->usb_handle);
    if (dev) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(dev, &d) == 0) {
            fprintf(stderr,
                    "[bridge] eszköz felcsatolva: VID=%04x PID=%04x "
                    "class=%02x subclass=%02x\n",
                    d.idVendor, d.idProduct,
                    d.bDeviceClass, d.bDeviceSubClass);
        }

        uint8_t bus  = libusb_get_bus_number(dev);
        uint8_t addr = libusb_get_device_address(dev);
        s->devid = ((uint32_t)bus << 16) | addr;
    }

    return 0;
}

/*
 * Az USB/IP wire loop helye. Fázis 1c-ben ide kerül:
 *   - struct usbip_header beolvasása, ntohl()
 *   - CMD_SUBMIT → libusb_submit_transfer (control/bulk/interrupt/iso)
 *   - completion callback → RET_SUBMIT serialize + write
 *   - CMD_UNLINK → libusb_cancel_transfer
 *
 * Most csak skeleton — a kliens-bontás után visszatérünk.
 */
int bridge_session_serve(bridge_session_t *s)
{
    fprintf(stderr, "[bridge] session-serve skeleton: a USB/IP loop még TODO\n");
    (void)s;
    return 0;
}

void bridge_session_close(bridge_session_t *s)
{
    if (!s) return;
    if (s->usb_handle) {
        libusb_close(s->usb_handle);
        s->usb_handle = NULL;
    }
    if (s->socket_fd >= 0) {
        close(s->socket_fd);
        s->socket_fd = -1;
    }
    /* usb_fd lifetime a libusb-é volt — már elengedve. */
    s->usb_fd = -1;
}
