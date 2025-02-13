#define _GNU_SOURCE
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
#include "/home/xjt/_Workspace_/System/rk3588-linux/buildroot/output/rockchip_rk3588/host/aarch64-buildroot-linux-gnu/sysroot/usr/include/drm/drm_fourcc.h"
#include <iostream>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <math.h>

#define ALGO_SEM_KEY 0x0010
#define ALGO_SHM_YUV_KEY 0x0011
#define ALGO_SHM_FLOAT_KEY 0x0012
#define ALGO_SHM_CSI_KEY 0x0013
#define ALGO_SHM_ALGO_KEY 0x0021
#define ALGO_CSI_SIZE (2592 * 1944 * 1.5)
#define ALGO_WIDTH 640
#define ALGO_HEIGHT 512
#define ALGO_YUV_SIZE (ALGO_WIDTH * ALGO_HEIGHT * 3 / 2)
// 添加共享内存相关变量
struct shared_memory {
    int shmid;
    void* addr;
    int semid;
    int width;
    int height;
};


#define RECT_WIDTH 100
#define RECT_HEIGHT 100
#define NUM_RECTS 8
#define FPS 60

// 设备参数
#define CONN_ID 224  // conn_id_dsi1
#define CRTC_ID 115  // crtc_id_dsi1
#define PLANE_ID 173 // plane_id_dsi1

struct Rectangle {
    int x, y;
    int velo_x, velo_y;
    uint32_t color;
};

// 帧率统计结构体
struct fps_stats {
    struct timeval last_time;    // 上一次统计的时间
    struct timeval current_time; // 当前时间
    int frame_count;            // 帧计数
    float fps;                  // 当前帧率
    float avg_fps;              // 平均帧率
    long total_frames;          // 总帧数
    long total_time;            // 总时间(ms)
};

// 初始化fps统计
static void init_fps_stats(struct fps_stats *stats) {
    gettimeofday(&stats->last_time, NULL);
    stats->frame_count = 0;
    stats->fps = 0.0f;
    stats->avg_fps = 0.0f;
    stats->total_frames = 0;
    stats->total_time = 0;
}

// 更新fps统计
static void update_fps_stats(struct fps_stats *stats) {
    stats->frame_count++;
    stats->total_frames++;
    
    gettimeofday(&stats->current_time, NULL);
    
    // 计算时间差（毫秒）
    long time_diff = (stats->current_time.tv_sec - stats->last_time.tv_sec) * 1000 +
                    (stats->current_time.tv_usec - stats->last_time.tv_usec) / 1000;
    
    // 每秒更新一次FPS
    if (time_diff >= 1000) {
        stats->fps = (float)stats->frame_count * 1000 / time_diff;
        stats->total_time += time_diff;
        stats->avg_fps = (float)stats->total_frames * 1000 / stats->total_time;
        
        // 打印FPS信息
        printf("FPS: %.2f (Current) %.2f (Average) - Frames: %ld Time: %.2fs\n",
               stats->fps,
               stats->avg_fps,
               stats->total_frames,
               stats->total_time / 1000.0f);
        
        // 重置计数器
        stats->frame_count = 0;
        stats->last_time = stats->current_time;
    }
}

struct drm_object {
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
    uint32_t id;
};

struct modeset_buf {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint32_t handle;
    uint32_t fb;
    uint8_t *map;
};

struct modeset_dev {
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

    struct Rectangle rects[NUM_RECTS];

    void* user_data;
};

static struct modeset_dev *modeset_list = NULL;

// 辅助函数：获取属性ID
static uint32_t get_property_id(int fd, drmModeObjectProperties *props, 
                               const char *name) {
    drmModePropertyPtr property;
    uint32_t i, prop_id = 0;

    for (i = 0; i < props->count_props; i++) {
        property = drmModeGetProperty(fd, props->props[i]);
        if (!property)
            continue;

        if (!strcmp(property->name, name))
            prop_id = property->prop_id;
        
        drmModeFreeProperty(property);
        if (prop_id)
            break;
    }
    return prop_id;
}

// 设置对象属性
static int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj,
                                 const char *name, uint64_t value) {
    uint32_t prop_id = 0;
    int i;

    for (i = 0; i < obj->props->count_props; i++) {
        if (!strcmp(obj->props_info[i]->name, name)) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id == 0) {
        fprintf(stderr, "Could not find property %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}

// 初始化DRM对象属性
static void modeset_get_object_properties(int fd, struct drm_object *obj, 
                                        uint32_t type) {
    obj->props = drmModeObjectGetProperties(fd, obj->id, type);
    if (!obj->props) {
        fprintf(stderr, "Cannot get object properties\n");
        return;
    }

    obj->props_info = (drmModePropertyRes **)calloc(obj->props->count_props, sizeof(*obj->props_info));
    for (int i = 0; i < obj->props->count_props; i++) {
        obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
    }
}

// 创建帧缓冲区
static int modeset_create_fb(int fd, struct modeset_buf *buf)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    int ret;

    // 创建ARGB缓冲区
    memset(&creq, 0, sizeof(creq));
    creq.width = buf->width;
    creq.height = buf->height;
    creq.bpp = 32;  // ARGB8888
    creq.flags = 0;
    
    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) {
        fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
        return -errno;
    }
    buf->stride = creq.pitch;
    buf->size = creq.size;
    buf->handle = creq.handle;

    // 创建framebuffer
    uint32_t handles[4] = {buf->handle};
    uint32_t pitches[4] = {buf->stride};
    uint32_t offsets[4] = {0};
    
    ret = drmModeAddFB2(fd, buf->width, buf->height,
                        DRM_FORMAT_ARGB8888, handles, pitches, offsets,
                        &buf->fb, DRM_MODE_FB_MODIFIERS);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
        ret = -errno;
        goto err_destroy;
    }

    // 映射内存
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = buf->handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    buf->map = (uint8_t *)mmap(0, buf->size,
                    PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, mreq.offset);
    if (buf->map == MAP_FAILED) {
        fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    return 0;

err_fb:
    drmModeRmFB(fd, buf->fb);
err_destroy:
    struct drm_mode_destroy_dumb dreq;
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = buf->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    return ret;
}

static int init_shared_memory(struct shared_memory *shm)
{
    shm->width = ALGO_WIDTH;
    shm->height = ALGO_HEIGHT;
    
    // 获取YUV共享内存
    shm->shmid = shmget(ALGO_SHM_YUV_KEY, ALGO_YUV_SIZE, 0666);
    if (shm->shmid < 0) {
        perror("shmget failed");
        return -1;
    }

    // 映射共享内存
    shm->addr = shmat(shm->shmid, NULL, 0);
    if (shm->addr == (void*)-1) {
        perror("shmat failed");
        return -1;
    }

    // 获取信号量
    shm->semid = semget(ALGO_SEM_KEY, 1, 0666);
    if (shm->semid < 0) {
        perror("semget failed");
        return -1;
    }

    return 0;
}

static inline uint8_t clamp(int value)
{
    return value < 0 ? 0 : (value > 255 ? 255 : value);
}

static int frame_count = 0;
#define COLOR_RED  0xFFFF0000
#define COLOR_BLUE 0xFF0000FF

// 直接生成ARGB测试图案
static void generate_test_pattern(uint32_t* argb_data, int width, int height, int frame_count) 
{
    // 简化的时间因子
    int offset = frame_count * 4;  // 增加速度
    
    for(int y = 0; y < height; y++) 
    {
        for(int x = 0; x < width; x++) 
        {
            // 生成RGB分量
            uint8_t r = (x + offset) & 0xFF;
            uint8_t g = (y + offset) & 0xFF;
            uint8_t b = (x + y + offset) & 0xFF;
            
            // 直接组装ARGB
            argb_data[y * width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

// 彩条图案
static void generate_color_bars(uint32_t* argb_data, int width, int height, int frame_count)
{
    int bar_width = width / 8;
    int offset = frame_count * 8;  // 控制移动速度
    
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            int bar = ((x + offset) / bar_width) % 8;
            uint32_t color;
            
            switch(bar)
            {
                case 0: color = 0xFFFFFFFF; break; // 白
                case 1: color = 0xFFFFFF00; break; // 黄
                case 2: color = 0xFF00FFFF; break; // 青
                case 3: color = 0xFF00FF00; break; // 绿
                case 4: color = 0xFFFF00FF; break; // 品红
                case 5: color = 0xFFFF0000; break; // 红
                case 6: color = 0xFF0000FF; break; // 蓝
                case 7: color = 0xFF000000; break; // 黑
            }
            
            argb_data[y * width + x] = color;
        }
    }
}

// 棋盘图案
static void generate_checkerboard(uint32_t* argb_data, int width, int height, int frame_count)
{
    int square_size = 64;  // 更大的方格
    int offset = frame_count * 4;  // 移动速度
    
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            int px = (x + offset) / square_size;
            int py = (y + offset) / square_size;
            
            uint32_t color = ((px + py) & 1) ? 0xFFFFFFFF : 0xFF000000;
            argb_data[y * width + x] = color;
        }
    }
}


#if 0
// Test Patten
static void nv12_to_argb(const uint8_t* nv12_data, uint32_t* argb_data, 
    int width, int height)
{
    // 每60帧切换一次模式
    switch((frame_count / 60) % 3) {
        case 0:
            generate_test_pattern(argb_data, width, height, frame_count);
            break;
        case 1:
            generate_color_bars(argb_data, width, height, frame_count);
            break;
        case 2:
            generate_checkerboard(argb_data, width, height, frame_count);
            break;
    }
    frame_count++;
}
#else
// NV12转ARGB的转换函数
static void nv12_to_argb(const uint8_t* nv12_data, uint32_t* argb_data,
    int width, int height)
{
    const uint8_t* y_plane = nv12_data;
    uint32_t current_color = (frame_count % 2) ? COLOR_RED : COLOR_BLUE;

    for (int i = 0; i < height; i++)
    {
        for (int j = 0; j < width; j++)
        {
            int y_index = i * width + j;
            int y = y_plane[y_index];
            argb_data[i * width + j] = (0xFF << 24) | (y << 16) | (y << 8) | y;
            // argb_data[i * width + j] = current_color;
        }
    }
}
#endif

// 销毁帧缓冲区
static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
    struct drm_mode_destroy_dumb dreq;

    // 取消内存映射
    munmap(buf->map, buf->size);

    // 移除帧缓冲区
    drmModeRmFB(fd, buf->fb);

    // 销毁哑缓冲区
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = buf->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

static int check_plane_capabilities(int fd, struct modeset_dev *dev) {
    drmModePlane *plane = drmModeGetPlane(fd, dev->plane.id);
    if (!plane) {
        fprintf(stderr, "Cannot get plane %u\n", dev->plane.id);
        return -EINVAL;
    }

    // 打印plane信息
    printf("Plane info: id=%u, possible_crtcs=0x%x, formats_count=%u\n",
           plane->plane_id, plane->possible_crtcs, plane->count_formats);

    // 获取资源
    drmModeRes *resources = drmModeGetResources(fd);
    if (!resources) {
        fprintf(stderr, "Cannot get DRM resources\n");
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    // 检查plane是否支持指定的CRTC
    uint32_t crtc_bit = 0;
    bool crtc_found = false;
    
    // 遍历所有encoder找到与CRTC相关的possible_crtcs
    for (int i = 0; i < resources->count_encoders; i++) {
        drmModeEncoder *encoder = drmModeGetEncoder(fd, resources->encoders[i]);
        if (!encoder)
            continue;

        drmModeCrtc *crtc = drmModeGetCrtc(fd, dev->crtc.id);
        if (crtc) {
            // 如果这个encoder当前连接到我们的CRTC
            if (encoder->crtc_id == dev->crtc.id) {
                crtc_bit = encoder->possible_crtcs;
                crtc_found = true;
                drmModeFreeCrtc(crtc);
                drmModeFreeEncoder(encoder);
                break;
            }
            drmModeFreeCrtc(crtc);
        }
        drmModeFreeEncoder(encoder);
    }

    drmModeFreeResources(resources);

    if (!crtc_found) {
        fprintf(stderr, "Could not find encoder for CRTC %u\n", dev->crtc.id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    if (!(plane->possible_crtcs & crtc_bit)) {
        fprintf(stderr, "Plane %u cannot be used with CRTC %u\n",
                plane->plane_id, dev->crtc.id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    // 检查格式支持
    bool format_supported = false;
    for (uint32_t i = 0; i < plane->count_formats; i++) {
        if (plane->formats[i] == DRM_FORMAT_ARGB8888) {
            format_supported = true;
            break;
        }
    }

    if (!format_supported) {
        fprintf(stderr, "Plane %u does not support ARGB8888 format\n",
                plane->plane_id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    drmModeFreePlane(plane);
    return 0;
}

// 初始化设备
static int modeset_setup_dev(int fd, struct modeset_dev *dev)
{
    int ret;
    
    // 使用固定的设备ID
    dev->connector.id = CONN_ID;
    dev->crtc.id = CRTC_ID;
    dev->plane.id = PLANE_ID;

    // 检查plane能力
    ret = check_plane_capabilities(fd, dev);
    if (ret < 0) {
        fprintf(stderr, "Plane capability check failed\n");
        return ret;
    }

    // 获取连接器信息
    drmModeConnector *conn = drmModeGetConnector(fd, dev->connector.id);
    if (!conn) {
        fprintf(stderr, "cannot get connector %u: %m\n", dev->connector.id);
        return -errno;
    }

    // 获取当前显示模式
    if (conn->count_modes <= 0) {
        fprintf(stderr, "no valid mode for connector %u\n", dev->connector.id);
        ret = -EFAULT;
        goto err_free;
    }
    
    memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
    // dev->bufs[0].width = conn->modes[0].hdisplay;
    // dev->bufs[0].height = conn->modes[0].vdisplay;
    // dev->bufs[1].width = conn->modes[0].hdisplay;
    // dev->bufs[1].height = conn->modes[0].vdisplay;

    dev->bufs[0].width = ALGO_WIDTH;   // 使用 640
    dev->bufs[0].height = ALGO_HEIGHT;  // 使用 512
    dev->bufs[1].width = ALGO_WIDTH;
    dev->bufs[1].height = ALGO_HEIGHT;

    // 创建模式blob
    ret = drmModeCreatePropertyBlob(fd, &dev->mode, sizeof(dev->mode),
                                   &dev->mode_blob_id);
    if (ret) {
        fprintf(stderr, "cannot create mode blob: %m\n");
        goto err_free;
    }

    // 获取对象属性
    modeset_get_object_properties(fd, &dev->connector, DRM_MODE_OBJECT_CONNECTOR);
    modeset_get_object_properties(fd, &dev->crtc, DRM_MODE_OBJECT_CRTC);
    modeset_get_object_properties(fd, &dev->plane, DRM_MODE_OBJECT_PLANE);

    // 创建帧缓冲区
    ret = modeset_create_fb(fd, &dev->bufs[0]);
    if (ret)
        goto err_blob;

    ret = modeset_create_fb(fd, &dev->bufs[1]);
    if (ret)
        goto err_fb0;

    // // 初始化矩形
    // for (int i = 0; i < NUM_RECTS; ++i) {
    //     dev->rects[i].x = rand() % (dev->bufs[0].width - RECT_WIDTH);
    //     dev->rects[i].y = rand() % (dev->bufs[0].height - RECT_HEIGHT);
    //     dev->rects[i].velo_x = (rand() % 10) + 5;
    //     dev->rects[i].velo_y = (rand() % 10) + 5;
    //     dev->rects[i].color = 0x80FFFFFF;  // 半透明白色
    // }

    drmModeFreeConnector(conn);
    return 0;

err_fb0:
    modeset_destroy_fb(fd, &dev->bufs[0]);
err_blob:
    drmModeDestroyPropertyBlob(fd, dev->mode_blob_id);
err_free:
    drmModeFreeConnector(conn);
    return ret;
}

// 准备原子提交的属性设置
static int modeset_atomic_prepare_commit(int fd, struct modeset_dev *dev,
    drmModeAtomicReq *req)
{
    struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];
    int ret;

    // 只设置必要的plane属性
    ret = set_drm_object_property(req, &dev->plane, "FB_ID", buf->fb);
    if (ret < 0) return ret;

    ret = set_drm_object_property(req, &dev->plane, "CRTC_ID", dev->crtc.id);
    if (ret < 0) return ret;

    // 源和目标区域设置
    // ret = set_drm_object_property(req, &dev->plane, "SRC_X", 0);
    // ret |= set_drm_object_property(req, &dev->plane, "SRC_Y", 0);
    // ret |= set_drm_object_property(req, &dev->plane, "SRC_W", buf->width << 16);
    // ret |= set_drm_object_property(req, &dev->plane, "SRC_H", buf->height << 16);
    // if (ret < 0) return ret;

    // ret = set_drm_object_property(req, &dev->plane, "CRTC_X", 0);
    // ret |= set_drm_object_property(req, &dev->plane, "CRTC_Y", 0);
    // ret |= set_drm_object_property(req, &dev->plane, "CRTC_W", buf->width);
    // ret |= set_drm_object_property(req, &dev->plane, "CRTC_H", buf->height);
    // if (ret < 0) return ret;

    ret = set_drm_object_property(req, &dev->plane, "SRC_X", 0);
    ret |= set_drm_object_property(req, &dev->plane, "SRC_Y", 0);
    ret |= set_drm_object_property(req, &dev->plane, "SRC_W", ALGO_WIDTH << 16);
    ret |= set_drm_object_property(req, &dev->plane, "SRC_H", ALGO_HEIGHT << 16);
    if (ret < 0) return ret;

    // 设置显示区域（居中显示）
    int disp_width = 1080;
    int disp_height = 1920;
    int x_offset = (disp_width - ALGO_WIDTH) / 2;
    int y_offset = (disp_height - ALGO_HEIGHT) / 2;

    // 修改显示属性
    ret = set_drm_object_property(req, &dev->plane, "CRTC_X", x_offset);
    ret |= set_drm_object_property(req, &dev->plane, "CRTC_Y", y_offset);
    ret |= set_drm_object_property(req, &dev->plane, "CRTC_W", ALGO_WIDTH);
    ret |= set_drm_object_property(req, &dev->plane, "CRTC_H", ALGO_HEIGHT);

    ret = set_drm_object_property(req, &dev->plane, "zpos", 0);
    if (ret < 0) {
        fprintf(stderr, "Note: zpos property not supported\n");
    }

    return 0;
}

// 执行原子模式设置
static int modeset_atomic_commit(int fd, struct modeset_dev *dev, uint32_t flags)
{
    drmModeAtomicReq *req;
    int ret;

    req = drmModeAtomicAlloc();
    if (!req) {
        fprintf(stderr, "Failed to allocate atomic request\n");
        return -ENOMEM;
    }

    ret = modeset_atomic_prepare_commit(fd, dev, req);
    if (ret < 0) {
        fprintf(stderr, "Failed to prepare atomic commit for plane %u\n", dev->plane.id);
        drmModeAtomicFree(req);
        return ret;
    }

    // 去掉NONBLOCK标志，使用同步提交
    flags &= ~DRM_MODE_ATOMIC_NONBLOCK;
    
    ret = drmModeAtomicCommit(fd, req, flags, dev);
    if (ret < 0) {
        fprintf(stderr, "Failed to commit atomic request for plane %u: %s\n",
                dev->plane.id, strerror(errno));
    }

    drmModeAtomicFree(req);
    return ret;
}

// 执行初始模式设置
static int modeset_atomic_modeset(int fd, struct modeset_dev *dev)
{
    int ret;
    uint32_t flags;

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        return -ENOMEM;
    }

    ret = modeset_atomic_prepare_commit(fd, dev, req);
    if (ret < 0) {
        drmModeAtomicFree(req);
        return ret;
    }

    // 使用最小权限标志
    flags = DRM_MODE_ATOMIC_NONBLOCK;
    ret = drmModeAtomicCommit(fd, req, flags, dev);
    if (ret < 0) {
        printf("Atomic modeset failed: %s\n", strerror(errno));
    }

    drmModeAtomicFree(req);
    return ret;
}

// 执行页面翻转
static int modeset_atomic_page_flip(int fd, struct modeset_dev *dev)
{
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
    return modeset_atomic_commit(fd, dev, flags);
}

// 设置原子模式的初始化
static int modeset_atomic_init(int fd)
{
    uint64_t cap;
    int ret;

    // 检查并启用原子能力
    ret = drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap);
    if (ret || !cap) {
    fprintf(stderr, "Device does not support atomic modesetting\n");
    return -ENOTSUP;
    }

    // 启用通用平面功能
    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret) {
    fprintf(stderr, "Failed to set universal planes cap\n");
    return ret;
    }

    // 启用原子功能
    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret) {
    fprintf(stderr, "Failed to set atomic cap\n");
    return ret;
    }

    return 0;
}

// 更新矩形位置
static void update_rect_positions(struct modeset_dev *dev)
{
    struct modeset_buf *buf = &dev->bufs[0]; // 使用第一个缓冲区的尺寸作为边界
    
    for (int i = 0; i < NUM_RECTS; i++) {
        // 更新位置
        dev->rects[i].x += dev->rects[i].velo_x;
        dev->rects[i].y += dev->rects[i].velo_y;

        // 边界碰撞检测和反弹
        if (dev->rects[i].x <= 0 || dev->rects[i].x + RECT_WIDTH >= buf->width) {
            dev->rects[i].velo_x = -dev->rects[i].velo_x;
            dev->rects[i].x = (dev->rects[i].x <= 0) ? 0 : buf->width - RECT_WIDTH;
        }
        
        if (dev->rects[i].y <= 0 || dev->rects[i].y + RECT_HEIGHT >= buf->height) {
            dev->rects[i].velo_y = -dev->rects[i].velo_y;
            dev->rects[i].y = (dev->rects[i].y <= 0) ? 0 : buf->height - RECT_HEIGHT;
        }
    }
}

// 绘制矩形到缓冲区
static void draw_rects(struct modeset_dev *dev)
{
    struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];
    uint32_t *ptr = (uint32_t *)buf->map;
    
    // 清空缓冲区（全透明黑色）
    memset(buf->map, 0, buf->size);
    
    // 绘制所有矩形
    for (int i = 0; i < NUM_RECTS; i++) {
        for (int y = 0; y < RECT_HEIGHT; y++) {
            for (int x = 0; x < RECT_WIDTH; x++) {
                int pos_x = dev->rects[i].x + x;
                int pos_y = dev->rects[i].y + y;
                
                if (pos_x >= 0 && pos_x < buf->width && 
                    pos_y >= 0 && pos_y < buf->height) {
                    ptr[pos_y * (buf->stride / 4) + pos_x] = dev->rects[i].color;
                }
            }
        }
    }
}

// 页面翻转事件处理函数
static void page_flip_handler(int fd, unsigned int frame,
    unsigned int sec, unsigned int usec,
    unsigned int crtc_id, void *data)
{
    struct modeset_dev *dev = (struct modeset_dev *)data;
    struct shared_memory *shm = (struct shared_memory *)dev->user_data;
    struct sembuf sem_op;
    
    dev->pflip_pending = false;

    if (!dev->cleanup) {
        // 等待信号量
        sem_op.sem_num = 0;
        sem_op.sem_op = -1;
        sem_op.sem_flg = 0;
        if (semop(shm->semid, &sem_op, 1) >= 0) {
            // 获取当前缓冲区
            struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];
            
            // 转换并复制图像数据
            nv12_to_argb((uint8_t*)shm->addr, (uint32_t*)buf->map, 640, 512);

            // 释放信号量
            sem_op.sem_op = 1;
            semop(shm->semid, &sem_op, 1);

            // 提交新帧
            int ret = modeset_atomic_page_flip(fd, dev);
            if (ret >= 0) {
                dev->front_buf ^= 1;
                dev->pflip_pending = true;
                
                // 增加帧计数
                frame_count++;
                
                // 可选：添加延时控制
                usleep(16666); // 约60fps
            }
        }
    }
}

// 主绘制循环
static void modeset_draw(int fd, struct modeset_dev *dev)
{
    struct pollfd fds[1];
    int ret;
    struct fps_stats fps_stats;
    
    // 初始化事件上下文
    drmEventContext ev = {};
    memset(&ev, 0, sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler2 = page_flip_handler;
    ev.vblank_handler = NULL;
    
    // 初始化FPS统计
    init_fps_stats(&fps_stats);
    
    // 设置DRM事件的文件描述符
    fds[0].fd = fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    // 初始绘制
    update_rect_positions(dev);
    draw_rects(dev);
    
    // 执行初始页面翻转
re_flip:
    ret = modeset_atomic_page_flip(fd, dev);
    if (ret) {
        fprintf(stderr, "Initial page flip failed: %s\n", strerror(errno));
        goto re_flip;
    }
    
    dev->front_buf ^= 1;
    dev->pflip_pending = true;

    // 主循环
    while (1) {
        fds[0].revents = 0;

        ret = poll(fds, 1, -1);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            printf("poll failed: %s\n", strerror(errno));
            break;
        }

        if (fds[0].revents & POLLIN) {
            ret = drmHandleEvent(fd, &ev);
            if (ret != 0) {
                printf("drmHandleEvent failed: %s\n", strerror(errno));
                break;
            }
            
            // 更新FPS统计
            update_fps_stats(&fps_stats);
        }
    }
}

// 清理函数
static void modeset_cleanup(int fd, struct modeset_dev *dev)
{
    drmEventContext ev = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler2 = page_flip_handler,
    };

    dev->cleanup = true;

    while (dev->pflip_pending) {
        drmHandleEvent(fd, &ev);
    }

    // 清理plane
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (req) {
        // 只清理plane，不触碰CRTC
        set_drm_object_property(req, &dev->plane, "FB_ID", 0);
        set_drm_object_property(req, &dev->plane, "CRTC_ID", 0);
        drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
        drmModeAtomicFree(req);
    }

    // 清理资源
    modeset_destroy_fb(fd, &dev->bufs[0]);
    modeset_destroy_fb(fd, &dev->bufs[1]);
    drmModeDestroyPropertyBlob(fd, dev->mode_blob_id);

    // 清理属性资源
    for (int i = 0; i < dev->connector.props->count_props; i++)
        drmModeFreeProperty(dev->connector.props_info[i]);
    for (int i = 0; i < dev->crtc.props->count_props; i++)
        drmModeFreeProperty(dev->crtc.props_info[i]);
    for (int i = 0; i < dev->plane.props->count_props; i++)
        drmModeFreeProperty(dev->plane.props_info[i]);

    free(dev->connector.props_info);
    free(dev->crtc.props_info);
    free(dev->plane.props_info);

    drmModeFreeObjectProperties(dev->connector.props);
    drmModeFreeObjectProperties(dev->crtc.props);
    drmModeFreeObjectProperties(dev->plane.props);
}

int main(int argc, char *argv[])
{
    int fd, ret;
    struct modeset_dev *dev;
    struct shared_memory shm;

    // 初始化共享内存
    ret = init_shared_memory(&shm);
    if (ret < 0) {
        fprintf(stderr, "Failed to init shared memory\n");
        return -1;
    }

    std::cout << "Open DRM" << std::endl;

    // 打开DRM设备
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open card: %s\n", strerror(errno));
        return 1;
    }

    std::cout << "Set Atomic" << std::endl;

    // 初始化原子模式设置
    ret = modeset_atomic_init(fd);
    if (ret) {
        close(fd);
        return ret;
    }

    std::cout << "Malloc device" << std::endl;

    // 分配设备结构
    dev = (struct modeset_dev *)malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));

    std::cout << "Set device" << std::endl;

    // 设置设备
    ret = modeset_setup_dev(fd, dev);
    if (ret) {
        free(dev);
        close(fd);
        return ret;
    }

    std::cout << "Init" << std::endl;

    // 执行初始模式设置
    ret = modeset_atomic_modeset(fd, dev);
    if (ret) {
        modeset_cleanup(fd, dev);
        free(dev);
        close(fd);
        return ret;
    }

    std::cout << "Main loop" << std::endl;

    // 运行主绘制循环
    dev->user_data = &shm;
    modeset_draw(fd, dev);

    std::cout << "Out loop" << std::endl;

    // 清理
    if (shm.addr != (void*)-1)
        shmdt(shm.addr);
    modeset_cleanup(fd, dev);
    free(dev);
    close(fd);

    return 0;
}