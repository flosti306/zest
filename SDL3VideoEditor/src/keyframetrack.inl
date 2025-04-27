#pragma once

// Lerp helper
inline float Lerp(float a, float b, float t) {
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
            float t = (local_time - kf1.time) / (kf2.time - kf1.time);

            switch (kf1.interp) {
                case InterpolationType::Linear:
                    break;
                case InterpolationType::EaseInOut:
                    t = t*t*(3.0f - 2.0f*t); // Smoothstep
                    break;
                case InterpolationType::Hold:
                    return kf1.value;
            }
            return Lerp(kf1.value, kf2.value, t);
        }
    }

    return keyframes.back().value;
}
