#include "fps.h"

void xDRM_Init_FPS_Stats(struct fps_stats *stats)
{
    gettimeofday(&stats->last_time, NULL);
    stats->frame_count = 0;
    stats->fps = 0.0f;
    stats->avg_fps = 0.0f;
    stats->total_frames = 0;
    stats->total_time = 0;
}

void xDRM_Update_FPS_Stats(struct fps_stats *stats)
{
    // clang-format off
    stats->frame_count++;
    stats->total_frames++;

    gettimeofday(&stats->current_time, NULL);

    // Calculate the time difference (milliseconds)
    long time_diff = (stats->current_time.tv_sec - stats->last_time.tv_sec) * 1000 +
                    (stats->current_time.tv_usec - stats->last_time.tv_usec) / 1000;

    // update FPS per second
    if (time_diff >= 1000)
    {
        stats->fps = (float)stats->frame_count * 1000 / time_diff;
        stats->total_time += time_diff;
        stats->avg_fps = (float)stats->total_frames * 1000 / stats->total_time;

#if __ENABLE_DEBUG_LOG__
        printf("FPS: %.2f (Current) %.2f (Average) - Frames: %ld Time: %.2fs\n",
               stats->fps,
               stats->avg_fps,
               stats->total_frames,
               stats->total_time / 1000.0f);
#endif

        // reset conter
        stats->frame_count = 0;
        stats->last_time = stats->current_time;
    }
    // clang-format on
}