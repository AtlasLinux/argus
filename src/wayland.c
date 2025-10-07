#define _GNU_SOURCE
#include "wayland.h"
#include "drm_simple.h"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* Config */
#define MAX_BUFFERS 32
#define MAX_POINTERS 16
#define MAX_KEYBOARDS 8

/* Fallback screen size used for cursor clamping if DRM dims aren't queried here */
static const int FALLBACK_W = 1024;
static const int FALLBACK_H = 768;

/* Pool user data stored on wl_shm_pool resource */
struct pool_user {
    void *map;
    size_t size;
};

/* Tracked shm buffer */
struct shm_buffer {
    int used;
    struct wl_resource *buffer_res;
    void *data;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    off_t offset;
    size_t size;
};

static struct shm_buffer buffers[MAX_BUFFERS];

/* Globals */
static struct wl_display *display = NULL;
static struct wl_event_loop *evloop = NULL;
static const char *socket_name = NULL;

/* Single compositor surface (simple single-surface compositor) */
static struct wl_resource *g_surface_res = NULL;

/* Pointer/keyboard resources lists */
static struct wl_resource *pointer_resources[MAX_POINTERS];
static int pointer_count = 0;
static struct wl_resource *keyboard_resources[MAX_KEYBOARDS];
static int keyboard_count = 0;

/* Virtual cursor position */
static double seat_cx = -1.0;
static double seat_cy = -1.0;

/* Helpers for buffer tracking */
static struct shm_buffer *allocate_shm_buffer(void) {
    for (int i = 0; i < MAX_BUFFERS; ++i) {
        if (!buffers[i].used) {
            buffers[i].used = 1;
            buffers[i].buffer_res = NULL;
            buffers[i].data = NULL;
            buffers[i].width = buffers[i].height = buffers[i].stride = 0;
            buffers[i].format = 0;
            buffers[i].offset = 0;
            buffers[i].size = 0;
            return &buffers[i];
        }
    }
    return NULL;
}

static void free_shm_buffer_by_resource(struct wl_resource *res) {
    if (!res) return;
    for (int i = 0; i < MAX_BUFFERS; ++i) {
        if (buffers[i].used && buffers[i].buffer_res == res) {
            buffers[i].used = 0;
            buffers[i].buffer_res = NULL;
            buffers[i].data = NULL;
            buffers[i].width = buffers[i].height = buffers[i].stride = 0;
            buffers[i].format = 0;
            buffers[i].offset = 0;
            buffers[i].size = 0;
            return;
        }
    }
}

/* --- wl_shm pool / buffer handling --- */

/* wl_shm_pool.create_buffer implementation (forwarded by generated binding) */
static void shm_pool_create_buffer(struct wl_client *client,
                                   struct wl_resource *pool_res,
                                   int32_t offset,
                                   int32_t width,
                                   int32_t height,
                                   int32_t stride,
                                   uint32_t format,
                                   uint32_t buffer_id) {
    (void)client;
    struct pool_user *pu = wl_resource_get_user_data(pool_res);
    if (!pu) {
        wl_resource_post_error(pool_res, WL_SHM_ERROR_INVALID_FD, "pool not initialized");
        return;
    }

    if (offset < 0 || width <= 0 || height <= 0 || stride <= 0) {
        wl_resource_post_error(pool_res, WL_SHM_ERROR_INVALID_STRIDE, "invalid buffer dimensions");
        return;
    }

    size_t needed = (size_t)offset + (size_t)stride * (size_t)height;
    if (needed > pu->size) {
        wl_resource_post_error(pool_res, WL_SHM_ERROR_INVALID_FD, "buffer out of pool bounds");
        return;
    }

    struct shm_buffer *b = allocate_shm_buffer();
    if (!b) {
        wl_resource_post_no_memory(pool_res);
        return;
    }

    b->data = (uint8_t *)pu->map + offset;
    b->offset = offset;
    b->size = pu->size - offset;
    b->width = (uint32_t)width;
    b->height = (uint32_t)height;
    b->stride = (uint32_t)stride;
    b->format = format;
    b->buffer_res = NULL;

    /* create the wl_buffer resource the client expects */
    struct wl_resource *buf_res = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    if (!buf_res) {
        b->used = 0;
        wl_resource_post_no_memory(pool_res);
        return;
    }

    /* associate our tracking struct with the created wl_buffer resource */
    b->buffer_res = buf_res;
    wl_resource_set_user_data(buf_res, b);

    /* ensure buffer destroy cleans our tracking */
    wl_resource_set_implementation(buf_res, NULL, b, (wl_resource_destroy_func_t)free_shm_buffer_by_resource);
}

/* wl_shm.create_pool implementation: mmap provided fd and create a wl_shm_pool for client */
static void shm_create_pool(struct wl_client *client, struct wl_resource *shm_res, int fd, int32_t size, uint32_t pool_id) {
    (void)client;
    if (fd < 0 || size <= 0) {
        wl_resource_post_error(shm_res, WL_SHM_ERROR_INVALID_FD, "invalid fd/size");
        if (fd >= 0) close(fd);
        return;
    }

    void *map = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        wl_resource_post_error(shm_res, WL_SHM_ERROR_INVALID_FD, "mmap failed");
        close(fd);
        return;
    }
    close(fd);

    struct pool_user *pu = calloc(1, sizeof(*pu));
    if (!pu) {
        munmap(map, (size_t)size);
        wl_resource_post_no_memory(shm_res);
        return;
    }
    pu->map = map;
    pu->size = (size_t)size;

    struct wl_client *client_for_pool = wl_resource_get_client(shm_res);
    struct wl_resource *pool_res = wl_resource_create(client_for_pool, &wl_shm_pool_interface, 1, pool_id);
    if (!pool_res) {
        munmap(map, (size_t)size);
        free(pu);
        wl_resource_post_no_memory(shm_res);
        return;
    }

    wl_resource_set_user_data(pool_res, pu);
    /* free pool_user on pool destroy */
    wl_resource_set_implementation(pool_res, NULL, pu, (wl_resource_destroy_func_t)free);
}

/* Buffer destroy callback used when client destroys wl_buffer */
static void wl_buffer_destroy_cb(struct wl_resource *buffer_res) {
    free_shm_buffer_by_resource(buffer_res);
}

/* --- wl_surface handling (single surface) --- */

/* wl_surface.attach handler */
static void wl_surface_attach_cb(struct wl_client *client, struct wl_resource *surface_res,
                                 struct wl_resource *buffer_res, int32_t x, int32_t y) {
    (void)client; (void)x; (void)y;
    if (!surface_res) return;

    if (!g_surface_res) {
        g_surface_res = surface_res;
        wl_resource_set_implementation(g_surface_res, NULL, NULL, (wl_resource_destroy_func_t)NULL);
    }

    if (!buffer_res) {
        wl_resource_set_user_data(surface_res, NULL);
        return;
    }

    struct shm_buffer *b = wl_resource_get_user_data(buffer_res);
    if (!b) {
        /* buffer has no user_data (unsupported creation path) */
        fprintf(stderr, "Argus: attach received wl_buffer with no user_data\n");
        return;
    }

    /* ensure buffer destroy will free our tracking */
    wl_resource_set_implementation(buffer_res, NULL, b, (wl_resource_destroy_func_t)wl_buffer_destroy_cb);

    /* attach buffer to surface user_data for commit */
    wl_resource_set_user_data(surface_res, b);
}

/* wl_surface.commit handler */
static void wl_surface_commit_cb(struct wl_client *client, struct wl_resource *surface_res) {
    (void)client;
    struct shm_buffer *b = wl_resource_get_user_data(surface_res);
    if (!b) return;

    /* Accept WL_SHM_FORMAT_XRGB8888 only */
    if (b->format != WL_SHM_FORMAT_XRGB8888) {
        fprintf(stderr, "Argus: unsupported shm format %u\n", b->format);
        return;
    }

    if (drm_present_from_shm(b->data, b->stride, b->width, b->height) != 0) {
        fprintf(stderr, "Argus: drm_present_from_shm failed\n");
    }
}

/* surface destroy */
static void wl_surface_destroy_cb(struct wl_resource *surface_res) {
    if (g_surface_res == surface_res) g_surface_res = NULL;
}

/* --- compositor bind / create_surface --- */

static void compositor_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *surf = wl_resource_create(client, &wl_surface_interface, 1, id);
    if (!surf) return;

    static const struct wl_surface_interface surf_impl = {
        .destroy = NULL,
        .attach = wl_surface_attach_cb,
        .damage = NULL,
        .frame = NULL,
        .set_opaque_region = NULL,
        .set_input_region = NULL,
        .commit = wl_surface_commit_cb,
        .set_buffer_transform = NULL,
        .set_buffer_scale = NULL,
        .damage_buffer = NULL
    };

    wl_resource_set_implementation(surf, &surf_impl, NULL, (wl_resource_destroy_func_t)wl_surface_destroy_cb);
}

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data; (void)version;
    struct wl_resource *res = wl_resource_create(client, &wl_compositor_interface, 1, id);
    if (!res) return;

    static const struct wl_compositor_interface comp_impl = {
        .create_surface = compositor_create_surface,
        .create_region = NULL
    };

    wl_resource_set_implementation(res, &comp_impl, NULL, NULL);
}

/* --- wl_shm bind (expose create_pool) --- */

static void shm_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data; (void)version;
    struct wl_resource *res = wl_resource_create(client, &wl_shm_interface, 1, id);
    if (!res) return;

    /* cast to match generated prototype on this system */
    static const struct wl_shm_interface shm_impl = {
        .create_pool = (void (*)(struct wl_client *, struct wl_resource *, uint32_t, int32_t, int32_t))shm_create_pool
    };

    wl_resource_set_implementation(res, &shm_impl, NULL, NULL);
}

/* --- wl_seat implementation (pointer + keyboard broadcasting) --- */

/* pointer and keyboard implementations (release only) */
static void pointer_release(struct wl_client *client, struct wl_resource *res) {
    (void)client; (void)res;
}
static void keyboard_release(struct wl_client *client, struct wl_resource *res) {
    (void)client; (void)res;
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = NULL,
    .release = pointer_release
};

static const struct wl_keyboard_interface keyboard_impl = {
    .release = keyboard_release
};

/* send pointer/key events helpers */
void wl_seat_send_pointer_motion(double dx, double dy) {
    if (seat_cx < 0.0 || seat_cy < 0.0) {
        seat_cx = (double)FALLBACK_W / 2.0;
        seat_cy = (double)FALLBACK_H / 2.0;
    }

    seat_cx += dx;
    seat_cy += dy;

    if (seat_cx < 0.0) seat_cx = 0.0;
    if (seat_cy < 0.0) seat_cy = 0.0;
    if (seat_cx > FALLBACK_W - 1) seat_cx = FALLBACK_W - 1;
    if (seat_cy > FALLBACK_H - 1) seat_cy = FALLBACK_H - 1;

    uint32_t time_ms = (uint32_t)(clock() * 1000 / CLOCKS_PER_SEC);

    for (int i = 0; i < pointer_count; ++i) {
        struct wl_resource *pr = pointer_resources[i];
        if (!pr) continue;
        wl_pointer_send_motion(pr, time_ms,
                               wl_fixed_from_double(seat_cx),
                               wl_fixed_from_double(seat_cy));
        wl_pointer_send_frame(pr);
    }
}

void wl_seat_send_pointer_button(uint32_t time_ms, uint32_t button, uint32_t state) {
    uint32_t serial = wl_display_get_serial(display);
    for (int i = 0; i < pointer_count; ++i) {
        struct wl_resource *pr = pointer_resources[i];
        if (!pr) continue;
        wl_pointer_send_button(pr, serial, time_ms, button, state);
        wl_pointer_send_frame(pr);
    }
}

void wl_seat_send_keyboard_key(uint32_t time_ms, uint32_t key, uint32_t state) {
    uint32_t serial = wl_display_get_serial(display);
    for (int i = 0; i < keyboard_count; ++i) {
        struct wl_resource *kr = keyboard_resources[i];
        if (!kr) continue;
        wl_keyboard_send_key(kr, serial, time_ms, key, state);
    }
}

/* wl_seat.get_pointer/get_keyboard handled by creating resources on bind */
static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data; (void)version;
    struct wl_resource *res = wl_resource_create(client, &wl_seat_interface, 1, id);
    if (!res) return;

    /* create pointer resource for this client */
    if (pointer_count < MAX_POINTERS) {
        struct wl_resource *pr = wl_resource_create(client, &wl_pointer_interface, 1, wl_display_get_serial(display));
        if (pr) {
            wl_resource_set_implementation(pr, &pointer_impl, NULL, NULL);
            pointer_resources[pointer_count++] = pr;
        }
    }

    /* create keyboard resource for this client */
    if (keyboard_count < MAX_KEYBOARDS) {
        struct wl_resource *kr = wl_resource_create(client, &wl_keyboard_interface, 1, wl_display_get_serial(display));
        if (kr) {
            wl_resource_set_implementation(kr, &keyboard_impl, NULL, NULL);
            keyboard_resources[keyboard_count++] = kr;
        }
    }

    /* advertise seat name */
    wl_seat_send_name(res, "seat0");
}

/* initialize/free seat */
int wl_seat_init(void) {
    if (!display) return -1;
    wl_global_create(display, &wl_seat_interface, 1, NULL, seat_bind);
    return 0;
}

void wl_seat_fini(void) {
    /* clear resources lists; actual wl_resource cleanup is handled by Wayland on client disconnect */
    for (int i = 0; i < pointer_count; ++i) pointer_resources[i] = NULL;
    pointer_count = 0;
    for (int i = 0; i < keyboard_count; ++i) keyboard_resources[i] = NULL;
    keyboard_count = 0;
}

/* --- server lifecycle --- */

int wl_init_server(void) {
    if (display) return 0;

    display = wl_display_create();
    if (!display) {
        fprintf(stderr, "wl_display_create failed\n");
        return -1;
    }

    socket_name = wl_display_add_socket_auto(display);
    if (!socket_name) {
        wl_display_destroy(display);
        display = NULL;
        return -1;
    }

    evloop = wl_display_get_event_loop(display);
    if (!evloop) {
        wl_display_destroy(display);
        display = NULL;
        socket_name = NULL;
        return -1;
    }

    /* create required globals */
    wl_global_create(display, &wl_compositor_interface, 1, NULL, compositor_bind);
    wl_global_create(display, &wl_shm_interface, 1, NULL, shm_bind);
    /* seat will be created by input_init calling wl_seat_init */

    wl_display_flush_clients(display);
    printf("Wayland display socket: %s\n", socket_name);
    return 0;
}

int wl_run_iteration(int timeout_ms) {
    if (!display) return -1;
    if (!evloop) return -1;
    int rc = wl_event_loop_dispatch(evloop, timeout_ms);
    if (rc < 0) {
        fprintf(stderr, "wl_event_loop_dispatch returned %d\n", rc);
        return -1;
    }
    wl_display_flush_clients(display);
    return 0;
}

void wl_fini_server(void) {
    if (!display) return;
    wl_display_destroy(display);
    display = NULL;
    evloop = NULL;
    socket_name = NULL;
}

struct wl_display *wl_get_display(void) {
    return display;
}
