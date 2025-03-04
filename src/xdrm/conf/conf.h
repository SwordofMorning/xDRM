#pragma once

#include "debug.h"
#include "device.h"

#include "../fps/fps.h"
#include "../pattern/pattern.h"

#ifdef __cplusplus
extern "C" {
#endif

struct drm_object
{
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
    uint32_t id;
};

struct modeset_buf
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint32_t handle;
    uint32_t fb;
    uint8_t *map;
};

struct modeset_dev
{
    struct modeset_dev *next;

    unsigned int front_buf;
    struct modeset_buf bufs[2];
    struct drm_object connector;
    struct drm_object crtc;
    struct drm_object plane;

    uint32_t src_width;
    uint32_t src_height;
    int x_offset;
    int y_offset;

    drmModeModeInfo mode;
    uint32_t mode_blob_id;

    bool pflip_pending;
    bool cleanup;

    uint32_t *data_buffer;
    pthread_mutex_t buffer_mutex;
};

#ifdef __cplusplus
}
#endif