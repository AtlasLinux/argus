#define _GNU_SOURCE
#include "drm_simple.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <poll.h>

#include <drm/drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* Minimal DRM state for double-buffered pageflip testing */
struct drm_state {
    int fd;
    drmModeConnector *conn;
    drmModeRes *res;
    drmModeEncoder *enc;
    drmModeCrtc *crtc;
    uint32_t crtc_index;
    uint32_t crtc_id;
    uint32_t connector_id;
    drmModeModeInfo mode;

    /* Two dumb buffers for pageflipping */
    uint32_t fb_id[2];
    uint32_t handle[2];
    uint32_t pitch[2];
    uint64_t size[2];
    void *map[2];
    int front_buf; /* index of currently scanned-out buffer */
    int pending_flip; /* whether a flip is pending */
};

static struct drm_state S = {0};

/* Event cookie passed to pageflip handler */
struct pageflip_cookie {
    struct drm_state *s;
    int which; /* buffer index that will become front after flip */
};

/* Forward */
static int create_dumb_buffer_index(int idx);
static void destroy_dumb_buffer_index(int idx);

/* Pageflip event handler */
static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data) {
    (void)fd; (void)frame; (void)sec; (void)usec;
    struct pageflip_cookie *cookie = data;
    struct drm_state *st = cookie->s;
    /* flip completed: update front buffer */
    st->front_buf = cookie->which;
    st->pending_flip = 0;
    free(cookie);
}

/* Helper to find connector, encoder and CRTC */
static int find_connector_and_crtc(void) {
    int i;
    drmModeConnector *conn = NULL;
    drmModeEncoder *enc = NULL;
    drmModeRes *res = drmModeGetResources(S.fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed\n");
        return -1;
    }
    S.res = res;

    for (i = 0; i < res->count_connectors; ++i) {
        conn = drmModeGetConnector(S.fd, res->connectors[i]);
        if (!conn) continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            S.conn = conn;
            S.connector_id = conn->connector_id;
            S.mode = conn->modes[0]; /* prefer first mode */
            break;
        }
        drmModeFreeConnector(conn);
        conn = NULL;
    }

    if (!S.conn) {
        fprintf(stderr, "No connected connector found\n");
        return -1;
    }

    /* try encoder from connector */
    if (S.conn->encoder_id)
        enc = drmModeGetEncoder(S.fd, S.conn->encoder_id);

    if (!enc) {
        for (i = 0; i < S.conn->count_encoders; ++i) {
            enc = drmModeGetEncoder(S.fd, S.conn->encoders[i]);
            if (enc) break;
        }
    }

    if (!enc) {
        fprintf(stderr, "No encoder found\n");
        return -1;
    }
    S.enc = enc;

    /* pick a CRTC compatible with encoder */
    for (i = 0; i < res->count_crtcs; ++i) {
        if (enc->possible_crtcs & (1 << i)) {
            S.crtc_index = i;
            S.crtc_id = res->crtcs[i];
            S.crtc = drmModeGetCrtc(S.fd, S.crtc_id);
            if (!S.crtc) {
                fprintf(stderr, "drmModeGetCrtc failed for %u\n", S.crtc_id);
                continue;
            }
            return 0;
        }
    }

    fprintf(stderr, "No suitable CRTC found\n");
    return -1;
}

static int create_dumb_buffer_index(int idx) {
    struct drm_mode_create_dumb creq = {0};
    struct drm_mode_map_dumb mreq = {0};
    struct drm_mode_destroy_dumb dreq = {0};
    int ret;

    creq.width = S.mode.hdisplay;
    creq.height = S.mode.vdisplay;
    creq.bpp = 32;

    ret = drmIoctl(S.fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return -1;
    }

    S.handle[idx] = creq.handle;
    S.pitch[idx] = creq.pitch;
    S.size[idx] = creq.size;

    ret = drmModeAddFB(S.fd, S.mode.hdisplay, S.mode.vdisplay, 24, 32, S.pitch[idx], S.handle[idx], &S.fb_id[idx]);
    if (ret) {
        perror("drmModeAddFB");
        dreq.handle = S.handle[idx];
        drmIoctl(S.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    mreq.handle = S.handle[idx];
    ret = drmIoctl(S.fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        drmModeRmFB(S.fd, S.fb_id[idx]);
        dreq.handle = S.handle[idx];
        drmIoctl(S.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    S.map[idx] = mmap(0, S.size[idx], PROT_READ | PROT_WRITE, MAP_SHARED, S.fd, mreq.offset);
    if (S.map[idx] == MAP_FAILED) {
        perror("mmap");
        drmModeRmFB(S.fd, S.fb_id[idx]);
        dreq.handle = S.handle[idx];
        drmIoctl(S.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    return 0;
}

static void destroy_dumb_buffer_index(int idx) {
    struct drm_mode_destroy_dumb dreq = {0};
    if (S.map[idx] && S.map[idx] != MAP_FAILED) {
        munmap(S.map[idx], S.size[idx]);
        S.map[idx] = NULL;
    }
    if (S.fb_id[idx]) {
        drmModeRmFB(S.fd, S.fb_id[idx]);
        S.fb_id[idx] = 0;
    }
    if (S.handle[idx]) {
        dreq.handle = S.handle[idx];
        drmIoctl(S.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        S.handle[idx] = 0;
    }
}

/* Initialize DRM, pick connector/mode, create 2 dumb buffers */
int drm_setup(void) {
    const char *path = "/dev/dri/card1";
    S.fd = open(path, O_RDWR | O_CLOEXEC);
    if (S.fd < 0) {
        perror("open drm device");
        return -1;
    }

    if (find_connector_and_crtc() != 0) {
        close(S.fd);
        S.fd = -1;
        return -1;
    }

    /* Create two buffers */
    for (int i = 0; i < 2; ++i) {
        if (create_dumb_buffer_index(i) != 0) {
            for (int j = 0; j < i; ++j) destroy_dumb_buffer_index(j);
            if (S.crtc) drmModeFreeCrtc(S.crtc);
            if (S.enc) drmModeFreeEncoder(S.enc);
            if (S.conn) drmModeFreeConnector(S.conn);
            if (S.res) drmModeFreeResources(S.res);
            close(S.fd);
            S.fd = -1;
            return -1;
        }
    }

    S.front_buf = 0;
    S.pending_flip = 0;

    return 0;
}

/* Tear down all resources */
void drm_teardown(void) {
    for (int i = 0; i < 2; ++i) destroy_dumb_buffer_index(i);
    if (S.crtc) {
        drmModeFreeCrtc(S.crtc);
        S.crtc = NULL;
    }
    if (S.enc) {
        drmModeFreeEncoder(S.enc);
        S.enc = NULL;
    }
    if (S.conn) {
        drmModeFreeConnector(S.conn);
        S.conn = NULL;
    }
    if (S.res) {
        drmModeFreeResources(S.res);
        S.res = NULL;
    }
    if (S.fd >= 0) {
        close(S.fd);
        S.fd = -1;
    }
}

/* Helper to block until no flip is pending (process events) */
static int wait_for_vblank_completion(int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = S.fd;
    pfd.events = POLLIN;
retry:
    if (!S.pending_flip) return 0;
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) goto retry;
        perror("poll");
        return -1;
    } else if (ret == 0) {
        /* timeout */
        return 1;
    } else {
        /* handle DRM event(s) */
        drmEventContext evctx = {
            .version = DRM_EVENT_CONTEXT_VERSION,
            .page_flip_handler = page_flip_handler
        };
        if (drmHandleEvent(S.fd, &evctx) != 0) {
            perror("drmHandleEvent");
            return -1;
        }
    }
    return 0;
}

/* Fill the non-front buffer with colour and schedule pageflip.
 * This function does not block except to process the flip event if wait==1.
 */
int drm_present_solid(uint32_t r, uint32_t g, uint32_t b) {
    if (!S.map[0] || !S.map[1]) return -1;

    int back = S.front_buf ^ 1;
    uint32_t width = S.mode.hdisplay;
    uint32_t height = S.mode.vdisplay;
    uint32_t pitch = S.pitch[back];
    uint8_t *p = S.map[back];
    uint32_t color = (0xff << 24) | (r << 16) | (g << 8) | b;

    for (uint32_t y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)(p + y * pitch);
        for (uint32_t x = 0; x < width; ++x) {
            row[x] = color;
        }
    }

    /* If this is the first time, setcrtc to back buffer synchronously */
    if (!S.crtc || S.crtc->buffer_id == 0) {
        int ret = drmModeSetCrtc(S.fd, S.crtc_id, S.fb_id[back], 0, 0,
                                 &S.connector_id, 1, &S.mode);
        if (ret) {
            perror("drmModeSetCrtc initial");
            return -1;
        }
        S.front_buf = back;
        return 0;
    }

    /* Schedule pageflip to back buffer with event handler */
    struct pageflip_cookie *cookie = malloc(sizeof(*cookie));
    if (!cookie) return -1;
    cookie->s = &S;
    cookie->which = back;
    int ret = drmModePageFlip(S.fd, S.crtc_id, S.fb_id[back], DRM_MODE_PAGE_FLIP_EVENT, cookie);
    if (ret) {
        perror("drmModePageFlip");
        free(cookie);
        return -1;
    }
    S.pending_flip = 1;

    /* process events until flip completes */
    while (S.pending_flip) {
        int w = wait_for_vblank_completion(5000);
        if (w < 0) return -1;
        if (w == 1) {
            fprintf(stderr, "Timeout waiting for pageflip\n");
            return -1;
        }
    }

    return 0;
}

/* place this function in drm_simple.c after the other helpers */

/* Copy client SHM pixels (assumed XRGB8888 / ARGB8888 little-endian) into back buffer and pageflip.
 * This reuses the pageflip logic already present in this file.
 */
int drm_present_from_shm(const void *src, uint32_t src_stride, uint32_t width, uint32_t height) {
    if (!S.map[0] || !S.map[1]) return -1;

    int back = S.front_buf ^ 1;
    uint32_t dst_pitch = S.pitch[back];
    uint8_t *dst = S.map[back];
    const uint8_t *s = src;

    /* Clip width/height to mode for safety */
    if (width > S.mode.hdisplay) width = S.mode.hdisplay;
    if (height > S.mode.vdisplay) height = S.mode.vdisplay;

    for (uint32_t y = 0; y < height; ++y) {
        memcpy(dst + y * dst_pitch, s + y * src_stride, width * 4);
    }

    /* If this is the first time, setcrtc to back buffer synchronously */
    if (!S.crtc || S.crtc->buffer_id == 0) {
        int ret = drmModeSetCrtc(S.fd, S.crtc_id, S.fb_id[back], 0, 0,
                                 &S.connector_id, 1, &S.mode);
        if (ret) {
            perror("drmModeSetCrtc initial");
            return -1;
        }
        S.front_buf = back;
        return 0;
    }

    /* Schedule pageflip to back buffer with event handler */
    struct pageflip_cookie *cookie = malloc(sizeof(*cookie));
    if (!cookie) return -1;
    cookie->s = &S;
    cookie->which = back;
    int ret = drmModePageFlip(S.fd, S.crtc_id, S.fb_id[back], DRM_MODE_PAGE_FLIP_EVENT, cookie);
    if (ret) {
        perror("drmModePageFlip");
        free(cookie);
        return -1;
    }
    S.pending_flip = 1;

    /* process events until flip completes */
    while (S.pending_flip) {
        int w = wait_for_vblank_completion(5000);
        if (w < 0) return -1;
        if (w == 1) {
            fprintf(stderr, "Timeout waiting for pageflip\n");
            return -1;
        }
    }

    return 0;
}
