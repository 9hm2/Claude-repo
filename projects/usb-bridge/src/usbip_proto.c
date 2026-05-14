/*
 * USB/IP wire PDU I/O — szigorúan szekvenciális read/write, network byte
 * order konverzió, kanonikus hibakezelés.
 */
#define _GNU_SOURCE
#include "usbip_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* read_all: pontosan `len` bájtot olvas, vagy hibát ad.
 * Visszaad:  0 — EOF (a kliens bontott)
 *           >0 — sikeres, == len
 *           <0 — hiba (-errno) */
ssize_t usbip_read_all(int fd, void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, (char *)buf + off, len - off);
        if (n == 0) return 0;                     /* EOF */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        off += (size_t)n;
    }
    return (ssize_t)len;
}

/* write_all: pontosan `len` bájtot ír, hibára -errno-t ad. */
ssize_t usbip_write_all(int fd, const void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, (const char *)buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        off += (size_t)n;
    }
    return (ssize_t)len;
}

/* Basic header beolvasása + ntohl konverzió a wire-ról a host-rendre. */
int usbip_read_basic(int fd, struct usbip_header_basic *out)
{
    ssize_t n = usbip_read_all(fd, out, sizeof(*out));
    if (n == 0) return 0;
    if (n < 0)  return (int)n;
    out->command   = ntohl(out->command);
    out->seqnum    = ntohl(out->seqnum);
    out->devid     = ntohl(out->devid);
    out->direction = ntohl(out->direction);
    out->ep        = ntohl(out->ep);
    return 1;
}

int usbip_read_cmd_submit(int fd, struct usbip_header_cmd_submit *out)
{
    ssize_t n = usbip_read_all(fd, out, sizeof(*out));
    if (n <= 0) return n == 0 ? -EIO : (int)n;
    out->transfer_flags         = ntohl(out->transfer_flags);
    out->transfer_buffer_length = (int32_t)ntohl((uint32_t)out->transfer_buffer_length);
    out->start_frame            = (int32_t)ntohl((uint32_t)out->start_frame);
    out->number_of_packets      = (int32_t)ntohl((uint32_t)out->number_of_packets);
    out->interval               = (int32_t)ntohl((uint32_t)out->interval);
    /* setup[] raw bájtok — nincs konverzió */
    return 0;
}

int usbip_read_cmd_unlink(int fd, struct usbip_header_cmd_unlink *out)
{
    ssize_t n = usbip_read_all(fd, out, sizeof(*out));
    if (n <= 0) return n == 0 ? -EIO : (int)n;
    out->seqnum = ntohl(out->seqnum);
    return 0;
}

/* RET_SUBMIT PDU írása az opcionális IN-payloaddal együtt. */
int usbip_write_ret_submit(int fd,
                           uint32_t seqnum, uint32_t devid,
                           uint32_t direction, uint32_t ep,
                           int32_t status, int32_t actual_length,
                           int32_t start_frame, int32_t number_of_packets,
                           int32_t error_count,
                           const void *in_payload, size_t in_payload_len)
{
    struct usbip_header_basic    bhdr = {
        .command   = htonl(USBIP_RET_SUBMIT),
        .seqnum    = htonl(seqnum),
        .devid     = htonl(devid),
        .direction = htonl(direction),
        .ep        = htonl(ep),
    };
    struct usbip_header_ret_submit r = {
        .status            = (int32_t)htonl((uint32_t)status),
        .actual_length     = (int32_t)htonl((uint32_t)actual_length),
        .start_frame       = (int32_t)htonl((uint32_t)start_frame),
        .number_of_packets = (int32_t)htonl((uint32_t)number_of_packets),
        .error_count       = (int32_t)htonl((uint32_t)error_count),
    };

    ssize_t n = usbip_write_all(fd, &bhdr, sizeof(bhdr));
    if (n < 0) return (int)n;
    n = usbip_write_all(fd, &r, sizeof(r));
    if (n < 0) return (int)n;
    if (in_payload_len > 0 && in_payload) {
        n = usbip_write_all(fd, in_payload, in_payload_len);
        if (n < 0) return (int)n;
    }
    return 0;
}

/* RET_UNLINK PDU. */
int usbip_write_ret_unlink(int fd,
                           uint32_t seqnum, uint32_t devid,
                           uint32_t direction, uint32_t ep,
                           int32_t status)
{
    struct usbip_header_basic    bhdr = {
        .command   = htonl(USBIP_RET_UNLINK),
        .seqnum    = htonl(seqnum),
        .devid     = htonl(devid),
        .direction = htonl(direction),
        .ep        = htonl(ep),
    };
    struct usbip_header_ret_unlink r = {
        .status = (int32_t)htonl((uint32_t)status),
    };
    ssize_t n = usbip_write_all(fd, &bhdr, sizeof(bhdr));
    if (n < 0) return (int)n;
    n = usbip_write_all(fd, &r, sizeof(r));
    if (n < 0) return (int)n;
    return 0;
}
