#include "xdrm/xdrm.h"
#include <iostream>
#include <thread>

void p2_func()
{
    struct modeset_dev *dev;
    int fd = xDRM_Init(&dev, CONN_ID_DSI2, CRTC_ID_DSI2, PLANE_ID_DSI2, 640, 512, 0, 0);
    modeset_draw(fd, dev);
    xDRM_Exit(fd, dev);
}

int main(int argc, char *argv[])
{
    std::thread p2 = std::thread(p2_func);

    struct modeset_dev *dev;
    int fd = xDRM_Init(&dev, CONN_ID_DSI1, CRTC_ID_DSI1, PLANE_ID_DSI1, 640, 512, 200, 200);

    modeset_draw(fd, dev);

    xDRM_Exit(fd, dev);
    return 0;
}