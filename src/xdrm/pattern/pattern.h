#pragma once

#include <stdio.h>
#include <stdint.h>
#include "../conf/debug.h"

#ifdef __cplusplus
extern "C" {
#endif

void xDRM_Pattern_Color(uint32_t *argb_data, int width, int height, int frame_count);

void xDRM_Pattern_Bar(uint32_t *argb_data, int width, int height, int frame_count);

void xDRM_Pattern_Checkerboard(uint32_t *argb_data, int width, int height, int frame_count);

void xDRM_Pattern(uint32_t *argb_data, int width, int height, int framecount);

#ifdef __cplusplus
}
#endif