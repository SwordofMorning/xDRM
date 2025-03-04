#include "xdrm.h"

/* ======================================================================================================================= */
/* ================================================== Section 1 : Basic ================================================== */
/* ======================================================================================================================= */

static uint32_t xDRM_Get_Property_ID(int fd, drmModeObjectProperties *props, const char *name)
{
    drmModePropertyPtr property;
    uint32_t i, prop_id = 0;

    for (i = 0; i < props->count_props; i++)
    {
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

static int xDRM_Set_DRM_Object_Property(drmModeAtomicReq *req, struct drm_object *obj, const char *name, uint64_t value)
{
    uint32_t prop_id = 0;
    int i;

    for (i = 0; i < obj->props->count_props; i++)
    {
        if (!strcmp(obj->props_info[i]->name, name))
        {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id == 0)
    {
        fprintf(stderr, "Could not find property %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}

static void xDRM_Modeset_Get_Object_Properties(int fd, struct drm_object *obj, uint32_t type)
{
    obj->props = drmModeObjectGetProperties(fd, obj->id, type);
    if (!obj->props)
    {
        fprintf(stderr, "Cannot get object properties\n");
        return;
    }

    obj->props_info = (drmModePropertyRes **)calloc(obj->props->count_props, sizeof(*obj->props_info));
    for (int i = 0; i < obj->props->count_props; i++)
    {
        obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
    }
}

static int xDRM_Modeset_Create_FB(int fd, struct modeset_buf *buf)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    int ret;

    // create ARGB buffer
    memset(&creq, 0, sizeof(creq));
    creq.width = buf->width;
    creq.height = buf->height;
    creq.bpp = 32;
    creq.flags = 0;

    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0)
    {
        fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
        return -errno;
    }

    buf->stride = creq.pitch;
    buf->size = creq.size;
    buf->handle = creq.handle;

    // crate framebuffer
    uint32_t handles[4] = {buf->handle};
    uint32_t pitches[4] = {buf->stride};
    uint32_t offsets[4] = {0};

    ret = drmModeAddFB2(fd, buf->width, buf->height, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &buf->fb, DRM_MODE_FB_MODIFIERS);

    if (ret)
    {
        fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
        ret = -errno;
        goto err_destroy;
    }

    // memory map
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = buf->handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

    if (ret)
    {
        fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    buf->map = (uint8_t *)mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (buf->map == MAP_FAILED)
    {
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

static void xDRM_Modeset_Destroy_FB(int fd, struct modeset_buf *buf)
{
    struct drm_mode_destroy_dumb dreq;

    // unmap
    munmap(buf->map, buf->size);

    // remove fb
    drmModeRmFB(fd, buf->fb);

    // destroy buffer
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = buf->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

static int xDRM_Check_Plane_Capabilities(int fd, struct modeset_dev *dev)
{
    drmModePlane *plane = drmModeGetPlane(fd, dev->plane.id);
    if (!plane)
    {
        fprintf(stderr, "Cannot get plane %u\n", dev->plane.id);
        return -EINVAL;
    }

#if __ENABLE_DEBUG_LOG__
    printf("Plane info: id=%u, possible_crtcs=0x%x, formats_count=%u\n", plane->plane_id, plane->possible_crtcs, plane->count_formats);
#endif

    // get resource
    drmModeRes *resources = drmModeGetResources(fd);
    if (!resources)
    {
        fprintf(stderr, "Cannot get DRM resources\n");
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    // checks whether the plane supports the specified CRTC
    uint32_t crtc_bit = 0;
    bool crtc_found = false;

    // traverse all encoders to find possible_crtcs related to CRTC
    for (int i = 0; i < resources->count_encoders; i++)
    {
        drmModeEncoder *encoder = drmModeGetEncoder(fd, resources->encoders[i]);
        if (!encoder)
            continue;

        drmModeCrtc *crtc = drmModeGetCrtc(fd, dev->crtc.id);
        if (crtc)
        {
            // if this encoder is currently connected to our CRTC
            if (encoder->crtc_id == dev->crtc.id)
            {
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

    if (!crtc_found)
    {
        fprintf(stderr, "Could not find encoder for CRTC %u\n", dev->crtc.id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    if (!(plane->possible_crtcs & crtc_bit))
    {
        fprintf(stderr, "Plane %u cannot be used with CRTC %u\n", plane->plane_id, dev->crtc.id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    // checkout format
    bool format_supported = false;
    for (uint32_t i = 0; i < plane->count_formats; i++)
    {
        if (plane->formats[i] == DRM_FORMAT_ARGB8888)
        {
            format_supported = true;
            break;
        }
    }

    if (!format_supported)
    {
        fprintf(stderr, "Plane %u does not support ARGB8888 format\n", plane->plane_id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    drmModeFreePlane(plane);
    return 0;
}

/* ======================================================================================================================== */
/* ================================================== Section 2 : Atomic ================================================== */
/* ======================================================================================================================== */

// clang-format off
static int xDRM_Modeset_Atomic_Prepare_Commit(int fd, struct modeset_dev *dev, drmModeAtomicReq *req,
    uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
// clang-format on
{
    struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];
    int ret;

    // only set necessary plane properties
    ret = xDRM_Set_DRM_Object_Property(req, &dev->plane, "FB_ID", buf->fb);
    if (ret < 0)
        return ret;

    ret = xDRM_Set_DRM_Object_Property(req, &dev->plane, "CRTC_ID", dev->crtc.id);
    if (ret < 0)
        return ret;

    // set source property
    ret = xDRM_Set_DRM_Object_Property(req, &dev->plane, "SRC_X", 0);
    ret |= xDRM_Set_DRM_Object_Property(req, &dev->plane, "SRC_Y", 0);
    ret |= xDRM_Set_DRM_Object_Property(req, &dev->plane, "SRC_W", source_width << 16);
    ret |= xDRM_Set_DRM_Object_Property(req, &dev->plane, "SRC_H", source_height << 16);
    if (ret < 0)
        return ret;

    // set display property
    ret = xDRM_Set_DRM_Object_Property(req, &dev->plane, "CRTC_X", x_offset);
    ret |= xDRM_Set_DRM_Object_Property(req, &dev->plane, "CRTC_Y", y_offset);
    ret |= xDRM_Set_DRM_Object_Property(req, &dev->plane, "CRTC_W", source_width);
    ret |= xDRM_Set_DRM_Object_Property(req, &dev->plane, "CRTC_H", source_height);

    // zpos
    ret = xDRM_Set_DRM_Object_Property(req, &dev->plane, "zpos", 0);
    if (ret < 0)
    {
        fprintf(stderr, "Note: zpos property not supported\n");
    }

    return 0;
}

// clang-format off
static int xDRM_Modeset_Atomic_Commit(int fd, struct modeset_dev *dev, uint32_t flags, 
    uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
// clang-format on
{
    drmModeAtomicReq *req;
    int ret;

    req = drmModeAtomicAlloc();
    if (!req)
    {
        fprintf(stderr, "Failed to allocate atomic request\n");
        return -ENOMEM;
    }

    ret = xDRM_Modeset_Atomic_Prepare_Commit(fd, dev, req, source_width, source_height, x_offset, y_offset);
    if (ret < 0)
    {
        fprintf(stderr, "Failed to prepare atomic commit for plane %u\n", dev->plane.id);
        drmModeAtomicFree(req);
        return ret;
    }

    // @note without the NONBLOCK flag and use synchronous commit
    flags &= ~DRM_MODE_ATOMIC_NONBLOCK;

    ret = drmModeAtomicCommit(fd, req, flags, dev);
    if (ret < 0)
    {
        fprintf(stderr, "Failed to commit atomic request for plane %u: %s\n", dev->plane.id, strerror(errno));
    }

    drmModeAtomicFree(req);
    return ret;
}

int xDRM_Modeset_Atomic_Page_Flip(int fd, struct modeset_dev *dev, uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
{
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
    return xDRM_Modeset_Atomic_Commit(fd, dev, flags, source_width, source_height, x_offset, y_offset);
}

/* ====================================================================================================================== */
/* ================================================== Section 3 : Wrap ================================================== */
/* ====================================================================================================================== */

// clang-format off
static int xDRM_Modeset_Setup_Dev(int fd, struct modeset_dev *dev,
    uint32_t conn_id, uint32_t crtc_id, uint32_t plane_id, uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
// clang-format on
{
    int ret;

    dev->connector.id = conn_id;
    dev->crtc.id = crtc_id;
    dev->plane.id = plane_id;

    // Step 1 : checkout plane
    ret = xDRM_Check_Plane_Capabilities(fd, dev);
    if (ret < 0)
    {
        fprintf(stderr, "Plane capability check failed\n");
        return ret;
    }

    // Step 2 : get connector information
    drmModeConnector *conn = drmModeGetConnector(fd, dev->connector.id);
    if (!conn)
    {
        fprintf(stderr, "cannot get connector %u: %m\n", dev->connector.id);
        return -errno;
    }

    // Step 3 : get the current display mode
    if (conn->count_modes <= 0)
    {
        fprintf(stderr, "no valid mode for connector %u\n", dev->connector.id);
        ret = -EFAULT;
        goto err_free;
    }

    memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));

    // Step 4 : set buffer display size, @note source!
    dev->bufs[0].width = source_width;
    dev->bufs[0].height = source_height;
    dev->bufs[1].width = source_width;
    dev->bufs[1].height = source_height;

    dev->src_width = source_width;
    dev->src_height = source_height;
    dev->x_offset = x_offset;
    dev->y_offset = y_offset;

    // Step 5 : set property blob
    ret = drmModeCreatePropertyBlob(fd, &dev->mode, sizeof(dev->mode), &dev->mode_blob_id);
    if (ret)
    {
        fprintf(stderr, "cannot create mode blob: %m\n");
        goto err_free;
    }

    // Step 6 : get properties
    xDRM_Modeset_Get_Object_Properties(fd, &dev->connector, DRM_MODE_OBJECT_CONNECTOR);
    xDRM_Modeset_Get_Object_Properties(fd, &dev->crtc, DRM_MODE_OBJECT_CRTC);
    xDRM_Modeset_Get_Object_Properties(fd, &dev->plane, DRM_MODE_OBJECT_PLANE);

    // Step 7 : create frame buffer
    ret = xDRM_Modeset_Create_FB(fd, &dev->bufs[0]);
    if (ret)
        goto err_blob;

    ret = xDRM_Modeset_Create_FB(fd, &dev->bufs[1]);
    if (ret)
        goto err_fb0;

    // Step 8 : User buffer and mutex
    dev->data_buffer = (uint32_t *)malloc(source_width * source_height * sizeof(uint32_t));
    if (!dev->data_buffer)
    {
        return -ENOMEM;
    }
    pthread_mutex_init(&dev->buffer_mutex, NULL);

    drmModeFreeConnector(conn);
    return 0;

err_fb0:
    xDRM_Modeset_Destroy_FB(fd, &dev->bufs[0]);
err_blob:
    drmModeDestroyPropertyBlob(fd, dev->mode_blob_id);
err_free:
    drmModeFreeConnector(conn);
    return ret;
}

static int xDRM_Modeset_Atomic_Modeset(int fd, struct modeset_dev *dev)
{
    int ret;
    uint32_t flags;

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req)
    {
        return -ENOMEM;
    }

    ret = xDRM_Modeset_Atomic_Prepare_Commit(fd, dev, req, dev->src_width, dev->src_height, dev->x_offset, dev->y_offset);
    if (ret < 0)
    {
        drmModeAtomicFree(req);
        return ret;
    }

    // use the least privilege flag
    flags = DRM_MODE_ATOMIC_NONBLOCK;
    ret = drmModeAtomicCommit(fd, req, flags, dev);
    if (ret < 0)
    {
        printf("Atomic modeset failed: %s\n", strerror(errno));
    }

    drmModeAtomicFree(req);
    return ret;
}

static int xDRM_Modeset_Atomic_Init(int fd)
{
    uint64_t cap;
    int ret;

    ret = drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap);
    if (ret || !cap)
    {
        fprintf(stderr, "Device does not support atomic modesetting\n");
        return -ENOTSUP;
    }

    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret)
    {
        fprintf(stderr, "Failed to set universal planes cap\n");
        return ret;
    }

    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret)
    {
        fprintf(stderr, "Failed to set atomic cap\n");
        return ret;
    }

    return 0;
}

void xDRM_Page_Flip_Handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
{
    struct modeset_dev *dev = (struct modeset_dev *)data;

    dev->pflip_pending = false;

#if __ENABLE_PATTERN__
    if (!dev->cleanup)
    {
        // get current buffer
        struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];

        // push data
        xDRM_Pattern((uint32_t *)buf->map, dev->src_width, dev->src_height, frame_count_test_pattern++);

        // commit
        int ret = xDRM_Modeset_Atomic_Page_Flip(fd, dev, dev->src_width, dev->src_height, dev->x_offset, dev->y_offset);
        if (ret >= 0)
        {
            dev->front_buf ^= 1;
            dev->pflip_pending = true;

            // @attention, control 60fps.
            usleep(16666);
        }
    }
#else
    if (!dev->cleanup)
    {
        struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];

        pthread_mutex_lock(&dev->buffer_mutex);
        memcpy(buf->map, dev->data_buffer, dev->src_width * dev->src_height * sizeof(uint32_t));
        pthread_mutex_unlock(&dev->buffer_mutex);

        // commit
        int ret = xDRM_Modeset_Atomic_Page_Flip(fd, dev, dev->src_width, dev->src_height, dev->x_offset, dev->y_offset);
        if (ret >= 0)
        {
            dev->front_buf ^= 1;
            dev->pflip_pending = true;
            usleep(16666);
        }
    }
#endif
}

static void xDRM_Modeset_Cleanup(int fd, struct modeset_dev *dev)
{
    drmEventContext ev = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler2 = xDRM_Page_Flip_Handler,
    };

    dev->cleanup = true;

    while (dev->pflip_pending)
    {
        drmHandleEvent(fd, &ev);
    }

    // Clean plane
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (req)
    {
        // Only clean plane, do nothing for CRTC
        xDRM_Set_DRM_Object_Property(req, &dev->plane, "FB_ID", 0);
        xDRM_Set_DRM_Object_Property(req, &dev->plane, "CRTC_ID", 0);
        drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
        drmModeAtomicFree(req);
    }

    // fb
    xDRM_Modeset_Destroy_FB(fd, &dev->bufs[0]);
    xDRM_Modeset_Destroy_FB(fd, &dev->bufs[1]);
    drmModeDestroyPropertyBlob(fd, dev->mode_blob_id);

    // properties
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

    // mutex and user buffer
    if (dev->data_buffer)
    {
        free(dev->data_buffer);
        dev->data_buffer = NULL;
    }
    pthread_mutex_destroy(&dev->buffer_mutex);
}

/* ====================================================================================================================== */
/* ================================================== Section 4 : APIs ================================================== */
/* ====================================================================================================================== */

// clang-format off
int xDRM_Init(struct modeset_dev **dev, uint32_t conn_id, uint32_t crtc_id, uint32_t plane_id, 
    uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
// clang-format on
{
    int fd, ret;

    // Step 1 : Open Device
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "Failed to open card: %s\n", strerror(errno));
        return -1;
    }

    // Step 2 : Init Atomic
    ret = xDRM_Modeset_Atomic_Init(fd);
    if (ret)
    {
        close(fd);
        return -1;
    }

    // Step 3 : Malloc Memory
    *dev = (struct modeset_dev *)malloc(sizeof(struct modeset_dev));
    if (!*dev)
    {
        close(fd);
        return -1;
    }
    memset(*dev, 0, sizeof(struct modeset_dev));

    // Step 4 : Setup Device
    ret = xDRM_Modeset_Setup_Dev(fd, *dev, conn_id, crtc_id, plane_id, source_width, source_height, x_offset, y_offset);
    if (ret)
    {
        free(*dev);
        close(fd);
        return -1;
    }

    // Step 5 : Stepup Atomic
    ret = xDRM_Modeset_Atomic_Modeset(fd, *dev);
    if (ret)
    {
        xDRM_Modeset_Cleanup(fd, *dev);
        free(*dev);
        close(fd);
        return -1;
    }

    // Step 6 : Initialize data buffer
    memset((*dev)->data_buffer, 0xFF000000, source_width * source_height * sizeof(uint32_t));

    return fd;
}

void xDRM_Exit(int fd, struct modeset_dev *dev)
{
    xDRM_Modeset_Cleanup(fd, dev);
    free(dev);
    close(fd);
}

void xDRM_Draw(int fd, struct modeset_dev *dev)
{
    struct pollfd fds[1];
    int ret;
    struct fps_stats fps_stats;

    // Init context
    drmEventContext ev = {};
    memset(&ev, 0, sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler2 = xDRM_Page_Flip_Handler;
    ev.vblank_handler = NULL;

    // Init FPS
    xDRM_Init_FPS_Stats(&fps_stats);

    // Set DRM file descriptor
    fds[0].fd = fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    // execute first atomic page flip
re_flip:
    ret = xDRM_Modeset_Atomic_Page_Flip(fd, dev, dev->src_width, dev->src_height, dev->x_offset, dev->y_offset);
    if (ret)
    {
        fprintf(stderr, "Initial page flip failed: %s\n", strerror(errno));
        goto re_flip;
    }

    dev->front_buf ^= 1;
    dev->pflip_pending = true;

    // main loop
    while (1)
    {
        fds[0].revents = 0;

        ret = poll(fds, 1, -1);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            printf("poll failed: %s\n", strerror(errno));
            break;
        }

        if (fds[0].revents & POLLIN)
        {
            ret = drmHandleEvent(fd, &ev);
            if (ret != 0)
            {
                printf("drmHandleEvent failed: %s\n", strerror(errno));
                break;
            }

            // Update FPS datas
            xDRM_Update_FPS_Stats(&fps_stats);
        }
    }
}

int xDRM_Push(struct modeset_dev *dev, uint32_t *data, size_t size)
{
    if (!dev || !data || size != dev->src_width * dev->src_height * sizeof(uint32_t))
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&dev->buffer_mutex);

    memcpy(dev->data_buffer, data, size);

    pthread_mutex_unlock(&dev->buffer_mutex);

    return 0;
}