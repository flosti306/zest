#pragma once
#include <cmath>

// --- Bezier Math Helpers ---

// Returns x(t) given control points x1, x2
// P0 is (0,0), P3 is (1,1)
inline float SampleBezierX(float t, float x1, float x2) {
    return 3.0f * (1.0f - t) * (1.0f - t) * t * x1 + 3.0f * (1.0f - t) * t * t * x2 + t * t * t;
}

// Returns y(t) given control points y1, y2
inline float SampleBezierY(float t, float y1, float y2) {
    return 3.0f * (1.0f - t) * (1.0f - t) * t * y1 + 3.0f * (1.0f - t) * t * t * y2 + t * t * t;
}

// Solves for 't' (parametric time) given 'x' (actual time progress)
// Uses Newton-Raphson iteration for speed and precision
inline float SolveBezierT(float x, float x1, float x2) {
    float t = x; // Initial guess
    // 5 iterations is usually enough for float precision in animation
    for (int i = 0; i < 5; i++) {
        float current_x = SampleBezierX(t, x1, x2) - x;
        if (std::abs(current_x) < 1e-5f) return t;
        
        // Derivative of X with respect to t
        float dxdt = 3.0f * (1.0f - t) * (1.0f - t) * x1 + 
                     6.0f * (1.0f - t) * t * (x2 - x1) + 
                     3.0f * t * t;
                     
        if (std::abs(dxdt) < 1e-5f) break;
        t -= current_x / dxdt;
    }
    return std::clamp(t, 0.0f, 1.0f);
}

// --- Main Implementation ---

// Lerp helper (Keep your existing one)
inline float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}
// Helper for glm types if you use them
inline glm::vec3 Lerp(glm::vec3 a, glm::vec3 b, float t) {
    return a + (b - a) * t;
}
inline glm::vec2 Lerp(glm::vec2 a, glm::vec2 b, float t) {
    return a + (b - a) * t;
}

template<typename T>
T KeyframeTrack<T>::Evaluate(float local_time) const {
    if (keyframes.empty())
        return T{};

    if (local_time <= keyframes.front().time)
        return keyframes.front().value;

    if (local_time >= keyframes.back().time)
        return keyframes.back().value;

    for (size_t i = 0; i < keyframes.size() - 1; ++i) {
        const auto& kf1 = keyframes[i];
        const auto& kf2 = keyframes[i+1];

        if (local_time >= kf1.time && local_time <= kf2.time) {
            // 1. Calculate Linear Progress (0.0 to 1.0)
            float duration = kf2.time - kf1.time;
            float t = (local_time - kf1.time) / duration;

            switch (kf1.interp) {
                case InterpolationType::Linear:
                    // t remains linear
                    break;

                case InterpolationType::EaseInOut:
                    // Standard Smoothstep
                    t = t * t * (3.0f - 2.0f * t);
                    break;

                case InterpolationType::Hold:
                    return kf1.value;

                case InterpolationType::Bezier: {
                    // Normalize handles
                    float duration = kf2.time - kf1.time;
                    if (duration <= 0.0001f) duration = 0.0001f; // Protect division

                    // Read from our safe POD struct
                    // x1 = handle_right.x normalized
                    float x1 = std::clamp(kf1.handle_right.x / duration, 0.0f, 1.0f);
                    
                    // x2 = 1.0 + handle_left.x normalized (handle_left.x is usually negative)
                    float x2 = std::clamp(1.0f + (kf2.handle_left.x / duration), 0.0f, 1.0f);
                    
                    // Sanity defaults if handles are zeroed
                    if (x1 == 0.0f && kf1.handle_right.y == 0.0f) x1 = 0.33f;
                    if (x2 == 1.0f && kf2.handle_left.y == 0.0f) x2 = 0.66f;

                    // Solve for T
                    float param_t = SolveBezierT(t, x1, x2);
                    
                    // Sample Y (Value influence)
                    float y1 = kf1.handle_right.y;       
                    float y2 = 1.0f + kf2.handle_left.y; 
                    
                    t = SampleBezierY(param_t, y1, y2);
                    break;
                }
            }

            return Lerp(kf1.value, kf2.value, t);
        }
    }

    return keyframes.back().value;
}