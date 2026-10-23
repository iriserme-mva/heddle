// Heddle probe plugin — a RAW (no JUCE wrapper) VST3 DLL used to prove that
// the host can drive in-block parameter curves through the VST3
// IParamValueQueue model instead of slicing the block into latched sub-blocks.
//
// Two objects, no diamond:
//   * ProbeAudio     -- implements IComponent + IAudioProcessor + IConnectionPoint
//                       DIRECTLY (JUCE does exactly this in JuceVST3Component).
//                       A single refcount + a single hand-written queryInterface
//                       serve every base, so there is no multiple-inheritance
//                       ambiguity. process() honours the FULL per-block
//                       IParamValueQueue (multiple (offset,value) points) by
//                       linearly interpolating between points -- the VST3
//                       SDK contract. With one offset-0 point it is a plain
//                       latch (what JUCE's wrapper sends per sub-block call).
//   * ProbeController-- extends Vst::EditController (the SDK base fills all the
//                       non-parameter IEditController pure virtuals; we only add
//                       the parameter surface so JUCE can enumerate "Gain" and
//                       "PWM pct").
//
// The factory (IPluginFactory) is hand-written because CPluginFactory is not in
// the bundled SDK. GetPluginFactory is the module export the host calls.
//
// DSP is IDENTICAL to the JUCE smoke plugin (host/test_plugin/plugin.cpp) so the
// expected-curve model transfers 1:1:
//     out[n] = in[n] * gain * gate[n]
//   with a fixed PERIOD=1024 gate that is ON for round(duty*PERIOD) samples of
//   each period (phase is a member that survives across process calls). gain
// and duty are the "Gain" and "PWM pct" parameters, both linear 0..1 (no dB).

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// The probe is a pure VST3 DLL: it links the Steinberg SDK objects itself. The
// host already builds the same SDK (via juce_audio_processors), but that is a
// separate module with its own object set -- no cross-target symbol clash.
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "public.sdk/source/vst/vstbus.h"
#include "public.sdk/source/vst/vstparameters.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

// The bundled SDK's vstinitiids.cpp does not define IPlugView::iid -- JUCE
// supplies it itself in juce_VST3Headers.h's "Missing IIDs" block. We compile
// our own copy of the SDK objects (vsteditcontroller.cpp references
// IPlugView::iid from CPluginView::queryInterface), so we define it here too.
DEF_CLASS_IID (IPlugView)

namespace heddle_probe
{

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Copy an ASCII (char8) string into a VST3 String128 (char16[64]). The probe's
// strings are pure ASCII, so a byte-wise widen is exact.
static void setStr (String128& dst, const char8* src)
{
    for (int i = 0; i < 64; ++i)
    {
        dst[i] = src[i] != '\0' ? static_cast<char16> (src[i]) : char16 (0);
    }
}

// GUID compare. This bundled SDK has no free isUuid(); JUCE's own client uses
// a plain memcmp of the TUIDs, which we replicate here.
static bool uidEq (const TUID a, const TUID b)
{
    return std::memcmp (a, b, sizeof (TUID)) == 0;
}

// Param IDs. These are the RAW VST3 ParamID values JUCE surfaces (as a decimal
// string) and the host round-trips as heddle::Param.id -- so the host's config
// (which addresses params by name) maps onto exactly these.
static const ParamID kParamGain = 1;
static const ParamID kParamDuty = 2;

// Identical gate to the smoke plugin.
static const int32 kGatePeriod = 1024;

// The controller's class iid, defined here (before ProbeAudio) so
// ProbeAudio::getControllerClassId can reference it without a forward class
// declaration. The matching FUID for the ProbeController class object uses the
// same bytes (see ProbeController::iid below).
static const TUID kProbeControllerUid = INLINE_UID (0xABCD0002, 0x00000002,
                                                    0x00000002, 0x48454444);

// ---------------------------------------------------------------------------
// ProbeAudio : IComponent + IAudioProcessor + IConnectionPoint (direct)
// ---------------------------------------------------------------------------
class ProbeAudio
    : public IComponent
    , public IAudioProcessor
    , public IConnectionPoint
{
public:
    static const FUID iid;

    // --- FUnknown (single refcount serves all three FUnknown bases) --------
    uint32 PLUGIN_API addRef () override
    {
        return static_cast<uint32> (++refCount_);
    }
    uint32 PLUGIN_API release () override
    {
        const int32 r = --refCount_;
        if (r == 0) delete this;
        return static_cast<uint32> (r);
    }
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (obj == nullptr)
            return kInvalidArgument;

        // Disambiguate the FUnknown upcast by casting through the most-derived
        // type first (mirrors JUCE's testFor), avoiding the multi-FUnknown-base
        // ambiguity.
        if (uidEq (iid, IPluginBase::iid) || uidEq (iid, IComponent::iid))
        {
            addRef ();
            *obj = static_cast<IComponent*> (static_cast<ProbeAudio*> (this));
        }
        else if (uidEq (iid, IAudioProcessor::iid))
        {
            addRef ();
            *obj = static_cast<IAudioProcessor*> (static_cast<ProbeAudio*> (this));
        }
        else if (uidEq (iid, IConnectionPoint::iid))
        {
            addRef ();
            *obj = static_cast<IConnectionPoint*> (static_cast<ProbeAudio*> (this));
        }
        else if (uidEq (iid, FUnknown::iid) || uidEq (iid, ProbeAudio::iid))
        {
            // Route the FUnknown upcast through IPluginBase (a single
            // unambiguous path) rather than directly from the most-derived
            // type, which has several FUnknown bases.
            addRef ();
            *obj = static_cast<FUnknown*> (static_cast<IPluginBase*> (
                static_cast<ProbeAudio*> (this)));
        }
        else
        {
            *obj = nullptr;
            return kNotImplemented;
        }
        return kResultOk;
    }

    // --- IConnectionPoint --------------------------------------------------
    tresult PLUGIN_API connect (IConnectionPoint*) override  { return kNotImplemented; }
    tresult PLUGIN_API disconnect (IConnectionPoint*) override { return kNotImplemented; }
    tresult PLUGIN_API notify (IMessage*) override          { return kNotImplemented; }

    // --- IPluginBase (via IComponent) -------------------------------------
    tresult PLUGIN_API initialize (FUnknown* /*context*/) override
    {
        active_ = false;
        processing_ = false;
        return kResultOk;
    }
    tresult PLUGIN_API terminate () override
    {
        active_ = false;
        processing_ = false;
        return kResultOk;
    }

    // --- IComponent --------------------------------------------------------
    tresult PLUGIN_API getControllerClassId (TUID classID) override
    {
        std::memcpy (classID, kProbeControllerUid, sizeof (TUID));
        return kResultTrue;
    }
    tresult PLUGIN_API setIoMode (IoMode) override         { return kNotImplemented; }
    int32 PLUGIN_API getBusCount (MediaType type, BusDirection dir) override
    {
        if (type != kAudio)
            return 0;
        return dir == kInput ? 1 : 1;
    }
    tresult PLUGIN_API getBusInfo (MediaType type, BusDirection dir, int32 index,
                                  BusInfo& info) override
    {
        if (type != kAudio || index != 0)
            return kInvalidArgument;
        std::memset (&info, 0, sizeof (info));
        info.mediaType = type;
        info.direction = dir;
        info.channelCount = 2;
        info.busType = kMain;
        info.flags = BusInfo::kDefaultActive;
        setStr (info.name, dir == kInput ? "Input" : "Output");
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo (RoutingInfo&, RoutingInfo&) override
    {
        return kNotImplemented;
    }
    tresult PLUGIN_API activateBus (MediaType type, BusDirection, int32, TBool) override
    {
        return (type == kAudio) ? kResultOk : kInvalidArgument;
    }
    tresult PLUGIN_API setActive (TBool state) override
    {
        active_ = (state != 0);
        return kResultOk;
    }
    tresult PLUGIN_API setState (IBStream*) override { return kNotImplemented; }
    tresult PLUGIN_API getState (IBStream*) override { return kNotImplemented; }

    // --- IAudioProcessor ---------------------------------------------------
    tresult PLUGIN_API setBusArrangements (SpeakerArrangement*, int32,
                                           SpeakerArrangement*, int32) override
    {
        return kResultTrue;
    }
    tresult PLUGIN_API getBusArrangement (BusDirection dir, int32,
                                          SpeakerArrangement& arr) override
    {
        arr = Vst::SpeakerArr::kStereo;
        (void) dir;
        return kResultTrue;
    }
    tresult PLUGIN_API canProcessSampleSize (int32 symbolicSampleSize) override
    {
        return (symbolicSampleSize == kSample32) ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples () override { return 0; }
    uint32 PLUGIN_API getTailSamples () override    { return 0; }

    tresult PLUGIN_API setupProcessing (ProcessSetup& setup) override
    {
        if (canProcessSampleSize (setup.symbolicSampleSize) != kResultTrue)
            return kResultFalse;
        sampleRate_ = setup.sampleRate;
        maxSamples_ = setup.maxSamplesPerBlock;
        gatePhase_ = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing (TBool state) override
    {
        processing_ = (state != 0);
        gatePhase_ = 0;
        return kResultOk;
    }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        const int32 numSamples = data.numSamples;
        if (numSamples <= 0)
            return kResultOk;

        // Resolve every queued point per parameter into a per-sample value.
        // The SDK contract: each point (offset, value) is a control point; the
        // value between consecutive points is the LINEAR interpolation. A lone
        // offset-0 point (what a latching host sends) degenerates to a constant
        // for the whole block. Heap-sized to numSamples (a whole-block queue can
        // be as long as the audio block, which need not be 256).
        std::vector<float> gain (numSamples), duty (numSamples);
        resolveParam (data.inputParameterChanges, kParamGain, gain.data(), numSamples);
        resolveParam (data.inputParameterChanges, kParamDuty, duty.data(), numSamples);

        const int32 numChannels = data.numInputs > 0 ? data.inputs->numChannels : 0;
        const int32 outChannels = data.numOutputs > 0 ? data.outputs->numChannels : 0;
        if (numChannels == 0 || outChannels == 0)
            return kResultOk;

        const Sample32* const* in  = data.inputs->channelBuffers32;
        Sample32**             out = data.outputs->channelBuffers32;
        if (in == nullptr || out == nullptr)
            return kResultOk;

        int32 phase = gatePhase_;
        for (int32 s = 0; s < numSamples; ++s)
        {
            // Duty is normally constant per block (the host queues one point);
            // honour it per-sample so the probe still matches the smoke DSP if
            // it ever ramps.
            const int32 onSamples = static_cast<int32> (
                std::lround (static_cast<double> (duty[s]) * static_cast<double> (kGatePeriod)));
            const float g = (phase < onSamples) ? gain[s] : 0.0f;
            phase = (phase + 1) % kGatePeriod;
            for (int32 c = 0; c < numChannels && c < outChannels; ++c)
                out[c][s] = in[c][s] * g;
        }
        gatePhase_ = phase;
        return kResultOk;
    }

private:
    // Fill `values[0..numSamples)` with the linearly-interpolated normalized
    // value of the queue entry for `paramId`. Falls back to the last latched
    // value (or the default) if the param has no queue entry this block.
    void resolveParam (IParameterChanges* changes, ParamID paramId,
                       float* values, int32 numSamples)
    {
        // Default = current latched value (the value the plugin last applied).
        float hold = (paramId == kParamGain) ? gain_ : duty_;

        IParamValueQueue* q = nullptr;
        if (changes != nullptr)
        {
            const int32 count = changes->getParameterCount ();
            for (int32 i = 0; i < count; ++i)
            {
                q = changes->getParameterData (i);
                if (q != nullptr && q->getParameterId () == paramId)
                {
                    const int32 pts = q->getPointCount ();
                    // Gather points in sample order (whole-queue path: up to
                    // ~2 points per set per block, unbounded count).
                    int32* offs = new int32[pts < 1 ? 1 : pts];
                    float* vals = new float[pts < 1 ? 1 : pts];
                    int32 n = 0;
                    for (int32 p = 0; p < pts; ++p)
                    {
                        int32 off = 0;
                        ParamValue v = 0.0;
                        if (q->getPoint (p, off, v) == kResultOk)
                        {
                            if (off < 0) off = 0;
                            if (off > numSamples) off = numSamples;
                            offs[n] = off;
                            vals[n] = static_cast<float> (v);
                            ++n;
                        }
                    }
                    // De-duplicate / order is guaranteed by the SDK (non-
                    // decreasing offset). If all offsets are equal (a single
                    // latched point), hold the value for the whole block.
                    if (n == 1)
                    {
                        for (int32 s = 0; s < numSamples; ++s)
                            values[s] = vals[0];
                        hold = vals[0];
                    }
                    else
                    {
                        size_t seg = 0;
                        for (int32 s = 0; s < numSamples; ++s)
                        {
                            while (seg + 1 < n && offs[seg + 1] <= s)
                                ++seg;
                            if (seg + 1 >= n)
                                values[s] = vals[seg];
                            else
                            {
                                const int32 a = offs[seg];
                                const int32 b = offs[seg + 1];
                                const float va = vals[seg];
                                const float vb = vals[seg + 1];
                                const float t = (b > a)
                                    ? static_cast<float> (s - a) / static_cast<float> (b - a)
                                    : 0.0f;
                                values[s] = va + (vb - va) * t;
                            }
                        }
                        hold = vals[n - 1];
                    }
                    gain_ = (paramId == kParamGain) ? hold : gain_;
                    duty_ = (paramId == kParamDuty) ? hold : duty_;
                    delete[] offs;
                    delete[] vals;
                    return;
                }
            }
        }
        for (int32 s = 0; s < numSamples; ++s)
            values[s] = hold;
    }

    std::atomic<int32> refCount_ { 1 };
    bool active_ = false;
    bool processing_ = false;
    double sampleRate_ = 44100.0;
    int32 maxSamples_ = 512;
    int32 gatePhase_ = 0;
    float gain_ = 1.0f;   // last applied normalized values (defaults 1.0)
    float duty_ = 1.0f;
};

const FUID ProbeAudio::iid (TUID INLINE_UID (0xABCD0001, 0x00000001,
                                             0x00000001, 0x48454444));  // "HEDD"

// ---------------------------------------------------------------------------
// ProbeController : Vst::EditController (SDK base fills the non-param virtuals)
// ---------------------------------------------------------------------------
class ProbeController : public Vst::EditController
{
public:
    static const FUID iid;

    ProbeController ()
    {
        // Two continuous 0..1 params, defaults 1.0 (fully open gain, full duty).
        // The controller's class id is registered by the factory (getClassInfo);
        // it does not need to set one here. Vst::EditController's base already
        // provides a headless createView (returns nullptr) and all the non-param
        // IEditController bodies.
        Vst::ParameterInfo info;
        std::memset (&info, 0, sizeof (info));
        info.id = kParamGain;
        setStr (info.title, "Gain");
        info.defaultNormalizedValue = 1.0f;
        info.flags = Vst::ParameterInfo::kCanAutomate;
        parameters.addParameter (info);

        std::memset (&info, 0, sizeof (info));
        info.id = kParamDuty;
        setStr (info.title, "PWM pct");
        info.defaultNormalizedValue = 1.0f;
        info.flags = Vst::ParameterInfo::kCanAutomate;
        parameters.addParameter (info);
    }
};

const FUID ProbeController::iid (kProbeControllerUid);

// ---------------------------------------------------------------------------
// ProbeFactory : IPluginFactory (hand-written; CPluginFactory not in SDK)
// ---------------------------------------------------------------------------
class ProbeFactory : public IPluginFactory
{
public:
    uint32 PLUGIN_API addRef () override      { return static_cast<uint32> (++refCount_); }
    uint32 PLUGIN_API release () override
    {
        const int32 r = --refCount_;
        if (r == 0) delete this;
        return static_cast<uint32> (r);
    }
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (obj == nullptr)
            return kInvalidArgument;
        if (uidEq (iid, IPluginFactory::iid) || uidEq (iid, FUnknown::iid))
        {
            addRef ();
            *obj = static_cast<IPluginFactory*> (this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNotImplemented;
    }

    tresult PLUGIN_API getFactoryInfo (PFactoryInfo* info) override
    {
        if (info == nullptr)
            return kInvalidArgument;
        *info = PFactoryInfo ("Heddle", "https://github.com/iriserme-mva/heddle",
                              "iris.mv.assistant@gmail.com", 0);
        return kResultOk;
    }

    int32 PLUGIN_API countClasses () override { return 2; }

    tresult PLUGIN_API getClassInfo (int32 index, PClassInfo* info) override
    {
        if (info == nullptr)
            return kInvalidArgument;
        if (index == 0)
            *info = PClassInfo (ProbeAudio::iid, PClassInfo::kManyInstances,
                                kVstAudioEffectClass, "HeddleProbeAudio");
        else if (index == 1)
            *info = PClassInfo (ProbeController::iid, PClassInfo::kManyInstances,
                                kVstComponentControllerClass, "HeddleProbeController");
        else
            return kInvalidArgument;
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (FIDString cid, FIDString iid, void** obj) override
    {
        if (cid == nullptr || iid == nullptr || obj == nullptr)
            return kInvalidArgument;
        *obj = nullptr;

        // Copy the requested iid into a TUID (FIDString is const char8*, but
        // queryInterface takes TUID = char[16]) -- same as JUCE's factory.
        TUID iidToQuery;
        std::memcpy (iidToQuery, iid, sizeof (TUID));

        if (std::memcmp (cid, ProbeAudio::iid, sizeof (TUID)) == 0)
        {
            // Hold the concrete object directly; call the (unambiguous,
            // most-derived) queryInterface/release on it, then hand the
            // interface pointer to the host. Never form a bare FUnknown* from
            // the multi-FUnknown-base most-derived type.
            ProbeAudio* a = new ProbeAudio ();
            const tresult r = a->queryInterface (iidToQuery, obj);
            a->release ();
            return r;
        }
        if (std::memcmp (cid, ProbeController::iid, sizeof (TUID)) == 0)
        {
            ProbeController* c = new ProbeController ();
            const tresult r = c->queryInterface (iidToQuery, obj);
            c->release ();
            return r;
        }
        return kNotImplemented;
    }

private:
    std::atomic<int32> refCount_ { 1 };
};

}  // namespace heddle_probe

// ---------------------------------------------------------------------------
// Module export. Canonical VST3 entry point: the host loads the DLL, resolves
// GetPluginFactory, calls getFactoryInfo()/countClasses()/getClassInfo() to
// enumerate, then createInstance(cid, iid, ...) to pull the audio component and
// the controller. The return type is IPluginFactory* (ProbeFactory derives from
// it, so `new ProbeFactory()` converts implicitly).
// ---------------------------------------------------------------------------
extern "C"
{
    SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory ()
    {
        return new heddle_probe::ProbeFactory ();
    }
}
