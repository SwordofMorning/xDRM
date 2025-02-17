#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <pthread.h>
#include "conf/conf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief xDRM init, Open /dev/dri/card0 then init struct modeset_dev with params
 * 
 * @param dev modeset_dev device pointer
 * @param conn_id connector id
 * @param crtc_id CRTC id
 * @param plane_id plane id
 * @param source_width display width (by pixel) on screen
 * @param source_height display height (by pixel) on screen
 * @param x_offset offset on width
 * @param y_offset offset on height
 * 
 * @return fd or fail
 * @retval -1, Init fail
 * @retval fd, file descriptor of /dev/dri/card0.
 */
int xDRM_Init(struct modeset_dev **dev, uint32_t conn_id, uint32_t crtc_id, uint32_t plane_id, uint32_t source_width, uint32_t source_height, int x_offset, int y_offset);

/**
 * @brief xDRM cleanup, release modeset_dev and close fd
 * 
 * @param fd file descriptor which is created by xDRM_Init
 * @param dev modeset_dev pointer
 */
void xDRM_Exit(int fd, struct modeset_dev *dev);

/**
 * @brief xDRM draw loop, draw dev->data_buffer to panel
 * 
 * @param fd file descriptor which is created by xDRM_Init
 * @param dev modeset_dev pointer
 */
void xDRM_Draw(int fd, struct modeset_dev *dev);

/**
 * @brief Copy data from param to dev->data_buffer
 * 
 * @param dev modeset_dev pointer
 * @param data ARGB array of image
 * @param size array size
 * @return success or not
 * @retval 0, success
 * @retval -EINVAL, fail
 */
int xDRM_Push(struct modeset_dev *dev, uint32_t *data, size_t size);

#ifdef __cplusplus
}
#endif