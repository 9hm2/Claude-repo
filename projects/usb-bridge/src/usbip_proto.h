#ifndef KALITERM_USBIP_PROTO_H
#define KALITERM_USBIP_PROTO_H

/*
 * USB/IP wire protocol — minimális subset.
 *
 * A teljes definíció a kernel forrásban:
 *   Documentation/usb/usbip_protocol.rst
 *   drivers/usb/usbip/usbip_common.h
 *
 * A vhci_hcd a következő üzeneteket küldi/várja a socket-en (a host
 * adatcsere mind big-endian — htonl/ntohl szükséges):
 *
 *  CMD_SUBMIT  / RET_SUBMIT  — egy URB submission és válasza
 *  CMD_UNLINK  / RET_UNLINK  — folyamatban lévő URB megszakítás
 *
 * Operation codes (devhandshake op_common.command):
 *   0x0003  OP_REQ_IMPORT   (a kliens kéri az eszközt)
 *   0x0001  OP_REP_IMPORT
 *   0x0008  OP_REQ_DEVLIST
 *
 * A handshake után a forgalom CMD_SUBMIT/RET_SUBMIT váltakozik.
 */

#include <stdint.h>

/* PDU command codes a sima URB-ekhez (kernel: USBIP_CMD_*). */
#define USBIP_CMD_SUBMIT    0x00000001
#define USBIP_RET_SUBMIT    0x00000003
#define USBIP_CMD_UNLINK    0x00000002
#define USBIP_RET_UNLINK    0x00000004

/* Direction: kernel: USBIP_DIR_OUT == 0, USBIP_DIR_IN == 1. */
#define USBIP_DIR_OUT       0
#define USBIP_DIR_IN        1

/* USBIP common header (network byte order). */
struct usbip_header_basic {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;       /* (busnum<<16) | devnum */
    uint32_t direction;
    uint32_t ep;          /* endpoint szám */
} __attribute__((packed));

/* CMD_SUBMIT payload (a basic header után). */
struct usbip_header_cmd_submit {
    uint32_t transfer_flags;
    int32_t  transfer_buffer_length;
    int32_t  start_frame;
    int32_t  number_of_packets;
    int32_t  interval;
    uint8_t  setup[8];    /* control transfer setup packet */
} __attribute__((packed));

/* RET_SUBMIT payload. */
struct usbip_header_ret_submit {
    int32_t  status;
    int32_t  actual_length;
    int32_t  start_frame;
    int32_t  number_of_packets;
    int32_t  error_count;
    uint8_t  padding[8];
} __attribute__((packed));

/* CMD_UNLINK payload. */
struct usbip_header_cmd_unlink {
    uint32_t seqnum;      /* az eredeti URB seqnum-a, amit törlünk */
    uint8_t  padding[24];
} __attribute__((packed));

/* RET_UNLINK payload. */
struct usbip_header_ret_unlink {
    int32_t  status;
    uint8_t  padding[24];
} __attribute__((packed));

/* Egy teljes PDU a wire-on. */
struct usbip_header {
    struct usbip_header_basic base;
    union {
        struct usbip_header_cmd_submit  cmd_submit;
        struct usbip_header_ret_submit  ret_submit;
        struct usbip_header_cmd_unlink  cmd_unlink;
        struct usbip_header_ret_unlink  ret_unlink;
    } u;
} __attribute__((packed));

/*
 * PDU I/O helperek (usbip_proto.c).
 * Mind szigorúan partial-read/write biztosak. Konverzió: ntohl/htonl
 * a fields-re, kivéve `setup[]`-ot a control transfer setup-packet.
 */
#include <stddef.h>
#include <sys/types.h>

ssize_t usbip_read_all(int fd, void *buf, size_t len);
ssize_t usbip_write_all(int fd, const void *buf, size_t len);

/* basic header beolvasása: 1 = ok, 0 = EOF, <0 = -errno */
int usbip_read_basic(int fd, struct usbip_header_basic *out);

int usbip_read_cmd_submit(int fd, struct usbip_header_cmd_submit *out);
int usbip_read_cmd_unlink(int fd, struct usbip_header_cmd_unlink *out);

int usbip_write_ret_submit(int fd,
                           uint32_t seqnum, uint32_t devid,
                           uint32_t direction, uint32_t ep,
                           int32_t status, int32_t actual_length,
                           int32_t start_frame, int32_t number_of_packets,
                           int32_t error_count,
                           const void *in_payload, size_t in_payload_len);

int usbip_write_ret_unlink(int fd,
                           uint32_t seqnum, uint32_t devid,
                           uint32_t direction, uint32_t ep,
                           int32_t status);

#endif /* KALITERM_USBIP_PROTO_H */
