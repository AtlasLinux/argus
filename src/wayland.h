#ifndef WAYLAND_HELPER_H
#define WAYLAND_HELPER_H

#include <wayland-server-core.h>

int wl_init_server(void);
int wl_run_iteration(int timeout_ms);
void wl_fini_server(void);
struct wl_display *wl_get_display(void);

/* Seat / input helpers (used by input.c) */
int wl_seat_init(void);
void wl_seat_fini(void);

/* Send input events from the compositor (called by input dispatcher) */
void wl_seat_send_pointer_motion(double dx, double dy);
void wl_seat_send_pointer_button(uint32_t time_ms, uint32_t button, uint32_t state);
void wl_seat_send_keyboard_key(uint32_t time_ms, uint32_t key, uint32_t state);

#endif
