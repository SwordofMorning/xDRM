#pragma once

#include <stdio.h>
#include <sys/time.h>
#include "../conf/debug.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fps_stats
{
    struct timeval last_time;       // last frame time
    struct timeval current_time;    // current
    int frame_count;                // frame counter
    float fps;                      // current fps
    float avg_fps;                  // average fps, start from begin
    long total_frames;              // total frames
    long total_time;                // total times, millisecond
};

void xDRM_Init_FPS_Stats(struct fps_stats *stats);

void xDRM_Update_FPS_Stats(struct fps_stats *stats);

#ifdef __cplusplus
}
#endif