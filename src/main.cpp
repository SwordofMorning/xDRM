#include "xdrm/xdrm.h"
#include <iostream>

int main(int argc, char *argv[])
{
    struct modeset_dev *dev;
    int fd = Modeset_Init(&dev, CONN_ID_DSI1, CRTC_ID_DSI1, PLANE_ID_DSI1, 640, 512, 0, 0);

    modeset_draw(fd, dev);

    Modeset_Exit(fd, dev);
    return 0;
}