/*
 * Unix socket szerver a kaliterm USB bridge-hez.
 *
 * Egy stream Unix socket-en hallgatunk. Minden bejövő kapcsolat egy
 * vhci_hcd attach-pont egy konkrét USB eszközhöz. A kliens (Android
 * oldal) első üzenetében átküldi az UsbManager-től kapott file descriptor-t
 * SCM_RIGHTS-szal, ezt fogadjuk itt és továbbadjuk a session worker-nek.
 */
#define _GNU_SOURCE
#include "bridge.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

int bridge_listen(bridge_ctx_t *ctx)
{
    struct sockaddr_un addr = {0};

    if (!ctx || !ctx->socket_path) {
        return -EINVAL;
    }

    /* Régi socket-fájl törlése (best-effort). */
    (void)unlink(ctx->socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return -errno;
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ctx->socket_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -errno;
    }

    if (listen(fd, 8) < 0) {
        perror("listen");
        close(fd);
        return -errno;
    }

    ctx->listen_fd = fd;
    fprintf(stderr, "[bridge] listening on %s (fd=%d)\n",
            ctx->socket_path, fd);
    return 0;
}

/*
 * SCM_RIGHTS-szal beérkező file descriptor fogadása.
 * Visszaadja a sock-on érkezett dummy bájtok számát.
 * `*out_fd` lesz a kapott fd (vagy -1 ha nem jött).
 */
int bridge_recv_fd(int sock_fd, int *out_fd)
{
    char           dummy = 0;
    struct iovec   iov = { .iov_base = &dummy, .iov_len = 1 };
    char           cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr  msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
    };

    *out_fd = -1;

    ssize_t n = recvmsg(sock_fd, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0) {
        return -errno;
    }

    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int fd;
            memcpy(&fd, CMSG_DATA(cm), sizeof(fd));
            *out_fd = fd;
            break;
        }
    }

    return (int)n;
}

int bridge_accept_loop(bridge_ctx_t *ctx)
{
    while (!ctx->stopping) {
        int cfd = accept4(ctx->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept4");
            return -errno;
        }

        fprintf(stderr, "[bridge] accepted client fd=%d\n", cfd);

        bridge_session_t s = {
            .ctx       = ctx,
            .socket_fd = cfd,
            .usb_fd    = -1,
        };

        if (bridge_recv_fd(cfd, &s.usb_fd) < 0 || s.usb_fd < 0) {
            fprintf(stderr, "[bridge] kliens nem küldött USB fd-t — bontás\n");
            close(cfd);
            continue;
        }

        if (bridge_session_open_usb(&s) == 0) {
            (void)bridge_session_serve(&s);
        }
        bridge_session_close(&s);
    }
    return 0;
}

void bridge_stop(bridge_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->stopping = 1;
    if (ctx->listen_fd >= 0) {
        shutdown(ctx->listen_fd, SHUT_RDWR);
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    if (ctx->socket_path) {
        (void)unlink(ctx->socket_path);
    }
}
