#include "xdrm/xdrm.h"
#include <iostream>
#include <thread>

struct modeset_dev *dev;
uint32_t image_data[640 * 512];

static void generate_checkerboard(uint32_t* argb_data, int width, int height, int frame_count)
{
    int square_size = 64;
    int offset = frame_count * 4;

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

void pattern_func()
{
    int fd = xDRM_Init(&dev, CONN_ID_DSI1, CRTC_ID_DSI1, PLANE_ID_DSI1, 640, 512, 200, 200);
    generate_checkerboard(image_data, 640, 512, 0);
    xDRM_Push(dev, image_data, sizeof(image_data));
    xDRM_Draw(fd, dev);
    xDRM_Exit(fd, dev);
}

int main(int argc, char *argv[])
{
    std::thread p = std::thread(pattern_func);

    sleep(1);

    int count = 0;
    while (1)
    {
        generate_checkerboard(image_data, 640, 512, count++);
        xDRM_Push(dev, image_data, sizeof(image_data));
        usleep(16666);
    }

    return 0;
}