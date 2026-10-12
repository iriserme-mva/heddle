#pragma once
#include <cstddef>
#include <string>

namespace weft {

// Lock-free, single-producer / single-consumer (SPSC) audio ring backed by
// named shared memory. One end (the producer) writes interleaved float32
// frames; the other (the consumer) reads them. No locks and no syscalls in the
// hot path — just two atomic frame cursors and a memcpy. This is the audio
// path the Ableton bridge VST3 uses to hand samples to the running Weft
// process (and back), so the added latency is about one block.
//
//   interleaved layout, 2 channels: [L0, R0, L1, R1, ...]
//
// create() creates (and owns) a fresh named ring; open() attaches to one
// created elsewhere. Both ends map the same bytes. A ring with a given name
// must be created before anyone opens it; create() fails if a ring with that
// name already exists (use a unique name per session, or remove() first).
class AudioRing {
public:
    AudioRing();
    ~AudioRing();
    AudioRing(const AudioRing&) = delete;
    AudioRing& operator=(const AudioRing&) = delete;
    AudioRing(AudioRing&& o) noexcept;
    AudioRing& operator=(AudioRing&& o) noexcept;

    // Create a fresh named ring. `capacityFrames` is rounded up to the next
    // power of two (minimum 2). Fails (and sets *err) if a ring with this
    // name already exists.
    static AudioRing create(const std::string& name, size_t capacityFrames,
                            int numChannels, int sampleRate,
                            std::string* err = nullptr);

    // Attach to a ring created elsewhere. Waits up to `timeoutMs` for the
    // creator to publish its header. Fails (and sets *err) if it can't attach
    // or the ring isn't ready in time.
    static AudioRing open(const std::string& name, std::string* err = nullptr,
                          int timeoutMs = 2000);

    // True if a ring with this name currently exists.
    static bool exists(const std::string& name);

    // Best-effort removal of a named ring (POSIX: shm_unlink; Windows: a live
    // named mapping cannot be deleted, so this is a no-op there).
    static void remove(const std::string& name);

    bool valid() const;
    const std::string& name() const;
    size_t capacityFrames() const;   // actual (power-of-two) frame capacity
    int numChannels() const;
    int sampleRate() const;
    size_t capacitySamples() const;  // capacityFrames * numChannels

    // Producer: copy up to `frames` interleaved frames in. Returns the number
    // of frames actually written (0 if full, <frames if only partial fit).
    size_t write(const float* src, size_t frames);

    // Consumer: copy up to `frames` interleaved frames out. Returns the number
    // of frames actually read (0 if empty, <frames if only partial available).
    size_t read(float* dst, size_t frames);

    // Frames that fit right now / frames available to read right now.
    size_t spaceAvailable() const;
    size_t dataAvailable() const;

    // Release this mapping. After close(), valid() is false.
    void close();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace weft
