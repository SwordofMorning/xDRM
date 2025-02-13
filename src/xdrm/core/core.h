#define _GNU_SOURCE
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <time.h>
#include <poll.h>
#include <sys/poll.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <math.h>
#include "../conf/conf.h"

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

    drmModeModeInfo mode;
    uint32_t mode_blob_id;

    bool pflip_pending;
    bool cleanup;

    void* user_data;
};

extern struct modeset_dev *modeset_list;

int modeset_atomic_init(int fd);

int modeset_setup_dev(int fd, struct modeset_dev *dev, uint32_t conn_id, uint32_t crtc_id, uint32_t plane_id, 
    uint32_t source_width, uint32_t source_height);

int modeset_atomic_modeset(int fd, struct modeset_dev *dev, 
    uint32_t source_width, uint32_t source_height, int x_offset, int y_offset);

void modeset_cleanup(int fd, struct modeset_dev *dev);

void modeset_draw(int fd, struct modeset_dev *dev, uint32_t source_width, uint32_t source_height, int x_offset, int y_offset);

#ifdef __cplusplus
}
#endif