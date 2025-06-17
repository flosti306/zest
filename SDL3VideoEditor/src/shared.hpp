#pragma once
#include <string>
#include <vector>
#include <memory>

struct EffectGraph; // Forward declaration

enum class ClipType {
    Video,
    Audio
};

enum class BlendMode {
    Normal,
    Additive,
    Multiply,
    Screen,
    Darken,
    Lighten,
    Difference,
    Subtract,
    Divide,
    Overlay
};

enum class InterpolationType {
    Linear,
    EaseInOut,
    Hold
};

template<typename T>
struct Keyframe {
    float time;               // Time in seconds
    T value;                  // The value at that time
    InterpolationType interp; // How to interpolate to next keyframe
};

template<typename T>
struct KeyframeTrack {
    std::vector<Keyframe<T>> keyframes;

    // Evaluate value at a given time
    T Evaluate(float time) const;
};

float Lerp(float a, float b, float t);


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
    float rotation = 0.0f;
    BlendMode blend_mode = BlendMode::Normal;

    KeyframeTrack<float> pos_x_track;
    KeyframeTrack<float> pos_y_track;
    KeyframeTrack<float> scale_track;
    KeyframeTrack<float> opacity_track;
    KeyframeTrack<float> rotation_track;

    bool selected = false;

    bool has_audio = true;
    bool is_audio_only = false;

    float volume = 1.0f;
    KeyframeTrack<float> volume_track;
    
    std::vector<float> waveform;     // normalized audio samples [-1.0, 1.0]

    Clip* linked_clip = nullptr; // for audio/video pairs

    std::shared_ptr<EffectGraph> effect_graph = nullptr; // optional

    bool has_effects = false;
};

#include "keyframetrack.inl"