#pragma once
#include <string>
#include <vector>
#include <map> // Added for media library potentially
#include <memory> // Added for shared_ptr

// Forward declaration
struct Node;

enum class ClipType {
    Video,
    Audio,
    Image // Added Image type
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
    float time;               // Time in seconds (relative to node start)
    T value;                  // The value at that time
    InterpolationType interp; // How to interpolate to next keyframe
};

template<typename T>
struct KeyframeTrack {
    std::vector<Keyframe<T>> keyframes;

    // Evaluate value at a given time (relative to node start)
    T Evaluate(float time) const;
};

// Forward Lerp declaration
float Lerp(float a, float b, float t);

// Represents the source media information
struct Clip {
    std::string name; // Base name derived from path
    std::string path; // Unique identifier (usually)
    ClipType type = ClipType::Video;

    float source_duration = 0.0f; // Duration of the original media file
    bool has_audio = false;
    bool is_audio_only = false;

    // Metadata (could add width, height, fps etc. later)
    int source_width = 0;
    int source_height = 0;

    // Optional: Preloaded audio data (could be moved elsewhere)
    std::vector<float> waveform;     // normalized audio samples [-1.0, 1.0] for preview
    // Potentially link source audio/video clips if they always come in pairs
    // std::string linked_clip_path; // Use path to link? Or manage links via Nodes?
};

// --- Include Implementations ---
#include "keyframetrack.inl"