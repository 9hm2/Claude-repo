/*
 * kaliterm-usb-bridge — Unix socket alapú USB/IP server, ami az Android
 * UsbManager-től kapott file descriptor-okat libusb-vel köti rá a saját
 * LKL kernelben futó vhci_hcd-re.
 *
 * Egyszerű daemon: egy stream Unix socket-en hallgat, minden új kapcsolat
 * egy USB eszköz "attach"-elt csatornája. A kliens (Android app) első
 * üzenetében átküldi az fd-t SCM_RIGHTS-szal.
 *
 * Fázis 1b skeleton: az URB-loop még TODO.
 */
#define _GNU_SOURCE
#include "bridge.h"

#include <errno.h>
#include <getopt.h>
#include <libusb-1.0/libusb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SOCKET_PATH "/tmp/kaliterm-usb.sock"

static bridge_ctx_t g_ctx;

static void on_signal(int sig)
{
    (void)sig;
    bridge_stop(&g_ctx);
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Használat: %s [opciók]\n"
        "  -s, --socket=PATH   Unix socket elérési útja (alap: %s)\n"
        "  -v, --verbose       Részletes libusb log\n"
        "  -h, --help          Súgó\n",
        prog, DEFAULT_SOCKET_PATH);
}

int main(int argc, char **argv)
{
    const char *socket_path = DEFAULT_SOCKET_PATH;
    int verbose = 0;

    static const struct option opts[] = {
        { "socket",  required_argument, 0, 's' },
        { "verbose", no_argument,       0, 'v' },
        { "help",    no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };

    int c;
    while ((c = getopt_long(argc, argv, "s:vh", opts, NULL)) != -1) {
        switch (c) {
        case 's': socket_path = optarg; break;
        case 'v': verbose = 1; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 2;
        }
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.socket_path = socket_path;
    g_ctx.listen_fd   = -1;

    /* Signalek: tiszta kilépés. */
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* libusb. */
    int rc = libusb_init(&g_ctx.usb_ctx);
    if (rc < 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_strerror(rc));
        return 1;
    }
    if (verbose) {
        libusb_set_option(g_ctx.usb_ctx, LIBUSB_OPTION_LOG_LEVEL,
                          LIBUSB_LOG_LEVEL_DEBUG);
    }

    if (bridge_listen(&g_ctx) < 0) {
        libusb_exit(g_ctx.usb_ctx);
        return 1;
    }

    fprintf(stderr, "[bridge] kaliterm-usb-bridge skeleton elindult\n");

    int err = bridge_accept_loop(&g_ctx);

    bridge_stop(&g_ctx);
    libusb_exit(g_ctx.usb_ctx);
    return err < 0 ? 1 : 0;
}
