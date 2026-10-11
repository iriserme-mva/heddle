# Weft architecture

## Goal

A sellable C++/JUCE application that chains VST2/VST3 plugins, describes the
rig in JSON, and exposes every parameter live over OSC — usable online (as a
host app with an IO-linking GUI) and offline (CLI, audio→audio with fixed or
time-series params).

## Layering

```
┌────────────────────────────────────────────────────────────┐
│  host/ (JUCE)                                              │
│  - JUCE VST2/3 backend (IPluginBackend)                    │
│  - weft_smoke: headless load + param introspection CLI     │
│  - weft-render: offline audio→audio renderer (WAV in/out)  │
│  - (planned) full host app: OSC server + GUI (IO linking), │
│    ASIO / virtual-cable audio routing                      │
├────────────────────────────────────────────────────────────┤
│  core/  (platform-agnostic C++17, MIT)                     │
│  - Param/ParamSet model        (param.hpp)                 │
│  - time-series params, static|curve (curve.hpp)            │
│  - Chain + IPluginBackend      (chain.hpp)                 │
│  - JSON config parse/dump/merge (config.hpp)               │
│  - OSC 1.0 codec + addressing (osc.hpp)                    │
│  - UDP OSC transport, source tracking (osc_io.hpp)         │
└────────────────────────────────────────────────────────────┘
```

### The key split (user-directed)

> The **inner VST wrapper describes the param interface**; the **outer app
> performs the actual OSC transport**.

Concretely:

- `IPluginBackend::load(slot, ParamSet&)` — the backend enumerates *all*
  params of a plugin into a normalized `ParamSet` (id, title, units, min/max
  linear range, steps, kind, default).
- `IPluginBackend::setParam(slotId, id, norm)` — live param setting.
- The core `Chain` keeps the per-slot `ParamSet` state, applies config values,
  resolves OSC slot tokens, and emits `onChange` callbacks.
- The *app* (JUCE host) binds `OscUdp`, classifies incoming messages with
  `osc_addr::classify`, drives the `Chain`, and sends replies/pushes. The core
  never touches a plugin SDK or owns the socket lifetime policy of the app.

This keeps the core unit-testable with a mock backend (see
`core/tests/test_chain.cpp`) and lets the same core serve both the JUCE app
and the offline CLI.

## Data flow

```
JSON config ──parse──► ChainConfig ──load/apply──► Chain ──► ParamSet per slot
                              │
                              └─ (hot)apply: unchanged paths NOT reloaded
OSC cmd ──classify──► ParamAction ──► Chain::setParam/reset/apply
Plugin UI / internal automation ──► Chain::onPluginParamChanged
                              └─► onChange ──► /change/<slot>/<param> push
```

### Hot-apply semantics

`Chain::apply(cfg)`:

1. For each configured slot, find the loaded slot with the same id.
2. If the plugin path changed (or the slot was disabled), reload exactly that
   slot; otherwise keep the loaded instance.
3. Re-apply all configured params (normalized, clamped to `[0,1]`).
4. Append newly added slots; remove nothing (slots absent from the new config
   stay loaded until explicitly reset/replaced — deterministic and safe for
   live sessions).

### Param normalization

Params are normalized to `[0,1]` everywhere in the core. The backend converts
to/from plugin units (and `min`/`max`/`steps` describe the linear range for
display). Enum/bool params carry `kind` and `listItems` so the app can render
proper widgets.

## Testing strategy

Targeted, meaningful tests only (no throwaway filler):

- `test_osc.cpp` — wire codec roundtrips (float/int/string/blob), malformed
  input rejection, address classification, reply construction.
- `test_config.cpp` — schema validation, clamping, dump round-trip, merge
  semantics, file I/O.
- `test_chain.cpp` — mock backend: load + configured-params-on-defaults,
  hot-apply reload accounting, reset, slot resolution (id and index),
  change propagation.
- `test_curve.cpp` — curve parse/eval: clamping, interpolation, keyframe
 ordering rules, static-vs-curve `ParamSpec`.
- `test_osc_udp.cpp` — UDP loopback: bind/openOut/poll roundtrip, sender
 capture, `sendTo()` reply routing.

Build & test: CMake + `ctest` (doctest). CI: GitHub Actions, same commands.

## Roadmap (tracked in STATUS.md)

1. **core** (done, CI green) — model, chain, config, OSC, transport;
   time-series params (static|curve) done locally, CI pending (unpushed).
2. **host/** — JUCE VST3 backend implementing `IPluginBackend` (done, CI
   green); `weft_smoke` introspection CLI (done, CI green); `weft-render`
   offline audio→audio with static + curve params (done; curve automation
   CI pending); full host app with OSC server + minimal GUI + IO linking
   between chain in/out and system devices (ASIO / virtual cables) —
   in progress.
3. **Polish** — docs, packaging, license review for the JUCE layer, samples.
