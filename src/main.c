/* main.c */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drm_util.h"
#include "drm/drm.h"
#include "drm/drm_mode.h"

int main(int argc, char **argv) {
    const char *dev = (argc > 1) ? argv[1] : "/dev/dri/card0";
    int fd = drm_open(dev);
    if (fd < 0) {
        perror("open DRM");
        return 1;
    }

    /* first call GETRESOURCES to get counts */
    struct drm_mode_card_res res = {0};
    if (drm_get_resources(fd, &res) < 0) {
        perror("GETRESOURCES (size)");
        return 1;
    }
    /* allocate arrays */
    uint32_t *conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
    res.connector_id_ptr = (uintptr_t)conn_ids;
    res.count_connectors = res.count_connectors;
    if (drm_get_resources(fd, &res) < 0) {
        perror("GETRESOURCES");
        return 1;
    }
    memcpy(conn_ids, (void*)res.connector_id_ptr, res.count_connectors * sizeof(uint32_t));

    /* find first connected connector */
    uint32_t connector_id = 0;
    struct drm_mode_get_connector conn = {0};
    struct drm_mode_modeinfo modeinfo = {0};

    for (int i = 0; i < res.count_connectors; ++i) {
        connector_id = conn_ids[i];
        conn.connector_id = connector_id;

        /* first query to get counts */
        if (drm_get_connector(fd, connector_id, &conn) < 0) continue;

        struct drm_mode_get_connector *conn2 = calloc(1, sizeof(*conn2));
        *conn2 = conn;
        conn2->connector_id = connector_id;
        conn2->modes_ptr = (uintptr_t)malloc(conn.count_modes * sizeof(struct drm_mode_modeinfo));
        conn2->count_modes = conn.count_modes;
        if (drm_get_connector(fd, connector_id, conn2) < 0) {
            free((void*)conn2->modes_ptr);
            free(conn2);
            continue;
        }

        if (conn2->count_modes > 0 && conn2->connection == DRM_MODE_CONNECTED) {
            struct drm_mode_modeinfo *modes = (struct drm_mode_modeinfo*)conn2->modes_ptr;
            modeinfo = modes[0]; /* pick first mode */
            free((void*)conn2->modes_ptr);
            free(conn2);
            break;
        }

        free((void*)conn2->modes_ptr);
        free(conn2);
    }

    if (connector_id == 0) {
        fprintf(stderr, "no connected connector found\n");
        return 1;
    }

    printf("Using connector %u, mode %ux%u\n",
           connector_id, modeinfo.hdisplay, modeinfo.vdisplay);

    /* pick a CRTC (first one) */
    uint32_t crtc_id = res.crtc_id_ptr ? ((uint32_t*)res.crtc_id_ptr)[0] : res.crtc_id_ptr;
    if (!crtc_id) crtc_id = ((uint32_t*)res.crtc_id_ptr)[0];

    /* create dumb buffer */
    struct drm_mode_create_dumb creq;
    if (drm_create_dumb(fd, modeinfo.hdisplay, modeinfo.vdisplay, 32, &creq) < 0) {
        perror("CREATE_DUMB");
        return 1;
    }

    /* add framebuffer */
    uint32_t fb_id;
    if (drm_add_fb(fd, creq.width, creq.height, creq.pitch, creq.bpp,
                   24, creq.handle, &fb_id) < 0) {
        perror("ADDFB");
        return 1;
    }

    /* map dumb buffer */
    off_t offset;
    if (drm_map_dumb(fd, creq.handle, &offset) < 0) {
        perror("MAP_DUMB");
        return 1;
    }
    void *map = mmap(0, creq.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* draw gradient */
    uint32_t *pix = map;
    uint32_t pitch_pix = creq.pitch / 4;
    for (uint32_t y = 0; y < creq.height; ++y) {
        for (uint32_t x = 0; x < creq.width; ++x) {
            uint8_t r = (x * 255) / (creq.width - 1);
            uint8_t g = (y * 255) / (creq.height - 1);
            uint8_t b = 0x80;
            pix[y * pitch_pix + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

    /* set CRTC to display our FB */
    if (drm_set_crtc(fd, crtc_id, fb_id, 0, 0, &connector_id, 1, &modeinfo) < 0) {
        perror("SETCRTC");
        return 1;
    }

    printf("Gradient displayed, press Enter to exit\n");
    getchar();

    munmap(map, creq.size);
    drm_destroy_dumb(fd, creq.handle);
    close(fd);
    return 0;
}
