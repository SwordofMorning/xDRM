#include "pattern.h"

// clang-format off
void xDRM_Pattern_Color(uint32_t *argb_data, int width, int height, int frame_count) 
{
    int offset = frame_count * 4;

    for(int y = 0; y < height; y++) 
    {
        for(int x = 0; x < width; x++) 
        {
            uint8_t r = (x + offset) & 0xFF;
            uint8_t g = (y + offset) & 0xFF;
            uint8_t b = (x + y + offset) & 0xFF;

            argb_data[y * width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

void xDRM_Pattern_Bar(uint32_t *argb_data, int width, int height, int frame_count)
{
    int bar_width = width / 8;
    int offset = frame_count * 8;

    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            int bar = ((x + offset) / bar_width) % 8;
            uint32_t color;

            switch(bar)
            {
                case 0: color = 0xFFFFFFFF; break;
                case 1: color = 0xFFFFFF00; break;
                case 2: color = 0xFF00FFFF; break;
                case 3: color = 0xFF00FF00; break;
                case 4: color = 0xFFFF00FF; break;
                case 5: color = 0xFFFF0000; break;
                case 6: color = 0xFF0000FF; break;
                case 7: color = 0xFF000000; break;
            }

            argb_data[y * width + x] = color;
        }
    }
}

void xDRM_Pattern_Checkerboard(uint32_t *argb_data, int width, int height, int frame_count)
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

void xDRM_Pattern(uint32_t *argb_data, int width, int height, int framecount)
{
    // change pattern per 60 frames
    switch((framecount / 60) % 3)
    {
        case 0:
            xDRM_Pattern_Color(argb_data, width, height, framecount);
            break;
        case 1:
            xDRM_Pattern_Bar(argb_data, width, height, framecount);
            break;
        case 2:
            xDRM_Pattern_Checkerboard(argb_data, width, height, framecount);
            break;
    }
}
// clang-format on