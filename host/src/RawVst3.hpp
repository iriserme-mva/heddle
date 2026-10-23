#pragma once

// Raw VST3 parameter-curve machinery - used by the host's DEFAULT
// (curve_resolution == 0) in-block path.
//
// Why raw: the JUCE host wrapper collapses every param change to ONE
// (param, value, offset 0) point per process() call (see
// juce_VST3PluginFormat.cpp, processAudio: inputParameterChanges->set(id,
// value, 0)). A VST3 host is, however, allowed to build a full multi-point
// IParameterChanges per block, and the VST3 spec says the plugin may
// evaluate the linear curve between queue points at any sample:
//   "Every point of the value queue defines a linear section of the curve as
//    a straight line from the previous point of a block to the new one. So
//    the plug-in can calculate the value of the curve for any sample."
// JUCE's host API never exposes multi-point queuing, so the whole queue has
// to be delivered by driving Vst::IAudioProcessor::process() directly with a
// real IParameterChanges. This file provides:
//   * RawParamQueue   - a two-point IParamValueQueue (start @ offset 0,
//                       end @ offset S-1) for one param.
//   * RawParamChanges - an IParameterChanges holding one queue per active
//                       param for the current block.
//   * rawProcess()    - one IAudioProcessor::process() call carrying the
//                       queue, mirroring the ProcessData JUCE builds in
//                       VST3PluginInstance::processAudio.
//
// The host fills RawParamChanges once per block (evalCurve at the block's
// first and last sample for each active param) and makes a single
// rawProcess() call. Compliant plugins interpolate per-sample -> smooth;
// latching plugins hold one value per block (the end value). Oversampling
// (curve_resolution > 0) is a separate, opt-in path (substep loop over JUCE
// processBlock) and does not use this file.
//
// COM plumbing follows the proven pattern JUCE's own host-side VST3 COM
// classes use (juce_VST3PluginFormat.cpp: JUCE_DECLARE_VST3_COM_* macros):
// a private refCount member + a memcmp-based queryInterface, rather than the
// SDK's IMPLEMENT_* macros, which assume the class also declared the
// __funknownRefCount member (the raw FUnknown base does not).

#include <cstdint>
#include <cstring>
#include <vector>

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace heddle
{
namespace rawv
{
// The SDK has no global `Vst` alias (only `Steinberg::Vst`), and this code is
// written with a bare `Vst::` prefix throughout. Bind it locally.
namespace Vst = Steinberg::Vst;

// These SDK types are used unqualified below. Pull them in fully qualified:
// FUnknown / TUID / int32 / uint32 / tresult / the kResult* + kNoInterface
// constants live in `Steinberg` (base), everything else in `Steinberg::Vst`.
// NOTE: the result type is `tresult` (all-lowercase, ftypes.h) -- NOT
// `tresult`.
using Steinberg::FUnknown;
using Steinberg::TUID;
using Steinberg::FUID;
using Steinberg::int32;
using Steinberg::uint32;
using Steinberg::tresult;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::kResultFalse;
using Steinberg::kNoInterface;
using Vst::ParamID;
using Vst::ParamValue;
using Vst::IParamValueQueue;
using Vst::IParameterChanges;
using Vst::IAudioProcessor;
using Vst::AudioBusBuffers;
using Vst::ProcessData;
using Vst::ProcessModes;
using Vst::kSample32;

// Shared COM (FUnknown) base: refCounting + a memcmp queryInterface that
// answers FUnknown and the object's own interface. The plugin never
// re-queries these for a third type (it uses the concrete pointers it is
// handed), so answering only the two ids is complete. ComBase itself is NOT
// an FUnknown; the caller passes the derived object's FUnknown subobject
// (the VST interface base) as `self` to be handed back on a match.
class ComBase
{
public:
    virtual ~ComBase () = default;

    tresult queryInterface (FUnknown* self, const TUID iid, void** object)
    {
        if (object == nullptr)
            return kResultFalse;

        // Compare via the SDK's own iidEqual (const void* + const void*), which
        // handles both the TUID (char[16]) argument and the FUID iid objects.
        if (Steinberg::FUnknownPrivate::iidEqual (iid, FUnknown::iid)
            || Steinberg::FUnknownPrivate::iidEqual (iid, ownIid ()))
        {
            addRef ();
            *object = self;
            return kResultOk;
        }

        *object = nullptr;
        return kNoInterface;
    }

    uint32 addRef ()
    {
        return (uint32) ++refCount_;
    }

    uint32 release ()
    {
        const uint32 r = (uint32) --refCount_;
        if (r == 0)
            delete this;
        return r;
    }

protected:
    ComBase () = default;

    virtual const FUID& ownIid () const = 0;
    int refCount_ = 1;
};

// --------------------------------------------------------------------------
// Two-point IParamValueQueue for ONE param over ONE block.
//
//   Point 0 -> { offset 0,   value v0 }   (block-start value)
//   Point 1 -> { offset S-1, value v1 }   (block-end value)
//
// A compliant plugin linearly interpolates between the two across the block,
// reaching v1 exactly at the last sample. If v0 == v1 the param is constant
// over the block (harmless; the host includes it so the queue is uniform).
// --------------------------------------------------------------------------
class RawParamQueue final : public Vst::IParamValueQueue, private ComBase
{
public:
    RawParamQueue () = default;

    // Two-point convenience form (the common case: start + end of block).
    RawParamQueue (ParamID id, ParamValue a, ParamValue b, int32 s)
        : paramId_ (id), samples_ (s < 1 ? 1 : s)
    {
        offsets_.push_back (0);
        values_.push_back (a);
        offsets_.push_back (samples_ - 1);
        values_.push_back (b);
    }

    // N-point form: the host builds a full (offset, value) list, sorted
    // non-decreasing in offset, first offset 0, last offset samples-1.
    RawParamQueue (ParamID id, int32 s,
                   const std::vector<int32>& offsets,
                   const std::vector<ParamValue>& values)
        : paramId_ (id), samples_ (s < 1 ? 1 : s)
    {
        const size_t n = std::min (offsets.size (), values.size ());
        for (size_t i = 0; i < n; ++i)
        {
            int32 off = offsets [i];
            if (off < 0) off = 0;
            if (off > samples_ - 1) off = samples_ - 1;
            offsets_.push_back (off);
            values_.push_back (values [i]);
        }
    }

    ~RawParamQueue () override = default;

    // FUnknown
    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        return ComBase::queryInterface (
            static_cast<FUnknown*> (static_cast<Vst::IParamValueQueue*> (this)),
            iid, object);
    }
    uint32 PLUGIN_API addRef () override
    {
        return ComBase::addRef ();
    }
    uint32 PLUGIN_API release () override
    {
        return ComBase::release ();
    }

    // IParamValueQueue
    ParamID PLUGIN_API getParameterId () override
    {
        return paramId_;
    }

    int32 PLUGIN_API getPointCount () override
    {
        return static_cast<int32> (offsets_.size ());
    }

    tresult PLUGIN_API getPoint (int32 index, int32& sampleOffset /*out*/,
                                  ParamValue& value /*out*/) override
    {
        if (index < 0 || index >= static_cast<int32> (offsets_.size ()))
            return kResultFalse;
        sampleOffset = offsets_ [index];
        value        = values_ [index];
        return kResultTrue;
    }

    tresult PLUGIN_API addPoint (int32, ParamValue, int32& index /*out*/) override
    {
        // Read-only for the plugin: the host sizes the queue up front.
        index = -1;
        return kResultFalse;
    }

private:
    const FUID& ownIid () const override
    {
        return Vst::IParamValueQueue::iid;
    }

    ParamID            paramId_ = 0;
    int32              samples_ = 1;
    std::vector<int32> offsets_;   // sample offsets within the block, sorted
    std::vector<ParamValue> values_;  // matching values; offsets_[0]=0,
                                      // offsets_[last]=samples_-1
};

// --------------------------------------------------------------------------
// IParameterChanges for ONE block: one queue per active param.
// --------------------------------------------------------------------------
class RawParamChanges final : public Vst::IParameterChanges, private ComBase
{
public:
    RawParamChanges () = default;

    // The host appends one queue per active param for this block.
    void append (ParamID id, ParamValue v0, ParamValue v1, int32 samples)
    {
        queues_.push_back (new RawParamQueue (id, v0, v1, samples));
    }

    // N-point form: one queue per active param built from a sorted
    // (offset, value) list. Used by the whole-queue path when a block carries
    // sub-block waypoints (in-block kinks).
    void append (ParamID id, int32 samples,
                 const std::vector<int32>& offsets,
                 const std::vector<ParamValue>& values)
    {
        queues_.push_back (new RawParamQueue (id, samples, offsets, values));
    }

    void clear ()
    {
        for (auto* q : queues_)
            q->release ();
        queues_.clear ();
    }

    int count () const
    {
        return static_cast<int> (queues_.size ());
    }

    // FUnknown (the changes object answers IParameterChanges, not a queue)
    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        return ComBase::queryInterface (
            static_cast<FUnknown*> (static_cast<Vst::IParameterChanges*> (this)),
            iid, object);
    }
    uint32 PLUGIN_API addRef () override
    {
        return ComBase::addRef ();
    }
    uint32 PLUGIN_API release () override
    {
        return ComBase::release ();
    }

    // IParameterChanges
    int32 PLUGIN_API getParameterCount () override
    {
        return static_cast<int32> (queues_.size ());
    }

    IParamValueQueue* PLUGIN_API getParameterData (int32 index) override
    {
        return (index >= 0 && index < static_cast<int32> (queues_.size ()))
            ? queues_[index]
            : nullptr;
    }

    IParamValueQueue* PLUGIN_API addParameterData (const ParamID&,
                                                   int32& index /*out*/) override
    {
        // Read-only for the plugin; the host builds the list up front.
        index = -1;
        return nullptr;
    }

    ~RawParamChanges () override
    {
        clear ();
    }

private:
    const FUID& ownIid () const override
    {
        return Vst::IParameterChanges::iid;
    }

    std::vector<RawParamQueue*> queues_;
};

// --------------------------------------------------------------------------
// rawProcess - one IAudioProcessor::process() call carrying a multi-point
// parameter queue for one audio block.
//
// Mirrors VST3PluginInstance::processAudio (juce_VST3PluginFormat.cpp):
// single input + output bus, 32-bit float, realtime mode, the block size in
// numSamples, and inputParameterChanges = the queue (or nullptr when no
// param is active this block). Buffers are the host's live float arrays,
// exactly as JUCE fills them. `component` is the plugin's raw Vst::
// IAudioProcessor (the QueryInterface of JUCE's IComponent), the same object
// JUCE calls process() on.
// --------------------------------------------------------------------------
inline tresult rawProcess (Vst::IAudioProcessor* component,
                           uint32                sampleRate,
                           uint32                numSamples,
                           float**               inCh,
                           uint32                inNumCh,
                           float**               outCh,
                           uint32                outNumCh,
                           RawParamChanges*      changes)
{
    if (component == nullptr || numSamples == 0)
        return kResultFalse;

    // One input bus + one output bus, 32-bit float, realtime. This mirrors
    // exactly the (single-bus, width=max(in,out)) layout JUCE builds for
    // processBlock in VST3PluginInstance::processAudio - JUCE fills the
    // same planar (width, samples) buffer, zero-padding channels beyond the
    // plugin's real width. (The sample rate is not a ProcessData field in the
    // VST3 SDK; the component learns it from setupProcessing, which JUCE's
    // prepareToPlay already issued for us.)
    (void)sampleRate;

    AudioBusBuffers inBuffers  {};
    // The caller hands the host's own live work buffers (float** from
    // std::vector<float*>::data()); inputs are writable by VST3 contract (the
    // plugin may process in place). Mirrors how JUCE hands its writable work
    // buffer to the same call.
    inBuffers.channelBuffers32 = inCh;
    inBuffers.numChannels      = (int32) inNumCh;

    AudioBusBuffers outBuffers {};
    outBuffers.channelBuffers32 = outCh;
    outBuffers.numChannels      = (int32) outNumCh;

    ProcessData pd {};
    pd.symbolicSampleSize     = kSample32;
    pd.processMode            = (int32) ProcessModes::kRealtime;
    pd.numSamples             = (int32) numSamples;
    pd.numInputs              = 1;
    pd.numOutputs             = 1;
    pd.inputs                 = &inBuffers;
    pd.outputs                = &outBuffers;
    pd.inputParameterChanges  =
        (changes != nullptr && changes->count () > 0)
            ? static_cast<IParameterChanges*> (changes)
            : nullptr;
    pd.outputParameterChanges = nullptr;

    return component->process (pd);
}

} // namespace rawv
} // namespace heddle
