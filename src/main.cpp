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

    // 创建哑缓冲区
    memset(&creq, 0, sizeof(creq));
    creq.width = buf->width;
    creq.height = buf->height;
    creq.bpp = 32;  // ARGB8888 格式
    creq.flags = 0;
    
    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) {
        fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
        return -errno;
    }
    buf->stride = creq.pitch;
    buf->size = creq.size;
    buf->handle = creq.handle;

    // 创建帧缓冲区
    uint32_t handles[4] = {buf->handle};
    uint32_t pitches[4] = {buf->stride};
    uint32_t offsets[4] = {0};
    
    // 使用 ARGB8888 格式创建帧缓冲，支持 alpha 通道
    ret = drmModeAddFB2(fd, buf->width, buf->height,
                        DRM_FORMAT_ARGB8888, handles, pitches, offsets,
                        &buf->fb, 0);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
        ret = -errno;
        goto err_destroy;
    }

    // 准备内存映射
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = buf->handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    // 执行内存映射
    buf->map = (uint8_t *)mmap(0, buf->size,
                    PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, mreq.offset);
    if (buf->map == MAP_FAILED) {
        fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    // 清空帧缓冲区
    memset(buf->map, 0, buf->size);

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

// 初始化设备
static int modeset_setup_dev(int fd, struct modeset_dev *dev)
{
    int ret;
    
    // 使用固定的设备ID
    dev->connector.id = CONN_ID;
    dev->crtc.id = CRTC_ID;
    dev->plane.id = PLANE_ID;

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
    dev->bufs[0].width = conn->modes[0].hdisplay;
    dev->bufs[0].height = conn->modes[0].vdisplay;
    dev->bufs[1].width = conn->modes[0].hdisplay;
    dev->bufs[1].height = conn->modes[0].vdisplay;

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

    // 初始化矩形
    for (int i = 0; i < NUM_RECTS; ++i) {
        dev->rects[i].x = rand() % (dev->bufs[0].width - RECT_WIDTH);
        dev->rects[i].y = rand() % (dev->bufs[0].height - RECT_HEIGHT);
        dev->rects[i].velo_x = (rand() % 10) + 5;
        dev->rects[i].velo_y = (rand() % 10) + 5;
        dev->rects[i].color = 0x80FFFFFF;  // 半透明白色
    }

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

    // 设置连接器属性
    ret = set_drm_object_property(req, &dev->connector, "CRTC_ID", dev->crtc.id);
    if (ret < 0) {
    fprintf(stderr, "Failed to set CRTC_ID property\n");
    return ret;
    }

    // 设置CRTC属性
    ret = set_drm_object_property(req, &dev->crtc, "MODE_ID", dev->mode_blob_id);
    if (ret < 0) {
    fprintf(stderr, "Failed to set MODE_ID property\n");
    return ret;
    }

    ret = set_drm_object_property(req, &dev->crtc, "ACTIVE", 1);
    if (ret < 0) {
    fprintf(stderr, "Failed to set ACTIVE property\n");
    return ret;
    }

    // 设置平面属性
    ret = set_drm_object_property(req, &dev->plane, "FB_ID", buf->fb);
    if (ret < 0) {
    fprintf(stderr, "Failed to set FB_ID property\n");
    return ret;
    }

    ret = set_drm_object_property(req, &dev->plane, "CRTC_ID", dev->crtc.id);
    if (ret < 0) {
    fprintf(stderr, "Failed to set plane CRTC_ID property\n");
    return ret;
    }

    // 设置平面源属性
    ret = set_drm_object_property(req, &dev->plane, "SRC_X", 0);
    ret |= set_drm_object_property(req, &dev->plane, "SRC_Y", 0);
    ret |= set_drm_object_property(req, &dev->plane, "SRC_W", buf->width << 16);
    ret |= set_drm_object_property(req, &dev->plane, "SRC_H", buf->height << 16);
    if (ret < 0) {
    fprintf(stderr, "Failed to set plane SRC properties\n");
    return ret;
    }

    // 设置平面目标属性
    ret = set_drm_object_property(req, &dev->plane, "CRTC_X", 0);
    ret |= set_drm_object_property(req, &dev->plane, "CRTC_Y", 0);
    ret |= set_drm_object_property(req, &dev->plane, "CRTC_W", buf->width);
    ret |= set_drm_object_property(req, &dev->plane, "CRTC_H", buf->height);
    if (ret < 0) {
    fprintf(stderr, "Failed to set plane CRTC properties\n");
    return ret;
    }

    // 设置平面的alpha混合属性（如果支持）
    ret = set_drm_object_property(req, &dev->plane, "alpha", 0xFFFF);
    if (ret < 0) {
    // 如果不支持alpha属性，忽略错误
    fprintf(stderr, "Note: alpha property not supported\n");
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

    // 准备原子提交
    ret = modeset_atomic_prepare_commit(fd, dev, req);
    if (ret < 0) {
    fprintf(stderr, "Failed to prepare atomic commit\n");
    drmModeAtomicFree(req);
    return ret;
    }

    // 执行原子提交
    ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret < 0) {
    fprintf(stderr, "Failed to commit atomic request: %s\n", strerror(errno));
    }

    drmModeAtomicFree(req);
    return ret;
}

// 执行初始模式设置
static int modeset_atomic_modeset(int fd, struct modeset_dev *dev)
{
    int ret;
    uint32_t flags;

    // 准备测试提交
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
    return -ENOMEM;
    }

    ret = modeset_atomic_prepare_commit(fd, dev, req);
    if (ret < 0) {
    drmModeAtomicFree(req);
    return ret;
    }

    // 首先执行测试提交
    flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret < 0) {
    fprintf(stderr, "Test-only atomic commit failed: %s\n", strerror(errno));
    drmModeAtomicFree(req);
    return ret;
    }

    // 执行实际的模式设置
    flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
    ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret < 0) {
    fprintf(stderr, "Atomic modeset failed: %s\n", strerror(errno));
    }

    drmModeAtomicFree(req);
    return ret;
}

// 执行页面翻转
static int modeset_atomic_page_flip(int fd, struct modeset_dev *dev)
{
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
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
    if (!dev) {
        printf("Invalid device data in page flip handler\n");
        return;
    }

    // 标记页面翻转完成
    dev->pflip_pending = false;

    // 如果不是清理阶段，继续下一帧
    if (!dev->cleanup) {
        // 更新矩形位置
        update_rect_positions(dev);

        // 绘制新帧
        draw_rects(dev);

        // 请求新的页面翻转
        if (modeset_atomic_page_flip(fd, dev) < 0) {
            printf("Failed to queue page flip\n");
            return;
        }

        // 切换前后缓冲区
        dev->front_buf ^= 1;
        dev->pflip_pending = true;
    }
}

// 主绘制循环
static void modeset_draw(int fd, struct modeset_dev *dev)
{
    struct pollfd fds[2];
    int ret;
    
    // 完整初始化事件上下文
    drmEventContext ev = {};
    memset(&ev, 0, sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler2 = page_flip_handler;
    ev.vblank_handler = NULL;  // 明确设置为NULL
    
    // 设置轮询描述符
    fds[0].fd = 0;
    fds[0].events = POLLIN;
    fds[0].revents = 0;  // 初始化revents
    fds[1].fd = fd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;  // 初始化revents

    // 初始绘制
    update_rect_positions(dev);
    draw_rects(dev);
    
    // 执行初始页面翻转
    ret = modeset_atomic_page_flip(fd, dev);
    if (ret) {
        fprintf(stderr, "Initial page flip failed: %s\n", strerror(errno));
        return;
    }
    
    dev->front_buf ^= 1;
    dev->pflip_pending = true;

    // 主循环
    while (1) {
        // 重置revents
        fds[0].revents = 0;
        fds[1].revents = 0;
        
        std::cout << "loop 1" << std::endl;

        ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR)
                continue;  // 处理中断
            printf("poll failed: %s\n", strerror(errno));
            break;
        }

        std::cout << "loop 2" << std::endl;

        if (fds[0].revents & POLLIN) {
            // 用户输入，退出循环
            break;
        }

        std::cout << "loop 3" << std::endl;

        if (fds[1].revents & POLLIN) {
            // DRM事件
            ret = drmHandleEvent(fd, &ev);
            if (ret != 0) {
                printf("drmHandleEvent failed: %s\n", strerror(errno));
                break;
            }
        }

        std::cout << "loop 4" << std::endl;
    }
}

// 清理函数
static void modeset_cleanup(int fd, struct modeset_dev *dev)
{
    drmEventContext ev = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler2 = page_flip_handler,
    };

    // 标记清理状态
    dev->cleanup = true;

    // 等待pending的页面翻转完成
    while (dev->pflip_pending) {
        drmHandleEvent(fd, &ev);
    }

    // 禁用CRTC
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (req) {
        set_drm_object_property(req, &dev->crtc, "ACTIVE", 0);
        drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
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
    modeset_draw(fd, dev);

    std::cout << "Out loop" << std::endl;

    // 清理
    modeset_cleanup(fd, dev);
    free(dev);
    close(fd);

    return 0;
}