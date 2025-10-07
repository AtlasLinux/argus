#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>

#include "drm_simple.h"
#include "wayland.h"
#include "input.h"

static volatile int running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, handle_sigint);

    printf("Argus starting: Wayland + DRM + Input integration test\n");

    if (drm_setup() != 0) {
        fprintf(stderr, "drm_setup failed\n");
        return 1;
    }

    if (wl_init_server() != 0) {
        fprintf(stderr, "Wayland server init failed\n");
        drm_teardown();
        return 1;
    }

    if (input_init() != 0) {
        fprintf(stderr, "input_init failed (continuing without input)\n");
    }

    /* Colors to pageflip through */
    uint32_t colors[][3] = {
        {0xff, 0x00, 0x00}, /* red */
        {0x00, 0xff, 0x00}, /* green */
        {0x00, 0x80, 0xff}, /* blue-ish */
        {0xff, 0xff, 0xff}, /* white */
    };

    int idx = 0;

    while (running) {
        /* Process Wayland events (non-blocking up to 10 ms) */
        if (wl_run_iteration(10) != 0) {
            fprintf(stderr, "Wayland iteration failed\n");
            break;
        }

        /* Process libinput if available */
        int ifd = input_get_fd();
        if (ifd >= 0) {
            struct pollfd pfd = { .fd = ifd, .events = POLLIN };
            int pret = poll(&pfd, 1, 0);
            if (pret > 0) {
                if (input_dispatch() != 0) {
                    fprintf(stderr, "input_dispatch error\n");
                }
            }
        }

        /* Present next color (blocking until pageflip completes) */
        uint32_t *c = colors[idx % (sizeof(colors)/sizeof(colors[0]))];
        if (drm_present_solid(c[0], c[1], c[2]) != 0) {
            fprintf(stderr, "drm_present_solid failed\n");
            break;
        }
        idx++;

        /* Sleep in small steps while still dispatching Wayland to keep clients responsive.
         * Sleep total 2000ms split into 100ms chunks and dispatch events.
         */
        int loops = 20;
        for (int i = 0; i < loops && running; ++i) {
            if (wl_run_iteration(100) != 0) {
                running = 0;
                break;
            }
            if (ifd >= 0) {
                struct pollfd pfd = { .fd = ifd, .events = POLLIN };
                int pret = poll(&pfd, 1, 0);
                if (pret > 0) {
                    if (input_dispatch() != 0) {
                        running = 0;
                        break;
                    }
                }
            }
        }
    }

    input_fini();
    wl_fini_server();
    drm_teardown();
    printf("Argus exiting\n");
    return 0;
}
