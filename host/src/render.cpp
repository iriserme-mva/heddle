// heddle-render — offline audio→audio renderer.
//
//   heddle-render <input.wav> <chain.json> <output.wav> [--bits 16|24|32]
//
// Reads an input WAV at its own sample rate (no resampling, ever), runs it
// through the JUCE-hosted plugin chain described by the config (the config's
// per-slot `params:` blocks are pushed onto the live instances), and writes
// the result to the output WAV at the same rate/channel count.
//
// Params may be static (a plain number) or time-series curves (a
// `{"curve": [[frame, value], ...]}` object — see docs/CONFIG.md). Curves are
// evaluated at the start of every processing block against the absolute
// sample position and pushed to the plugin, so the automation follows the
// timeline as the render advances.
//
// Bit depth: 32 (default) = 32-bit float WAV, lossless for the float chain;
// 16 and 24 are plain integer conversion (no dither). A mono input is
// upmixed to stereo by the reader; the output channel count is the chain bus
// width (see JuceBackend::busWidth), i.e. max(input channels, 2, widest slot).
// The output sample rate must be one of the standard WAV rates (8k–384k);
// the reader's rate is passed through unchanged.
//
// Headless: no audio devices, no GUI — the JUCE message thread is used for
// plugin hosting only (same pattern as heddle_smoke).
//
// Stride design: JuceBackend::process reads its flat `in`/`out` pointers as
// planar with per-channel stride == `frames`, and JUCE AudioBuffer lays
// channels out contiguously with stride == its per-channel size. So a buffer
// handed to process() MUST have per-channel size == the frames argument.
// We therefore process FULL block-size blocks: the working buffers are
// (width, blockSize), the last partial block is zero-padded to blockSize
// before processing, and only the real n frames are written to the output.
// This keeps stride == buffer size invariant on every block (a final partial
// block processed at n < blockSize would mis-stride and read stale data).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>              // juce::initialiseJuce_GUI()
#include <juce_audio_formats/juce_audio_formats.h>  // WavAudioFormat, AudioFormatReader

#include "JuceBackend.hpp"
#include "heddle/chain.hpp"
#include "heddle/config.hpp"

// Version is injected at build time by CMake (configure_file -> heddle_version.hpp,
// on this target's include path via target_include_directories).
#include "heddle_version.hpp"
#ifndef HEDDLE_VERSION_STRING
#define HEDDLE_VERSION_STRING "unknown"
#endif

namespace {

int fail(const std::string& msg) {
    std::fprintf(stderr, "heddle-render: %s\n", msg.c_str());
    return 1;
}

void usage() {
    std::fprintf(stderr,
                 "usage: heddle-render [--version] <input.wav> <chain.json> <output.wav> [--bits 16|24|32]\n"
                 "       Renders input.wav through the JUCE-hosted chain from chain.json\n"
                 "       to output.wav (same sample rate, no resampling).\n"
                 "       --bits: output bit depth, 16, 24, or 32 (default 32 = float WAV).\n"
                 "       --version: print the version and exit.\n");
}

}  // namespace

int main(int argc, char** argv) {
    // Pre-scan for --version so it works before/without the positional args.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-V") == 0) {
            std::printf("heddle-render %s\n", HEDDLE_VERSION_STRING);
            return 0;
        }
    }

    juce::initialiseJuce_GUI();

    if (argc < 4) { usage(); return argc < 2 ? 1 : 0; }

    const std::string inPath  = argv[1];
    const std::string cfgPath = argv[2];
    const std::string outPath = argv[3];
    int bits = 32;
    for (int i = 4; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--bits") == 0) {
            const int b = std::atoi(argv[i + 1]);
            if (b != 16 && b != 24 && b != 32) { usage(); return 1; }
            bits = b;
        } else {
            std::fprintf(stderr, "heddle-render: unknown option '%s'\n", argv[i]);
            usage();
            return 1;
        }
    }

    // ---- read input -------------------------------------------------------
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(
        formats.createReaderFor(juce::File(inPath.c_str())));
    if (reader == nullptr)
        return fail("cannot open input WAV: " + inPath);

    const double sampleRate = reader->sampleRate;
    const int inChannels = (int)reader->numChannels;
    const int64_t total = reader->lengthInSamples;
    if (sampleRate <= 0.0 || inChannels < 1)
        return fail("input WAV has an invalid sample rate or channel count");
    if (total <= 0)
        std::fprintf(stderr, "heddle-render: warning: input is empty, writing empty output\n");

    // ---- load chain -------------------------------------------------------
    heddle::ChainConfig cfg;
    std::string err;
    if (!heddle::config::parseFile(cfgPath, cfg, &err))
        return fail("bad config: " + err);
    if (cfg.slots.empty())
        return fail("config has no chain slots: " + cfgPath);

    heddle::JuceBackend backend;
    heddle::Chain chain(backend);
    if (!chain.load(cfg, &err))
        return fail("chain load failed: " + err);

    // Only enabled slots have a live instance; a disabled slot is skipped
    // during rendering (JuceBackend::process would return -1 for it).
    std::vector<std::string> activeSlots;
    activeSlots.reserve(cfg.slots.size());
    for (const auto& slot : cfg.slots)
        if (slot.enabled)
            activeSlots.push_back(slot.id);

    // loadSlot prepared the instances at a default rate (44100/512); re-prepare
    // at the input file's real rate and the config's block size so processing
    // runs at the file's true sample rate (no resampling, ever).
    const int blockSize = cfg.blockSize > 0 ? cfg.blockSize : 512;
    backend.setProcessSpec(sampleRate, blockSize);

    const int width = std::max(std::max(inChannels, 2), backend.busWidth());

    // Curve params: a slot's param can be a time-series (see heddle/curve.hpp).
    // At each block's start we re-evaluate the slot's params at the absolute
    // sample position and push them to the plugin, so the automation follows
    // the timeline as the render advances. Slots with only static params are
    // untouched (their values were already pushed at load time).
    std::vector<const heddle::SlotConfig*> curvySlots;
    for (const auto& slot : cfg.slots) {
        if (slot.enabled)
            for (const auto& kv : slot.params)
                if (kv.second.isCurve()) {
                    curvySlots.push_back(&slot);
                    break;
                }
    }
    const bool hasCurves = !curvySlots.empty();

    // ---- output writer ----------------------------------------------------
    // AudioFormatManager has no writer helper (only createReaderFor), so build
    // the WAV writer directly. It takes ownership of the stream and deletes it
    // in ~AudioFormatWriter — but only if it was created successfully: on
    // failure the stream is NOT deleted and fileOut below still owns it.
    auto fileOut = juce::File(outPath.c_str()).createOutputStream();
    if (fileOut == nullptr)
        return fail("cannot open output for writing: " + outPath);
    // createWriterFor() is non-const (it mutates format internals), so the
    // WavAudioFormat must not be const.
    juce::WavAudioFormat wav;
    auto writer = std::unique_ptr<juce::AudioFormatWriter>(
        wav.createWriterFor(fileOut.get(), sampleRate, (unsigned int)width, bits, {}, 0));
    if (writer == nullptr)
        return fail("cannot create output WAV at " + std::to_string((int)sampleRate) +
                    " Hz / " + std::to_string(bits) +
                    "-bit (not a standard WAV sample rate / bit depth)");
    fileOut.release();  // the writer owns the stream now

    // ---- process + write --------------------------------------------------
    // All working buffers are (width, blockSize); per-channel size always
    // equals the frames argument passed to process() (see stride design).
    juce::AudioBuffer<float> inBuf(width, blockSize);
    juce::AudioBuffer<float> a(width, blockSize), b(width, blockSize);

    int64_t pos = 0;
    while (pos < total) {
        const int n = (int)std::min<int64_t>(blockSize, total - pos);

        // Pull one block at the file's rate. clear() first guarantees the
        // zero-padding (n..blockSize on the last block) and any channel the
        // reader leaves unfilled are zero, not uninit memory. The reader
        // upmixes a mono source to the buffer's 2 channels (dups L->R), so
        // for mono input BOTH channels are valid and must not be re-cleared.
        inBuf.clear();
        if (!reader->read(&inBuf, 0, n, pos, true, true))
            return fail("failed reading input at sample " + std::to_string(pos));
        pos += n;

        // Timeline: advance any curve params to this block's start.
        // Frame 0 of the timeline == sample 0 of the input file, so the
        // absolute position is simply pos - n. Static-only chains skip this
        // entirely (setParams was already done at load time).
        if (hasCurves) {
            const int64_t frame = pos - n;
            for (const auto* slot : curvySlots) {
                backend.setParams(slot->id,
                                  heddle::evalParams(slot->params, frame));
            }
        }

        // Ping-pong through the active slots; buffer 0 starts with the input.
        // We always hand process() the FULL blockSize: every working buffer is
        // (width, blockSize) so its per-channel stride is blockSize, and
        // JuceBackend::process requires stride == frames. The last block's
        // zero-padded tail runs through the plugins and is simply never
        // written. (Processing at n < blockSize would mis-stride: channel 1+
        // would be read at offset n instead of blockSize.)
        // Each target is cleared before the hop: a slot only writes its own
        // outCh channels, so channels beyond that (if outCh < width) would
        // otherwise be stale when the next slot reads them.
        juce::AudioBuffer<float>* cur = &inBuf;
        for (const auto& slotName : activeSlots) {
            juce::AudioBuffer<float>* next = (cur == &a) ? &b : &a;
            next->clear();
            const int processed =
                backend.process(slotName, cur->getWritePointer(0, 0),
                                next->getWritePointer(0, 0), width, blockSize);
            if (processed != blockSize)
                return fail("slot '" + slotName + "' failed after " +
                            std::to_string(processed) + " frames");
            cur = next;
        }

        // Write only the real n frames (the zero-padded tail is discarded).
        if (!writer->writeFromAudioSampleBuffer(*cur, 0, n))
            return fail("failed writing output at frame " + std::to_string(pos - n));
    }

    writer->flush();
    writer = nullptr;  // closes + deletes the owned stream

    std::printf("heddle-render: %s (%.0f Hz, %d ch, %lld frames) -> %s (%d-bit WAV, %d slot(s))\n",
                inPath.c_str(), sampleRate, inChannels, (long long)total,
                outPath.c_str(), bits, (int)activeSlots.size());
    return 0;
}
