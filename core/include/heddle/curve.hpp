#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace heddle {

// A time-series parameter: keyframes of (absolute_frame, normalized_0_1).
//
// Frames are absolute sample positions relative to the start of the render
// (or the start of the live session). Keyframes are stored sorted by frame
// (ascending). Linear interpolation between keyframes; clamped to the first
// keyframe's value before the timeline and to the last keyframe's value
// after it.
//
// JSON form (see docs/CONFIG.md):
//   "Cutoff": { "curve": [[0, 0.2], [45000, 0.8], [90000, 0.3]] }
//
// A plain number (e.g. "Cutoff": 0.5) is a static value — the backward-
// compatible form. Curves and static values can be mixed freely in the same
// params object.
struct Curve {
    std::vector<std::pair<int64_t, float>> keyframes;

    Curve() = default;
    Curve(std::initializer_list<std::pair<int64_t, float>> kfs)
        : keyframes(kfs) {}

    bool empty() const { return keyframes.empty(); }
    size_t size() const { return keyframes.size(); }

    // Value at an absolute sample position.
    //   - Empty curve  -> 0.0
    //   - Single kf    -> that kf's value (constant)
    //   - Before first -> first kf's value (clamp)
    //   - After last   -> last kf's value  (clamp)
    //   - Between      -> linear interpolation
    float eval(int64_t frame) const;
};

// Unified param value: a static float OR a time-series curve.
//
// isCurve() is true when the curve has at least one keyframe; in that case
// at(frame) evaluates the curve, otherwise it returns the static value.
//
// at(0) is the "load-time" value: the value the param should have when the
// chain is first loaded (frame 0 of the timeline).
struct ParamSpec {
    float value = 0.f;   // static value (used when !isCurve())
    Curve curve;         // time-series (used when isCurve())

    ParamSpec() = default;
    ParamSpec(float v) : value(v) {}  // static value, e.g. {{ "x", 0.5f }}

    bool isCurve() const { return !curve.empty(); }

    // Value at an absolute sample position.
    float at(int64_t frame) const {
        return isCurve() ? curve.eval(frame) : value;
    }

    // Value at the start of the timeline (frame 0).
    float atStart() const { return at(0); }
};

}  // namespace heddle
