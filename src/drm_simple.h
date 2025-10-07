#ifndef DRM_SIMPLE_H
#define DRM_SIMPLE_H

#include <stdint.h>

/* existing API */
int drm_setup(void);
void drm_teardown(void);
int drm_present_solid(uint32_t r, uint32_t g, uint32_t b);

/* new: copy a client-provided ARGB8888 buffer into the back buffer and pageflip.
 * src: pointer to client data (in client bytes per row src_stride)
 * src_stride: bytes per row in the source (client stride)
 * width: width in pixels to copy (should match mode.hdisplay)
 * height: height in pixels to copy
 *
 * returns 0 on success.
 */
int drm_present_from_shm(const void *src, uint32_t src_stride, uint32_t width, uint32_t height);

#endif
