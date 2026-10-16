#include "heddle/curve.hpp"

#include <algorithm>
#include <cmath>

namespace heddle {

namespace {

// Clamp to [0,1] like Param::clamp01 (kept local so curve.hpp stays
// dependency-free — it must not drag param.hpp into every includer).
float clamp01f(float v) {
    if (v < 0.f) return 0.f;
    if (v > 1.f) return 1.f;
    return v;
}

}  // namespace

float Curve::eval(int64_t frame) const {
    const size_t n = keyframes.size();
    if (n == 0) return 0.f;
    if (n == 1) return clamp01f(keyframes[0].second);
    if (frame <= keyframes.front().first) return clamp01f(keyframes.front().second);
    if (frame >= keyframes.back().first) return clamp01f(keyframes.back().second);

    // Invariant (enforced by the config parser): keyframes sorted by frame.
    // Linear interpolation between the two bracketing keyframes.
    // Upper bound: first kf with frame > query. O(log n) over the sorted list.
    auto it = std::upper_bound(
        keyframes.begin(), keyframes.end(), frame,
        [](int64_t value, const std::pair<int64_t, float>& kf) {
            return value < kf.first;
        });
    const auto& hi = *it;
    const auto& lo = *(it - 1);

    if (hi.first == lo.first) return clamp01f(hi.second);  // duplicate frames
    const float t =
        float(frame - lo.first) / float(hi.first - lo.first);
    return clamp01f(lo.second + t * (hi.second - lo.second));
}

}  // namespace heddle
