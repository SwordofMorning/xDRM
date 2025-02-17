#pragma once

#include <stdio.h>
#include <sys/time.h>
#include "../conf/debug.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fps_stats
{
    struct timeval last_time;
    struct timeval current_time;
    int frame_count;
    float fps;
    float avg_fps;
    long total_frames;
    long total_time;
};

void xDRM_Init_FPS_Stats(struct fps_stats *stats);

void xDRM_Update_FPS_Stats(struct fps_stats *stats);

#ifdef __cplusplus
}
#endif