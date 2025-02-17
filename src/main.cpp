#include "xdrm/xdrm.h"
#include <iostream>
#include <thread>

struct modeset_dev *panel, *evf;
uint32_t image_data[640 * 512];

void panel_func()
{
    int fd = xDRM_Init(&panel, CONN_ID_DSI1, CRTC_ID_DSI1, PLANE_ID_DSI1, 640, 512, 200, 200);
    xDRM_Pattern(image_data, 640, 512, 0);
    xDRM_Push(panel, image_data, sizeof(image_data));
    xDRM_Draw(fd, panel);
    xDRM_Exit(fd, panel);
}

void evf_func()
{
    int fd = xDRM_Init(&evf, CONN_ID_DSI2, CRTC_ID_DSI2, PLANE_ID_DSI2, 640, 512, 200, 200);
    xDRM_Pattern(image_data, 640, 512, 0);
    xDRM_Push(evf, image_data, sizeof(image_data));
    xDRM_Draw(fd, evf);
    xDRM_Exit(fd, evf);
}

int main()
{
    std::thread th_panel = std::thread(panel_func);
    std::thread th_evf = std::thread(evf_func);

    sleep(1);

    int count = 0;
    while (1)
    {
        xDRM_Pattern(image_data, 640, 512, count++);
        xDRM_Push(panel, image_data, sizeof(image_data));
        xDRM_Push(evf, image_data, sizeof(image_data));
        usleep(16666);
    }

    if (th_panel.joinable())
        th_panel.join();
    if (th_evf.joinable())
        th_evf.join();

    return 0;
}