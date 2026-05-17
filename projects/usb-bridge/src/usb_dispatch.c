/*
 * USB-oldali kezelés a kaliterm USB bridge-ben — Fázis 1c.
 *
 * A vhci_hcd USB/IP wire-on CMD_SUBMIT pdu-kat küld. Mi minden CMD_SUBMIT-re
 * egy libusb async transfer-t indítunk; a completion callback RET_SUBMIT-et
 * ír vissza. CMD_UNLINK → libusb_cancel_transfer → callback CANCELLED
 * státusszal → RET_SUBMIT a kernel felé.
 *
 * Egy libusb event-feldolgozó szál polloz `libusb_handle_events_timeout`-tal.
 * A socket-írás `write_lock`-kal sorosított; több completion soha nem
 * üzenetét össze nem keveri.
 */
#define _GNU_SOURCE
#include "bridge.h"
#include "usbip_proto.h"

#include <errno.h>
#include <libusb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------- Korlátok és állapot ---------- */

#define MAX_PENDING        256        /* egyidejű URB-ek */
#define MAX_XFER_BYTES     (1 << 18)  /* 256 KiB / URB sanity-cap */
#define MAX_IFACES         16         /* USB interface count cap */
#define EP_TABLE_SIZE      32         /* 0x00..0x0F OUT, 0x80..0x8F IN */
#define EP_IDX(addr)       (((addr) & 0x0F) | (((addr) & 0x80) ? 0x10 : 0x00))

#define LIBUSB_EP_TYPE_UNKNOWN 0xff

typedef struct pending {
    int                       in_use;
    uint32_t                  seqnum;       /* eredeti CMD_SUBMIT seqnum */
    uint32_t                  devid;
    uint32_t                  direction;    /* USBIP_DIR_IN/OUT */
    uint32_t                  ep;           /* endpoint szám 0..15 */
    int                       is_control;   /* setup-csonk miatt offsetelünk */
    int32_t                   number_of_packets;
    struct libusb_transfer   *xfer;
    struct bridge_dispatch   *d;            /* visszafele a callback miatt */
} pending_t;

struct bridge_dispatch {
    bridge_session_t        *s;             /* parent session */
    pthread_mutex_t          lock;          /* a table[]-t védi */
    pthread_mutex_t          write_lock;    /* socket-írást sorosít */
    pending_t                table[MAX_PENDING];

    /* endpoint típus cache (libusb_transfer_type értékek) */
    uint8_t                  ep_type[EP_TABLE_SIZE];

    /* claim-elt interface-ek */
    int                      claimed[MAX_IFACES];
    int                      n_claimed;

    /* event thread */
    pthread_t                event_thread;
    int                      event_thread_started;
    atomic_int               stop;          /* 1 = shutdown kéréce */
};

/* ---------- libusb errno mapping (USBIP status field) ---------- */

static int32_t libusb_status_to_errno(enum libusb_transfer_status st)
{
    switch (st) {
    case LIBUSB_TRANSFER_COMPLETED:  return 0;
    case LIBUSB_TRANSFER_ERROR:      return -EPROTO;
    case LIBUSB_TRANSFER_TIMED_OUT:  return -ETIMEDOUT;
    case LIBUSB_TRANSFER_CANCELLED:  return -ECONNRESET;
    case LIBUSB_TRANSFER_STALL:      return -EPIPE;
    case LIBUSB_TRANSFER_NO_DEVICE:  return -ENODEV;
    case LIBUSB_TRANSFER_OVERFLOW:   return -EOVERFLOW;
    default:                          return -EPROTO;
    }
}

/* ---------- Endpoint cache + interface claim ---------- */

static int cache_endpoint_types(struct bridge_dispatch *d)
{
    memset(d->ep_type, LIBUSB_EP_TYPE_UNKNOWN, sizeof(d->ep_type));
    /* endpoint 0 mindig control — mindkét irány */
    d->ep_type[EP_IDX(0x00)] = LIBUSB_TRANSFER_TYPE_CONTROL;
    d->ep_type[EP_IDX(0x80)] = LIBUSB_TRANSFER_TYPE_CONTROL;

    struct libusb_device *dev = libusb_get_device(d->s->usb_handle);
    if (!dev) return -ENODEV;

    struct libusb_config_descriptor *cfg = NULL;
    int rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc < 0 || !cfg) {
        /* lehet, hogy nincs aktív config — próbálkozz az elsővel */
        rc = libusb_get_config_descriptor(dev, 0, &cfg);
        if (rc < 0 || !cfg) {
            fprintf(stderr, "[dispatch] nincs config descriptor: %s\n",
                    libusb_strerror(rc));
            return rc;
        }
    }
    for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &iface->altsetting[a];
            for (uint8_t e = 0; e < id->bNumEndpoints; e++) {
                uint8_t addr = id->endpoint[e].bEndpointAddress;
                uint8_t type = id->endpoint[e].bmAttributes & 0x03;
                d->ep_type[EP_IDX(addr)] = type;  /* control/iso/bulk/int */
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return 0;
}

static int claim_all_interfaces(struct bridge_dispatch *d)
{
    struct libusb_device *dev = libusb_get_device(d->s->usb_handle);
    if (!dev) return -ENODEV;

    struct libusb_config_descriptor *cfg = NULL;
    int rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc < 0 || !cfg) {
        rc = libusb_get_config_descriptor(dev, 0, &cfg);
        if (rc < 0 || !cfg) return rc;
    }

    for (uint8_t i = 0; i < cfg->bNumInterfaces && d->n_claimed < MAX_IFACES; i++) {
        int ino = cfg->interface[i].altsetting[0].bInterfaceNumber;
        /* Android oldal általában már elengedte a kernel driver-t (UsbManager
         * megnyitása után). Mégis: detach próba best-effort. */
        (void)libusb_detach_kernel_driver(d->s->usb_handle, ino);
        rc = libusb_claim_interface(d->s->usb_handle, ino);
        if (rc < 0) {
            fprintf(stderr, "[dispatch] claim iface %d: %s (folytatás)\n",
                    ino, libusb_strerror(rc));
            continue;
        }
        d->claimed[d->n_claimed++] = ino;
    }
    libusb_free_config_descriptor(cfg);
    return 0;
}

static void release_all_interfaces(struct bridge_dispatch *d)
{
    for (int i = 0; i < d->n_claimed; i++) {
        (void)libusb_release_interface(d->s->usb_handle, d->claimed[i]);
    }
    d->n_claimed = 0;
}

/* ---------- Pending table ---------- */

static pending_t *table_alloc(struct bridge_dispatch *d, uint32_t seqnum)
{
    pthread_mutex_lock(&d->lock);
    pending_t *slot = NULL;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!d->table[i].in_use) {
            slot = &d->table[i];
            slot->in_use = 1;
            slot->seqnum = seqnum;
            slot->d = d;
            break;
        }
    }
    pthread_mutex_unlock(&d->lock);
    return slot;
}

static void table_free(struct bridge_dispatch *d, pending_t *slot)
{
    pthread_mutex_lock(&d->lock);
    slot->in_use = 0;
    slot->seqnum = 0;
    slot->xfer = NULL;
    pthread_mutex_unlock(&d->lock);
}

static pending_t *table_find_by_seqnum(struct bridge_dispatch *d, uint32_t seqnum)
{
    pthread_mutex_lock(&d->lock);
    pending_t *found = NULL;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (d->table[i].in_use && d->table[i].seqnum == seqnum) {
            found = &d->table[i];
            break;
        }
    }
    pthread_mutex_unlock(&d->lock);
    return found;
}

/* ---------- Transfer completion ---------- */

static void LIBUSB_CALL on_transfer_complete(struct libusb_transfer *xfer)
{
    pending_t *p = (pending_t *)xfer->user_data;
    struct bridge_dispatch *d = p->d;
    bridge_session_t *s = d->s;

    int32_t status = libusb_status_to_errno(xfer->status);
    int32_t actual = xfer->actual_length;
    const void *in_data = NULL;
    size_t      in_data_len = 0;

    /* IN-payload visszaírása RET_SUBMIT után. Control transfernél az első
     * 8 bájt a setup, azt nem küldjük vissza. */
    if (p->direction == USBIP_DIR_IN && actual > 0) {
        if (p->is_control) {
            in_data     = xfer->buffer + LIBUSB_CONTROL_SETUP_SIZE;
            in_data_len = (size_t)actual;
        } else {
            in_data     = xfer->buffer;
            in_data_len = (size_t)actual;
        }
    }

    pthread_mutex_lock(&d->write_lock);
    int rc = usbip_write_ret_submit(s->socket_fd,
                                    p->seqnum, p->devid,
                                    p->direction, p->ep,
                                    status, actual,
                                    0 /* start_frame */,
                                    p->number_of_packets,
                                    0 /* error_count */,
                                    in_data, in_data_len);
    pthread_mutex_unlock(&d->write_lock);

    if (rc < 0) {
        fprintf(stderr, "[dispatch] RET_SUBMIT write hiba: %s\n", strerror(-rc));
        atomic_store(&d->stop, 1);
    }

    free(xfer->buffer);
    table_free(d, p);
    libusb_free_transfer(xfer);
}

/* ---------- Event thread ---------- */

static void *event_thread_main(void *arg)
{
    struct bridge_dispatch *d = arg;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };  /* 200 ms */

    while (!atomic_load(&d->stop)) {
        int rc = libusb_handle_events_timeout(d->s->ctx->usb_ctx, &tv);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
            fprintf(stderr, "[dispatch] libusb_handle_events: %s\n",
                    libusb_strerror(rc));
            atomic_store(&d->stop, 1);
            break;
        }
    }
    return NULL;
}

/* ---------- CMD_SUBMIT ---------- */

static int submit_one(struct bridge_dispatch *d,
                      const struct usbip_header_basic *bhdr,
                      const struct usbip_header_cmd_submit *cmd,
                      void *out_payload, size_t out_payload_len)
{
    bridge_session_t *s = d->s;
    int32_t  xlen = cmd->transfer_buffer_length;

    /* IZO most még nincs támogatva — RET_SUBMIT -EOPNOTSUPP-pal. */
    if (cmd->number_of_packets > 0) {
        pthread_mutex_lock(&d->write_lock);
        usbip_write_ret_submit(s->socket_fd,
                               bhdr->seqnum, bhdr->devid,
                               bhdr->direction, bhdr->ep,
                               -EOPNOTSUPP, 0, 0,
                               cmd->number_of_packets, 0,
                               NULL, 0);
        pthread_mutex_unlock(&d->write_lock);
        free(out_payload);
        return 0;
    }

    if (xlen < 0 || xlen > MAX_XFER_BYTES) {
        pthread_mutex_lock(&d->write_lock);
        usbip_write_ret_submit(s->socket_fd,
                               bhdr->seqnum, bhdr->devid,
                               bhdr->direction, bhdr->ep,
                               -EOVERFLOW, 0, 0, 0, 0, NULL, 0);
        pthread_mutex_unlock(&d->write_lock);
        free(out_payload);
        return 0;
    }

    pending_t *p = table_alloc(d, bhdr->seqnum);
    if (!p) {
        pthread_mutex_lock(&d->write_lock);
        usbip_write_ret_submit(s->socket_fd,
                               bhdr->seqnum, bhdr->devid,
                               bhdr->direction, bhdr->ep,
                               -ENOMEM, 0, 0, 0, 0, NULL, 0);
        pthread_mutex_unlock(&d->write_lock);
        free(out_payload);
        return 0;
    }
    p->devid             = bhdr->devid;
    p->direction         = bhdr->direction;
    p->ep                = bhdr->ep;
    p->number_of_packets = cmd->number_of_packets;
    p->is_control        = (bhdr->ep == 0);

    uint8_t ep_addr = (uint8_t)((bhdr->ep & 0x0F) |
                                (bhdr->direction == USBIP_DIR_IN ? 0x80 : 0x00));
    uint8_t ep_type = d->ep_type[EP_IDX(ep_addr)];
    if (ep_type == LIBUSB_EP_TYPE_UNKNOWN && !p->is_control) {
        table_free(d, p);
        pthread_mutex_lock(&d->write_lock);
        usbip_write_ret_submit(s->socket_fd,
                               bhdr->seqnum, bhdr->devid,
                               bhdr->direction, bhdr->ep,
                               -ENXIO, 0, 0, 0, 0, NULL, 0);
        pthread_mutex_unlock(&d->write_lock);
        free(out_payload);
        return 0;
    }

    struct libusb_transfer *xfer = libusb_alloc_transfer(0);
    if (!xfer) {
        table_free(d, p);
        free(out_payload);
        return -ENOMEM;
    }

    if (p->is_control) {
        /* Control: a setup-csonk 8 bájt + xlen adat. */
        uint8_t *buf = calloc(1, LIBUSB_CONTROL_SETUP_SIZE + (size_t)xlen);
        if (!buf) {
            libusb_free_transfer(xfer);
            table_free(d, p);
            free(out_payload);
            return -ENOMEM;
        }
        memcpy(buf, cmd->setup, 8);
        if (bhdr->direction == USBIP_DIR_OUT && out_payload_len > 0) {
            memcpy(buf + LIBUSB_CONTROL_SETUP_SIZE, out_payload, out_payload_len);
        }
        libusb_fill_control_transfer(xfer, s->usb_handle, buf,
                                     on_transfer_complete, p, 0);
    } else {
        /* Bulk/Int: csak xlen bájt. */
        uint8_t *buf = calloc(1, (size_t)xlen ? (size_t)xlen : 1);
        if (!buf) {
            libusb_free_transfer(xfer);
            table_free(d, p);
            free(out_payload);
            return -ENOMEM;
        }
        if (bhdr->direction == USBIP_DIR_OUT && out_payload_len > 0) {
            memcpy(buf, out_payload, out_payload_len);
        }
        if (ep_type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
            libusb_fill_interrupt_transfer(xfer, s->usb_handle, ep_addr,
                                           buf, xlen,
                                           on_transfer_complete, p, 0);
        } else {
            /* BULK alapból, ami a leggyakoribb. */
            libusb_fill_bulk_transfer(xfer, s->usb_handle, ep_addr,
                                      buf, xlen,
                                      on_transfer_complete, p, 0);
        }
    }

    p->xfer = xfer;
    free(out_payload);

    int rc = libusb_submit_transfer(xfer);
    if (rc < 0) {
        fprintf(stderr, "[dispatch] submit_transfer: %s\n", libusb_strerror(rc));
        free(xfer->buffer);
        libusb_free_transfer(xfer);
        table_free(d, p);
        pthread_mutex_lock(&d->write_lock);
        usbip_write_ret_submit(s->socket_fd,
                               bhdr->seqnum, bhdr->devid,
                               bhdr->direction, bhdr->ep,
                               -EIO, 0, 0, 0, 0, NULL, 0);
        pthread_mutex_unlock(&d->write_lock);
    }
    return 0;
}

/* ---------- CMD_UNLINK ---------- */

static int unlink_one(struct bridge_dispatch *d,
                      const struct usbip_header_basic *bhdr,
                      const struct usbip_header_cmd_unlink *u)
{
    pending_t *p = table_find_by_seqnum(d, u->seqnum);
    int32_t status = 0;
    if (p && p->xfer) {
        int rc = libusb_cancel_transfer(p->xfer);
        if (rc == LIBUSB_ERROR_NOT_FOUND) {
            status = 0; /* már befejeződött */
        } else if (rc < 0) {
            status = -EINVAL;
        } else {
            status = -ECONNRESET;
        }
    } else {
        status = 0;  /* nem találtuk — már befejeződhetett */
    }

    pthread_mutex_lock(&d->write_lock);
    int rc = usbip_write_ret_unlink(d->s->socket_fd,
                                    bhdr->seqnum, bhdr->devid,
                                    bhdr->direction, bhdr->ep,
                                    status);
    pthread_mutex_unlock(&d->write_lock);
    return rc;
}

/* ---------- Public API ---------- */

int bridge_session_open_usb(bridge_session_t *s)
{
    if (!s || !s->ctx || s->usb_fd < 0) return -EINVAL;

    int rc = libusb_wrap_sys_device(s->ctx->usb_ctx,
                                    (intptr_t)s->usb_fd,
                                    &s->usb_handle);
    if (rc < 0) {
        fprintf(stderr, "[bridge] libusb_wrap_sys_device: %s\n",
                libusb_strerror(rc));
        return rc;
    }

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

int bridge_session_serve(bridge_session_t *s)
{
    struct bridge_dispatch *d = calloc(1, sizeof(*d));
    if (!d) return -ENOMEM;
    s->dispatch = d;
    d->s = s;

    pthread_mutex_init(&d->lock, NULL);
    pthread_mutex_init(&d->write_lock, NULL);

    cache_endpoint_types(d);
    claim_all_interfaces(d);

    int prc = pthread_create(&d->event_thread, NULL, event_thread_main, d);
    if (prc != 0) {
        fprintf(stderr, "[dispatch] pthread_create: %s\n", strerror(prc));
        release_all_interfaces(d);
        pthread_mutex_destroy(&d->lock);
        pthread_mutex_destroy(&d->write_lock);
        free(d);
        s->dispatch = NULL;
        return -prc;
    }
    d->event_thread_started = 1;

    /* PDU read loop. */
    int err = 0;
    while (!atomic_load(&d->stop)) {
        struct usbip_header_basic bhdr;
        int rrc = usbip_read_basic(s->socket_fd, &bhdr);
        if (rrc <= 0) {
            if (rrc < 0) {
                fprintf(stderr, "[dispatch] basic read: %s\n", strerror(-rrc));
                err = rrc;
            }
            break;
        }

        if (bhdr.command == USBIP_CMD_SUBMIT) {
            struct usbip_header_cmd_submit cmd;
            int r = usbip_read_cmd_submit(s->socket_fd, &cmd);
            if (r < 0) { err = r; break; }

            void  *payload     = NULL;
            size_t payload_len = 0;
            if (bhdr.direction == USBIP_DIR_OUT
                && cmd.transfer_buffer_length > 0
                && cmd.transfer_buffer_length <= MAX_XFER_BYTES) {
                payload_len = (size_t)cmd.transfer_buffer_length;
                payload     = malloc(payload_len);
                if (!payload) { err = -ENOMEM; break; }
                ssize_t pr = usbip_read_all(s->socket_fd, payload, payload_len);
                if (pr <= 0) {
                    free(payload);
                    err = pr == 0 ? -EIO : (int)pr;
                    break;
                }
            }

            submit_one(d, &bhdr, &cmd, payload, payload_len);
        } else if (bhdr.command == USBIP_CMD_UNLINK) {
            struct usbip_header_cmd_unlink u;
            int r = usbip_read_cmd_unlink(s->socket_fd, &u);
            if (r < 0) { err = r; break; }
            unlink_one(d, &bhdr, &u);
        } else {
            fprintf(stderr, "[dispatch] ismeretlen PDU command: 0x%08x\n",
                    bhdr.command);
            err = -EPROTO;
            break;
        }
    }

    /* Shutdown: jelezzük az event szálnak, várjuk meg. */
    atomic_store(&d->stop, 1);

    /* Folyamatban lévő transferek lemondása. */
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (d->table[i].in_use && d->table[i].xfer) {
            (void)libusb_cancel_transfer(d->table[i].xfer);
        }
    }
    pthread_mutex_unlock(&d->lock);

    if (d->event_thread_started) {
        pthread_join(d->event_thread, NULL);
    }

    release_all_interfaces(d);
    pthread_mutex_destroy(&d->lock);
    pthread_mutex_destroy(&d->write_lock);
    free(d);
    s->dispatch = NULL;
    return err;
}

void bridge_session_close(bridge_session_t *s)
{
    if (!s) return;
    /* dispatch felszabadítása az bridge_session_serve végén történik. */
    if (s->usb_handle) {
        libusb_close(s->usb_handle);
        s->usb_handle = NULL;
    }
    if (s->socket_fd >= 0) {
        close(s->socket_fd);
        s->socket_fd = -1;
    }
    s->usb_fd = -1;
}
