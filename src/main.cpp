#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>

#include "xdrm/xdrm.h"

// Note: 注意潜在的16字节对齐
#define TEST_WIDTH  1080
#define TEST_HEIGHT 1920
#define TEST_SIZE   (TEST_WIDTH * TEST_HEIGHT * sizeof(uint32_t))

static int g_drm_fd = -1;
static struct modeset_dev *g_panel_dev = NULL;

void *display_thread_func(void *arg)
{
    int ret;

    printf("[Thread] Initializing DRM...\n");

    // 初始化 DRM
    int fd = xDRM_Init(&g_panel_dev, CONN_ID_DSI1, CRTC_ID_DSI1, PLANE_ID_DSI1, 
                       TEST_WIDTH, TEST_HEIGHT, 0, 0);
    
    if (fd < 0)
    {
        fprintf(stderr, "[Thread] xDRM_Init failed\n");
        return NULL;
    }

    g_drm_fd = fd;
    printf("[Thread] DRM Initialized. FD=%d. Starting Draw Loop...\n", fd);

    xDRM_Draw(fd, g_panel_dev);

    printf("[Thread] Draw loop exited.\n");
    xDRM_Exit(fd, g_panel_dev);
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t th;
    uint32_t *capture_buffer = NULL;
    uint32_t *pattern_buffer = NULL;
    int ret;

    // 1. 启动显示线程
    if (pthread_create(&th, NULL, display_thread_func, NULL) != 0)
    {
        perror("Failed to create display thread");
        return -1;
    }

    // 2. 等待 DRM 初始化完成
    while (g_drm_fd < 0)
    {
        usleep(10000); 
    }
    sleep(1); // 等待 Pipeline 稳定

    // 3. 准备内存
    capture_buffer = (uint32_t *)malloc(TEST_SIZE);
    pattern_buffer = (uint32_t *)malloc(TEST_SIZE);

    if (!capture_buffer || !pattern_buffer)
    {
        fprintf(stderr, "Memory allocation failed\n");
        goto cleanup;
    }

    // 4. 推送测试图案
    printf("[Main] Pushing test pattern (frame 100) to screen...\n");
    // 注意：这里生成图案也必须用 1080 宽度
    xDRM_Pattern(pattern_buffer, TEST_WIDTH, TEST_HEIGHT, 100);
    xDRM_Push(g_panel_dev, pattern_buffer, TEST_SIZE);

    // 等待图像上屏
    usleep(50000);

    // 5. 执行抓图
    printf("[Main] Pulling image from CRTC %u (Size: %dx%d)...\n", CRTC_ID_DSI1, TEST_WIDTH, TEST_HEIGHT);
    ret = xDRM_Pull_CRTC(g_drm_fd, CRTC_ID_DSI1, capture_buffer, TEST_SIZE);

    if (ret == 0)
    {
        // 6. 保存到文件
        FILE *fp = fopen("test.bgra", "wb");
        if (fp)
        {
            size_t written = fwrite(capture_buffer, 1, TEST_SIZE, fp);
            fclose(fp);
            printf("[Main] Success! Saved %zu bytes to 'test.bgra'\n", written);
            printf("[Main] View command: ffplay -f rawvideo -pixel_format bgra -video_size %dx%d test.bgra\n", 
                   TEST_WIDTH, TEST_HEIGHT);
        }
        else
        {
            perror("[Main] Failed to open output file");
        }
    }
    else
    {
        fprintf(stderr, "[Main] xDRM_Pull_CRTC failed with code %d\n", ret);
    }

cleanup:
    if (capture_buffer) free(capture_buffer);
    if (pattern_buffer) free(pattern_buffer);

    return 0;
}