#pragma once
#include <vector> // Ensure vector is included where KeyframeTrack is defined
#include <algorithm> // For std::clamp if needed later

// Lerp helper
inline float Lerp(float a, float b, float t) {
    // Consider adding clamping for t if interp types don't guarantee it
    // t = std::clamp(t, 0.0f, 1.0f);
    return a + (b - a) * t;
}

// Ensure KeyframeTrack is defined before use (moved from shared.hpp to here or keep in shared.hpp)
// template<typename T>
// struct Keyframe {
//     float time;
//     T value;
//     InterpolationType interp;
// };

// template<typename T>
// struct KeyframeTrack {
//     std::vector<Keyframe<T>> keyframes;
//     T Evaluate(float time) const;
// };
// End of potential definition block


template<typename T>
T KeyframeTrack<T>::Evaluate(float local_time) const {
    if (keyframes.empty()) {
        // If there are no keyframes, what should we return?
        // Option 1: Default value of T. Requires T to be default constructible.
        return T{};
        // Option 2: Throw an error or return an optional?
        // Option 3: Assume a static value exists elsewhere (like in Node).
        // For now, return default T. The Node::Evaluate methods handle the fallback.
    }

    // Clamp time or handle extrapolation? For now, clamp to the track's range.
    if (local_time <= keyframes.front().time) {
        return keyframes.front().value;
    }

    if (local_time >= keyframes.back().time) {
        return keyframes.back().value;
    }

    // Find the segment containing local_time
    // Assuming keyframes are sorted by time (which they should be!)
    for (size_t i = 0; i < keyframes.size() - 1; ++i) {
        const auto& kf1 = keyframes[i];
        const auto& kf2 = keyframes[i+1];

        if (local_time >= kf1.time && local_time <= kf2.time) {
            // Avoid division by zero if keyframes have same time
            if (kf2.time == kf1.time) {
                return kf1.value; // Or kf2.value, depending on desired behavior
            }

            float t = (local_time - kf1.time) / (kf2.time - kf1.time);

            // Apply interpolation based on the *first* keyframe's type
            switch (kf1.interp) {
                case InterpolationType::Linear:
                    // t remains unchanged
                    break;
                case InterpolationType::EaseInOut:
                    // Smoothstep: 3t^2 - 2t^3
                    t = t * t * (3.0f - 2.0f * t);
                    break;
                case InterpolationType::Hold:
                    // Hold the value of the first keyframe until the next one
                    return kf1.value;
                // Add other interpolation types here (e.g., Bezier)
            }

            // Perform the interpolation (Lerp assumes T can be interpolated linearly)
            // This might need specialization for types like Quaternions or non-numeric types.
            return Lerp(kf1.value, kf2.value, t);
        }
    }

    // Should technically be unreachable if time is clamped/handled correctly above,
    // but return last value as a safeguard.
    return keyframes.back().value;
}