#pragma once
#include <string>

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

#include "keyframetrack.inl"