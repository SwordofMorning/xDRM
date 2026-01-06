#include "xdrm/xdrm.h"
#include <iostream>
#include <thread>

struct modeset_dev *panel, *evf;
uint32_t image_data[1088 * 1920];

void panel_func()
{
    int fd = xDRM_Init(&panel, CONN_ID_DSI1, CRTC_ID_DSI1, PLANE_ID_DSI1, 1088, 1920, 0, 0, 1088, 1920);
    xDRM_Draw(fd, panel);
    xDRM_Exit(fd, panel);
}

void evf_func()
{
    int fd = xDRM_Init(&evf, CONN_ID_DSI2, CRTC_ID_DSI2, PLANE_ID_DSI2, 1920, 1088, 0, 0, 1920, 1088);
    xDRM_Draw(fd, evf);
    xDRM_Exit(fd, evf);
}

int main()
{
    std::thread th_panel = std::thread(panel_func);
    std::thread th_evf = std::thread(evf_func);

    // wait to finish initialize
    sleep(1);

    int count = 0;
    int mode_switch_counter = 0;
    // true: 1440x1080, false: 1280x1080
    bool large_sensor_mode = true;

    // Mode A: 1440x1080
    const int MODE_A_W = 1440;
    const int MODE_A_H = 1080;
    const int MODE_A_X = 240;
    const int MODE_A_Y = 0;

    // Mode B: 1280x1080
    const int MODE_B_W = 1280;
    const int MODE_B_H = 1080;
    const int MODE_B_X = 320;
    const int MODE_B_Y = 0;

    while (1)
    {
        xDRM_Pattern(image_data, 1088, 1920, count++);
        xDRM_Push(panel, image_data, sizeof(image_data));
        xDRM_Push(evf, image_data, sizeof(image_data));

        // Switch Mode
        if (++mode_switch_counter >= 200)
        {
            mode_switch_counter = 0;
            large_sensor_mode = !large_sensor_mode;

            if (large_sensor_mode)
            {
                printf("Switching to Mode A: 1440x1080 (Offset X: %d)\n", MODE_A_X);
                xDRM_Set_Layout(panel, MODE_A_X, MODE_A_Y, MODE_A_W, MODE_A_H);
                xDRM_Set_Layout(evf, MODE_A_X, MODE_A_Y, MODE_A_W, MODE_A_H);
            }
            else
            {
                printf("Switching to Mode B: 1280x1080 (Offset X: %d)\n", MODE_B_X);
                xDRM_Set_Layout(panel, MODE_B_X, MODE_B_Y, MODE_B_W, MODE_B_H);
                xDRM_Set_Layout(evf, MODE_B_X, MODE_B_Y, MODE_B_W, MODE_B_H);
            }
        }

        usleep(16 * 1000);
    }

    if (th_panel.joinable())
        th_panel.join();
    if (th_evf.joinable())
        th_evf.join();

    return 0;
}