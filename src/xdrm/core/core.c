#include "core.h"

struct modeset_dev *modeset_list = NULL;

/* ======================================================================================================================= */
/* ================================================== Section 1 : Basic ================================================== */
/* ======================================================================================================================= */

static uint32_t get_property_id(int fd, drmModeObjectProperties *props, const char *name)
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

static int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj, const char *name, uint64_t value)
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

static void modeset_get_object_properties(int fd, struct drm_object *obj, uint32_t type)
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

static int modeset_create_fb(int fd, struct modeset_buf *buf)
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
    
    ret = drmModeAddFB2(fd, buf->width, buf->height,
                        DRM_FORMAT_ARGB8888, handles, pitches, offsets,
                        &buf->fb, DRM_MODE_FB_MODIFIERS);
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

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
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

static int check_plane_capabilities(int fd, struct modeset_dev *dev)
{
    drmModePlane *plane = drmModeGetPlane(fd, dev->plane.id);
    if (!plane)
    {
        fprintf(stderr, "Cannot get plane %u\n", dev->plane.id);
        return -EINVAL;
    }

#if __ENABLE_DEBUG_LOG__
    printf("Plane info: id=%u, possible_crtcs=0x%x, formats_count=%u\n",
           plane->plane_id, plane->possible_crtcs, plane->count_formats);
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
        fprintf(stderr, "Plane %u cannot be used with CRTC %u\n",
                plane->plane_id, dev->crtc.id);
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
        fprintf(stderr, "Plane %u does not support ARGB8888 format\n",
                plane->plane_id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    drmModeFreePlane(plane);
    return 0;
}

/* ====================================================================================================================== */
/* ================================================== Section 2 : APIs ================================================== */
/* ====================================================================================================================== */

int modeset_setup_dev(int fd, struct modeset_dev *dev, uint32_t conn_id, uint32_t crtc_id, uint32_t plane_id, uint32_t width, uint32_t height)
{
    int ret;
    
    dev->connector.id = conn_id;
    dev->crtc.id = crtc_id;
    dev->plane.id = plane_id;

    // Step 1 : checkout plane
    ret = check_plane_capabilities(fd, dev);
    if (ret < 0) {
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

    // Step 4 : set buffer display size
    dev->bufs[0].width = width;
    dev->bufs[0].height = height;
    dev->bufs[1].width = width;
    dev->bufs[1].height = height;

    // Step 5 : set property blob
    ret = drmModeCreatePropertyBlob(fd, &dev->mode, sizeof(dev->mode),
                                   &dev->mode_blob_id);
    if (ret) {
        fprintf(stderr, "cannot create mode blob: %m\n");
        goto err_free;
    }

    // Step 6 : get properties
    modeset_get_object_properties(fd, &dev->connector, DRM_MODE_OBJECT_CONNECTOR);
    modeset_get_object_properties(fd, &dev->crtc, DRM_MODE_OBJECT_CRTC);
    modeset_get_object_properties(fd, &dev->plane, DRM_MODE_OBJECT_PLANE);

    // Step 7 : create frame buffer
    ret = modeset_create_fb(fd, &dev->bufs[0]);
    if (ret)
        goto err_blob;

    ret = modeset_create_fb(fd, &dev->bufs[1]);
    if (ret)
        goto err_fb0;

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