#pragma once
#include <string>

struct Clip {
    std::string name;
    
    int start_time = 0;      // timeline position in seconds
    int duration = 0;        // duration on timeline
    int layer = 0;           // compositing layer (higher = in front)
    
    std::string path;

    float media_start = 0.0f; // where in the source video to start

    // Transform attributes
    float pos_x = 0.0f;      // normalized [-1, 1] or pixel values (we can decide)
    float pos_y = 0.0f;
    float scale = 1.0f;      // uniform scale for now
    float opacity = 1.0f;    // 0.0 to 1.0

    bool selected = false;
};
