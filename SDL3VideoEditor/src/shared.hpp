#pragma once
#include <string>

enum class ClipType {
    Video,
    Audio
};

struct Clip {
    std::string name;
    
    float start_time = 0.0f;      // timeline position in seconds
    float duration = 0.0f;        // duration on timeline
    int layer = 0;           // compositing layer (higher = in front)
    
    std::string path;

    float media_start = 0.0f; // where in the source video to start

    ClipType type = ClipType::Video;

    // Transform attributes
    float pos_x = 0.0f;      // normalized [-1, 1] or pixel values (we can decide)
    float pos_y = 0.0f;
    float scale = 1.0f;      // uniform scale for now
    float opacity = 1.0f;    // 0.0 to 1.0

    bool selected = false;

    bool has_audio = true;
    bool is_audio_only = false;
    std::vector<float> waveform;     // normalized audio samples [-1.0, 1.0]

    Clip* linked_clip = nullptr; // for audio/video pairs

};
