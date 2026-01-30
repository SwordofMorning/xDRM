#include "xdrm/xdrm.h"
#include <iostream>
#include <thread>

struct modeset_dev *panel, *evf, *hdmi;
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

void hdmi_func()
{
    // Note: Use local pointer to avoid crash
    struct modeset_dev *local_dev = NULL;
    const int RETRY_DELAY_MS = 1000;

    printf("[HDMI] Thread started, waiting for connection...\n");

    while (1)
    {
        // [State 1 Connection] Try Connection
        int connected = xDRM_Check_Connection(CONN_ID_HDMI);

        if (connected == 1)
        {
            printf("[HDMI] Detected connection, initializing...\n");

            // Step_Ptr 1: Init
            // Pass in the address of the local pointer &local_dev;
            // at this point, the global hdmi is still nullptr.
            int fd = xDRM_Init(&local_dev, CONN_ID_HDMI, CRTC_ID_HDMI, PLANE_ID_HDMI, 1920, 1088, 0, 0, 1920, 1088);

            if (fd >= 0 && local_dev != nullptr)
            {
                // Step_Ptr 2: Assign
                // Assign local_dev to global hdmi pointer.
                hdmi = local_dev;

                // [State 2 Loop] Draw Loop
                printf("[HDMI] Starting draw loop.\n");
                xDRM_Draw(fd, local_dev);

                // [State 3 Clean]: Cleanup
                printf("[HDMI] Exited draw loop, cleaning up.\n");

                // Step_Ptr 3：Clean
                // Cut off the global pointer
                // to prevent the main thread from continuing to access it.
                hdmi = nullptr;

                // Step_Ptr 3：Clean
                // Sleep 50ms to make sure main thread exit hdmi's resource (xDRM_Push)
                usleep(50 * 1000);

                // Step_Ptr 3：Clean
                // Clean local pointer.
                xDRM_Exit(fd, local_dev);
                local_dev = nullptr;
            }
            else
            {
                fprintf(stderr, "[HDMI] Initialization failed, retrying in %d ms.\n", RETRY_DELAY_MS);
            }
        }

        // Wait before next check
        usleep(RETRY_DELAY_MS * 1000);
    }
}

int main()
{
    std::thread th_panel = std::thread(panel_func);
    std::thread th_evf = std::thread(evf_func);
    std::thread th_hdmi = std::thread(hdmi_func);

    // wait to finish initialize
    sleep(1);

    int count = 0;
    int mode_switch_counter = 0;
    // true: 1440x1080, false: 1280x720
    bool large_sensor_mode = true;

    // Mode A: 1440x1080
    const int MODE_A_W = 1440;
    const int MODE_A_H = 1080;
    const int MODE_A_X = 240;
    const int MODE_A_Y = 0;

    // Mode B: 1280x720
    const int MODE_B_W = 1280;
    const int MODE_B_H = 720;
    const int MODE_B_X = 320;
    const int MODE_B_Y = 0;

    while (1)
    {
        xDRM_Pattern(image_data, 1088, 1920, count++);
        xDRM_Push(panel, image_data, sizeof(image_data));
        xDRM_Push(evf, image_data, sizeof(image_data));
        xDRM_Push(hdmi, image_data, sizeof(image_data));

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
                printf("Switching to Mode B: 1280x720 (Offset X: %d)\n", MODE_B_X);
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
    if (th_hdmi.joinable())
        th_hdmi.join();

    return 0;
}