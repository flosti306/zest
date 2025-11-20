#pragma once
#include <cmath>
#include <algorithm>
#include <type_traits>

// --- Bezier Math Helpers ---

// Solves for 't' (parametric curve position 0..1) given 'x' (time ratio 0..1)
// Uses Newton-Raphson to invert the X(t) bezier function
inline float SolveBezierT(float x, float x1, float x2) {
    float t = x; // Linear guess
    for (int i = 0; i < 8; i++) { 
        // B(t) for X axis (P0=0, P1=x1, P2=x2, P3=1)
        // formula: (1-t)^3*0 + 3(1-t)^2*t*x1 + 3(1-t)*t^2*x2 + t^3*1
        float invT = 1.0f - t;
        float b_x = 3.0f * invT * invT * t * x1 + 
                    3.0f * invT * t * t * x2 + 
                    t * t * t;
        
        float err = b_x - x;
        if (std::abs(err) < 1e-5f) return t;
        
        // Derivative of B(t)
        float dxdt = 3.0f * invT * invT * x1 + 
                     6.0f * invT * t * (x2 - x1) + 
                     3.0f * t * t;
                     
        if (std::abs(dxdt) < 1e-5f) break;
        t -= err / dxdt;
    }
    return std::clamp(t, 0.0f, 1.0f);
}

// Explicit Cubic Bezier for Values (Allows Overshoot)
template<typename T>
inline T CubicBezierValue(float t, T p0, T p1, T p2, T p3) {
    float u = 1.0f - t;
    float tt = t * t;
    float uu = u * u;
    float uuu = uu * u;
    float ttt = tt * t;

    // Standard formula: P0(1-t)^3 + 3P1(1-t)^2t + 3P2(1-t)t^2 + P3t^3
    return (p0 * uuu) + 
           (p1 * (3.0f * uu * t)) + 
           (p2 * (3.0f * u * tt)) + 
           (p3 * ttt);
}

template<typename T>
inline T Lerp(T a, T b, float t) {
    return a + (b - a) * t;
}

// --- Main Evaluation ---

template<typename T>
T KeyframeTrack<T>::Evaluate(float local_time) const {
    if (keyframes.empty()) return T{};

    // Boundary Checks
    if (local_time <= keyframes.front().time) return keyframes.front().value;
    if (local_time >= keyframes.back().time) return keyframes.back().value;

    // Find Segment
    for (size_t i = 0; i < keyframes.size() - 1; ++i) {
        const auto& kf1 = keyframes[i];
        const auto& kf2 = keyframes[i+1];

        if (local_time >= kf1.time && local_time <= kf2.time) {
            float duration = kf2.time - kf1.time;
            if (duration <= 1e-5f) duration = 1e-5f;
            
            // 'linear_t' is the 0.0-1.0 progress through the keyframe duration
            float linear_t = (local_time - kf1.time) / duration;

            switch (kf1.interp) {
                case InterpolationType::Hold:
                    return kf1.value;

                case InterpolationType::Linear:
                    return Lerp(kf1.value, kf2.value, linear_t);

                case InterpolationType::EaseInOut:
                    // Simple Smoothstep
                    linear_t = linear_t * linear_t * (3.0f - 2.0f * linear_t);
                    return Lerp(kf1.value, kf2.value, linear_t);

                case InterpolationType::Bezier: {
                    // 1. Solve X-Axis (Time) to find the curve position 'u'
                    // Normalize handles to 0..1
                    float x1 = std::clamp(kf1.handle_right.x / duration, 0.0f, 1.0f);
                    float x2 = std::clamp(1.0f + (kf2.handle_left.x / duration), 0.0f, 1.0f);
                    
                    float u = SolveBezierT(linear_t, x1, x2);

                    // 2. Solve Y-Axis (Value)
                    // We calculate the absolute control points for the Value curve.
                    // We force this path for floats to ensure overshoots are respected.
                    
                    // Using 'if constexpr' to allow this template to compile for Vectors too, 
                    // but prioritizing the float path for scalars.
                    if constexpr (std::is_arithmetic_v<T>) {
                        auto p0 = kf1.value;
                        // Add handle Y offset to the base value
                        auto p1 = kf1.value + static_cast<T>(kf1.handle_right.y);
                        auto p2 = kf2.value + static_cast<T>(kf2.handle_left.y);
                        auto p3 = kf2.value;

                        return CubicBezierValue(u, p0, p1, p2, p3);
                    } 
                    else {
                        // Fallback for complex types (Vec3/Color) where scalar addition might differ
                        // (This still eases the timing, but won't overshoot value)
                        return Lerp(kf1.value, kf2.value, u); 
                    }
                }
            }
        }
    }

    return keyframes.back().value;
}