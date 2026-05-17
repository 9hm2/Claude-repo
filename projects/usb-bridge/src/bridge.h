#ifndef KALITERM_USB_BRIDGE_H
#define KALITERM_USB_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct libusb_context;
struct libusb_device_handle;

/*
 * Egyetlen híd-kontextus per daemon-példány.
 * A daemon egy Unix socketen hallgat; minden bejövő kapcsolat egy LKL
 * `vhci_hcd` attach-pont egy konkrét USB eszközhöz. A klienst (Android
 * oldal) első üzenetében átküldi a UsbManager-től kapott file descriptor-t
 * SCM_RIGHTS-szal; ezt libusb_wrap_sys_device() köré csomagoljuk.
 */
typedef struct bridge_ctx {
    struct libusb_context *usb_ctx;
    const char           *socket_path;
    int                   listen_fd;
    volatile int          stopping;
} bridge_ctx_t;

/* A session-en belüli dispatch-állapot előre deklarálása — a részletek
 * a usb_dispatch.c-ben élnek (transfer table, mutexek, event thread). */
struct bridge_dispatch;

/*
 * Per-kapcsolat (per-eszköz) állapot. Egy munkaszál (vagy worker) tartja
 * fenn. A libusb handle a hozzá tartozó vhci-csatornán átküldött URB-eket
 * kiszolgálja.
 */
typedef struct bridge_session {
    bridge_ctx_t                  *ctx;
    int                            socket_fd;     /* USB/IP wire socket */
    int                            usb_fd;        /* az UsbManager-től kapott fd */
    struct libusb_device_handle   *usb_handle;    /* libusb_wrap_sys_device-szal */
    uint32_t                       devid;         /* USB/IP devid (busnum<<16 | devnum) */
    struct bridge_dispatch        *dispatch;      /* a session URB-loop állapota */
} bridge_session_t;

/* socket_server.c */
int  bridge_listen(bridge_ctx_t *ctx);
int  bridge_accept_loop(bridge_ctx_t *ctx);
int  bridge_recv_fd(int sock_fd, int *out_fd);
void bridge_stop(bridge_ctx_t *ctx);

/* usb_dispatch.c */
int  bridge_session_open_usb(bridge_session_t *s);
void bridge_session_close(bridge_session_t *s);
int  bridge_session_serve(bridge_session_t *s);  /* az USB/IP loop majd ide kerül */

#endif /* KALITERM_USB_BRIDGE_H */
