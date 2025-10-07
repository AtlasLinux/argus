#define _GNU_SOURCE
#include "input.h"
#include "wayland.h"

#include <libinput.h>
#include <libudev.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>

#include <wayland-server-protocol.h> /* for WL_* state constants */

/* libinput context and udev */
static struct libinput *li = NULL;
static struct udev *udev_ctx = NULL;

static uint32_t libinput_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
}

/* These callbacks are used by libinput to open device FDs */
static int open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    int fd = open(path, flags);
    if (fd < 0)
        fprintf(stderr, "open_restricted %s failed: %s\n", path, strerror(errno));
    return fd;
}
static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    if (fd >= 0) close(fd);
}

static const struct libinput_interface li_interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

int input_init(void) {
    if (li) return 0;
    udev_ctx = udev_new();
    if (!udev_ctx) {
        fprintf(stderr, "udev_new failed\n");
        return -1;
    }

    li = libinput_udev_create_context(&li_interface, NULL, udev_ctx);
    if (!li) {
        fprintf(stderr, "libinput_udev_create_context failed\n");
        udev_unref(udev_ctx);
        udev_ctx = NULL;
        return -1;
    }

    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        fprintf(stderr, "libinput_udev_assign_seat failed\n");
        libinput_unref(li);
        li = NULL;
        udev_unref(udev_ctx);
        udev_ctx = NULL;
        return -1;
    }

    /* Initialize wl_seat (creates globals) */
    if (wl_seat_init() != 0) {
        fprintf(stderr, "wl_seat_init failed\n");
        libinput_unref(li);
        li = NULL;
        udev_unref(udev_ctx);
        udev_ctx = NULL;
        return -1;
    }

    return 0;
}

void input_fini(void) {
    if (li) {
        libinput_unref(li);
        li = NULL;
    }
    if (udev_ctx) {
        udev_unref(udev_ctx);
        udev_ctx = NULL;
    }
    wl_seat_fini();
}

int input_get_fd(void) {
    if (!li) return -1;
    return libinput_get_fd(li);
}

static void handle_pointer_motion(struct libinput_event_pointer *pev) {
    double dx = libinput_event_pointer_get_dx(pev);
    double dy = libinput_event_pointer_get_dy(pev);
    wl_seat_send_pointer_motion(dx, dy);
}

static void handle_pointer_button(struct libinput_event_pointer *pev) {
    uint32_t button = libinput_event_pointer_get_button(pev);
    enum libinput_button_state bs = libinput_event_pointer_get_button_state(pev);

    uint32_t state = (bs == LIBINPUT_BUTTON_STATE_PRESSED) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
    uint32_t time_ms = libinput_time_ms();
    wl_seat_send_pointer_button(time_ms, button, state);
}

static void handle_keyboard_key(struct libinput_event_keyboard *kev) {
    uint32_t key = libinput_event_keyboard_get_key(kev);
    enum libinput_key_state ks = libinput_event_keyboard_get_key_state(kev);

    uint32_t state = (ks == LIBINPUT_KEY_STATE_PRESSED) ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    uint32_t time_ms = libinput_time_ms();
    wl_seat_send_keyboard_key(time_ms, key, state);
}

int input_dispatch(void) {
    if (!li) return -1;

    if (libinput_dispatch(li) != 0) {
        fprintf(stderr, "libinput_dispatch failed\n");
        return -1;
    }

    struct libinput_event *ev;
    while ((ev = libinput_get_event(li)) != NULL) {
        enum libinput_event_type t = libinput_event_get_type(ev);
        switch (t) {
        case LIBINPUT_EVENT_POINTER_MOTION: {
            handle_pointer_motion(libinput_event_get_pointer_event(ev));
            break;
        }
        case LIBINPUT_EVENT_POINTER_BUTTON: {
            handle_pointer_button(libinput_event_get_pointer_event(ev));
            break;
        }
        case LIBINPUT_EVENT_KEYBOARD_KEY: {
            handle_keyboard_key(libinput_event_get_keyboard_event(ev));
            break;
        }
        default:
            break;
        }
        libinput_event_destroy(ev);
    }
    return 0;
}
