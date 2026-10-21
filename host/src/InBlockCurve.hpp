#pragma once
// heddle InBlockCurve -- sample-accurate in-block parameter curves over OSC
// waypoints (the VST3 IParamValueQueue model realized host-side).
//
// WHY HOST-SIDE
//   A VST3 DAW host expresses in-block automation as a list of {offset, value}
//   points and the PLUGIN interpolates between them. Heddle's plugins (and
//   the JUCE host wrapper around them) honour ONE value per process() call:
//   juce_VST3PluginFormat.cpp processAudio drains the atomic cached param
//   value at offset 0 at the start of each process() and clears it, so a
//   smooth in-block curve cannot be expressed by pushing several offsets.
//   The host therefore renders the curve itself: it splits a block into
//   sub-blocks and pushes one (possibly new) value per sub-block before
//   processing it -- a fine staircase that converges to the true curve as
//   the sub-step shrinks. This works for ANY plugin format (only the public
//   setParam + process API are used) and stateful plugins still see a
//   strictly sequential stream of process() calls (gate phase, envelopes,
//   ... all advance correctly). No tail re-render.
//
// WAYPOINT MODEL
//   An OSC set is a waypoint (block, value): "the value at the START of block
//   `block` is `value`." The message thread stamps each set at
//   block = currentAudioBlock + LOOKAHEAD (2) using the host's monotonic
//   audio-block counter, so stamps are non-decreasing in arrival order.
//   The curve is the piecewise-linear function through all known waypoints:
//     * exactly at a waypoint -> its value (holds for the whole block);
//     * between two waypoints -> the chord (linear interpolation);
//     * before the first waypoint -> hold (the caller carries the value the
//       plugin actually latches -- no extrapolation into the past, so a
//       late-arriving point can never leak backward into finished blocks).
//
//   BENDABILITY ("new time points should affect the curve which is still
//   schedulable for the coming-up block"). A set stamped during block b
//   targets b+2, so blocks b and b+1 keep their committed values while every
//   block >= b+2 bends: at b+2's start the new point is exact, and the blocks
//   between the previous waypoint and b+2 are the chord toward it. Continuity
//   is by construction: the host carries each block's end value forward as
//   the next block's start, so blocks never click at boundaries.
//
//   Same-block collapse: every set arriving while block b is in flight is
//   stamped at b+2, so a burst of N sets collapses to the LAST one of the
//   burst (newest wins per block). That is honest VST3-queue semantics and
//   is what the rate sweep (0.1/block .. 100/block) measures: below
//   ~1 set/block the waypoint density is the arrival rate, above it the
//   density saturates at 1 per block and only the sub-step staircase floor
//   remains.
//
// THREAD MODEL (lock-free SPSC)
//   One producer (message/OSC thread) pushes; one consumer (audio thread)
//   drains. Push is a single release-store on head after the slot is filled;
//   drain is an acquire-load of head then relaxed reads of the slots, ending
//   with a release-store on tail. 64-bit head/tail counters make the ring
//   wrap-safe forever (no ABA, no eviction):
//     * the window is unbounded in time -- sparse streams (1 waypoint per
//       10 blocks) never lose their nearest-left context;
//     * there is no modulo aliasing: two waypoints 16 blocks apart cannot
//       collide, unlike a fixed residue table.
//   Capacity kCap is generous (4096) and a full ring drops the push (the
//   audio thread drains once per audio block, so this can only happen if the
//   audio thread stops).
//
// The consumer keeps its own window (an audio-thread-owned vector) and calls
// eval() with it; this class is stateless beyond the two counters, so a
// per-(slot,param) instance costs a page and needs no locking at reset
// (reset() is only ever called while the audio thread is quiesced).
class heddle_InBlockCurve {
public:
    static constexpr size_t kCap = 512;    // power of two; must exceed the max
                                           // pushes between drains (the audio
                                           // thread drains once per block)
    struct Wp { uint32_t block; float value; };

    heddle_InBlockCurve() { reset(); }

    // Reset to a clean state. Message-thread only, with the audio thread
    // quiesced (apply/reset paths).
    void reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    // Message thread: a set arrived; the value takes effect at the START of
    // `block` (the caller adds the lookahead). A later waypoint for the same
    // block supersedes the earlier one at drain time (newest wins) -- that
    // is how a late-arriving point bends a not-yet-rendered block.
    // Returns false if the ring was full (drop; audio thread stalled).
    bool push(uint32_t block, float v01) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= kCap) return false;
        auto& s = slots_[h & (kCap - 1)];
        s.value = clamp01(v01);
        s.block = block;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Audio thread: drain up to `cap` pending waypoints into out[] in
    // arrival order (stamps non-decreasing). Returns how many were drained.
    size_t takeAll(Wp out[], size_t cap) {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        size_t n = 0;
        while (t < h && n < cap) {
            const auto& s = slots_[t & (kCap - 1)];
            out[n].block = s.block;
            out[n].value = s.value;
            ++n;
            ++t;
        }
        tail_.store(t, std::memory_order_release);
        return n;
    }

    // Number of undrained waypoints (diagnostics).
    size_t pending() const {
        const uint64_t h = head_.load(std::memory_order_acquire);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        return static_cast<size_t>(h - t);
    }

    // Piecewise-linear evaluation of a SORTED waypoint window (non-decreasing
    // block stamps, newest-wins already applied per block by the caller) at a
    // (possibly fractional) block position pos, in BLOCKS.
    //   * pos exactly at a waypoint        -> that waypoint's value;
    //   * pos between two waypoints        -> the chord (linear interp);
    //   * pos before the first waypoint    -> hold (the carried value);
    //   * pos at/after the last waypoint   -> the last waypoint's value (hold,
    //                                          never extrapolate past it).
    // `hold` is the value the plugin currently latches -- the caller carries
    // it forward across blocks so the returned value always continues the
    // audio thread's own state (no click at boundaries).
    static float evalCurve(const Wp wps[], size_t n, double pos, float hold) {
        if (n == 0) return clamp01(hold);
        size_t i0 = 0;
        for (size_t i = 0; i < n; ++i)
            if (static_cast<double>(wps[i].block) <= pos) i0 = i + 1;
        if (i0 == 0) return clamp01(hold);          // everything to the right
        const Wp& left = wps[i0 - 1];
        if (i0 == n) return clamp01(left.value);    // pos past the last: hold
        const Wp& right = wps[i0];
        const double span = static_cast<double>(right.block - left.block);
        const double t = (span > 0.0)
                             ? (pos - static_cast<double>(left.block)) / span
                             : 0.0;
        return clamp01(left.value + (right.value - left.value) *
                                       static_cast<float>(t));
    }

    // Sample-accurate chord value for sub-block sample p in [0, S): the block
    // being rendered starts at block `blockStart`, so sample p is at block
    // position blockStart + p/S. Because the chord is linear, this reduces to
    // evalCurve at that fractional block position. `hold` is the carried value
    // (returned for positions before the first waypoint).
    static float chordAt(const Wp wps[], size_t n, uint32_t blockStart,
                         uint32_t S, uint32_t p, float hold) {
        const double pos =
            static_cast<double>(blockStart) +
            (S > 0 ? static_cast<double>(p) / static_cast<double>(S) : 0.0);
        return evalCurve(wps, n, pos, hold);
    }

private:
    static float clamp01(float v) {
        if (v < 0.f) return 0.f;
        if (v > 1.f) return 1.f;
        return v;
    }

    struct alignas(64) Slot { float value; uint32_t block; };

    std::atomic<uint64_t> head_{0};   // next slot to write
    std::atomic<uint64_t> tail_{0};   // next slot to read
    std::array<Slot, kCap> slots_{};  // plain fields; ordered by the atomics
};
