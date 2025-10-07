// Minimal wl_shm client: creates a surface, a wl_shm_pool -> wl_buffer,
// draws a simple gradient into XRGB8888 shm and commits the surface.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <time.h>

static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;

static void registry_handler(void *data, struct wl_registry *reg,
                             uint32_t id, const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 1);
    } else if (strcmp(interface, "wl_shm") == 0) {
        shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    }
}

static void registry_remover(void *data, struct wl_registry *reg, uint32_t id) {
    (void)data; (void)reg; (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handler,
    .global_remove = registry_remover
};

static int create_tmpfile_cloexec(char *tmpname) {
    int fd;
    fd = mkstemp(tmpname);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFD);
        flags |= FD_CLOEXEC;
        fcntl(fd, F_SETFD, flags);
    }
    return fd;
}

/* create anonymous in-memory file suitable for mmap + ftruncate
 * returns fd >= 0 on success or -1 on failure
 */
static int os_create_anonymous_file(off_t size) {
    /* Try memfd_create first (Linux) */
#ifdef MFD_CLOEXEC
    int fd = syscall(SYS_memfd_create, "argus-client", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, size) == 0) return fd;
        close(fd);
    }
#endif

    /* Fallback: create a temporary file in /tmp and mark close-on-exec */
    char template[] = "/tmp/argus-client-XXXXXX";
    int fd2 = mkstemp(template);
    if (fd2 < 0) return -1;
    /* unlink so it is removed when closed */
    unlink(template);
    if (ftruncate(fd2, size) < 0) {
        close(fd2);
        return -1;
    }
    int flags = fcntl(fd2, F_GETFD);
    flags |= FD_CLOEXEC;
    fcntl(fd2, F_SETFD, flags);
    return fd2;
}

int main(int argc, char **argv) {
main:
    (void)argc; (void)argv;
    display = wl_display_connect(NULL);
    if (!display) {
	goto main;
        return 1;
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm) {
        fprintf(stderr, "compositor or shm not available\n");
        return 1;
    }

    const int width = 400;
    const int height = 300;
    const int stride = width * 4;
    const int size = stride * height;

    int fd = os_create_anonymous_file(size);
    if (fd < 0) {
        fprintf(stderr, "failed to create anonymous file: %s\n", strerror(errno));
        return 1;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    // draw simple pattern: XRGB8888 (ignore alpha)
    uint32_t *pix = data;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t r = (uint8_t)((x * 255) / (width - 1));
            uint8_t g = (uint8_t)((y * 255) / (height - 1));
            uint8_t b = 0x80;
            pix[y * width + x] = (0xff << 24) | (r << 16) | (g << 8) | b;
        }
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);

    // roundtrips to ensure commit processed
    wl_display_roundtrip(display);

    // keep window visible for 10 seconds, dispatching events
    for (int i = 0; i < 100; ++i) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        usleep(100000); // 100 ms
    }

    // cleanup
    wl_buffer_destroy(buffer);
    wl_surface_destroy(surface);
    munmap(data, size);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
