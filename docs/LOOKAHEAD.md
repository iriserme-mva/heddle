# Lookahead & default ramp — live vs offline

This page pins down one recurring question: *does offline rendering apply the
host's 2-block lookahead / default ramp, or does it honour the automation
exactly as written?*

**Short answer: offline has no lookahead and no implicit ramp. The live and
offline paths are separate, and only the live path has a lookahead.** No code
was changed for this — it documents the behaviour that is already true.

## The two paths are independent

| | **Live host** (`heddle-host`) | **Offline renderer** (`heddle-render`) |
|---|---|---|
| Automation source | OSC `/chain/<slot>/<param>` sets arriving over UDP | keyframe curves in the config JSON (`"param": {"curve": [[frame, value], …]}`) |
| Timing model | async: a set can land at any instant mid-render | **all controls known in advance** (parsed before block 0) |
| Lookahead | `kLookahead = 2` blocks | **none** |
| Default ramp | an OSC set with no duration ramps over `kLookahead` (2) blocks | **no default ramp** — value is exactly the keyframe interpolation |
| Delivery to plugin | raw `IAudioProcessor::process` with a multi-point `IParamValueQueue`; plugin interpolates **per-sample** | `setValueNotifyingHost` **once per block**, at the block-start sample (block-resolution latch) |
| Code | `host/src/host_main.cpp` (`scheduleCurveSet`, `prepareCurvesForBlock`, `buildQueuePts`) | `host/src/render.cpp` (`heddle::evalParams` + `JuceBackend::setParams`) |

They share only `core/` (the OSC codec, the `Curve`/`ParamSpec` model, the
chain). The live lookahead is a **host** constant (`kLookahead`) that the
offline renderer never references.

## Why the live path keeps the 2-block lookahead

`kLookahead = 2` does two jobs, both of which exist **only because live sets
arrive asynchronously**:

1. **Default ramp length** — an OSC set that sends no explicit duration ramps
   over 2 blocks. This is a *gesture default* (a control-surface move should
   not hard-step the audio), not a latency requirement.
2. **Bendable window** — blocks `b` and `b+1` are committed; block `b+2` and
   beyond stay bendable so a late-arriving set can be scheduled into them
   without retroactively moving already-rendered audio (the click-free anchor
   re-resolution).

Offline has neither pressure: nothing arrives late, so there is no window to
keep open, and a written automation point is authoritative, not a gesture.

## What "exact" means offline (the one thing to know)

Offline automation is **block-start sampled**, not per-sample:

- At the start of each block the renderer evaluates every keyframed param at
  that block's first sample and latches the value for the whole block
  (`render.cpp` → `evalParams(params, blockStartFrame)` → `setParams`).
- Between keyframes the value is **linear** (`Curve::eval`); before the first
  keyframe it clamps to the first value, after the last it clamps to the last.
- Consequence: automation finer than one block is downsampled to block
  resolution (intermediate keyframe values inside a block are not seen), and a
  step between two keyframes happens at the block boundary, not at the exact
  keyframe sample.

So offline is *control-accurate in value* (no phantom ramp, no lookahead
offset) but *block-quantised in time*. That is the correct trade for an
offline renderer — it is deterministic and sample-rate exact, and the
block-size knob (`block_size` in the config) sets its automation time
resolution. The **live** host, by contrast, is per-sample time-exact (the raw
`IParamValueQueue` carries in-block kinks, so a 0.5-block offset renders at the
true sample).

## Decision (2026-10-23)

Keep the split as-is — do **not** unify offline onto the live scheduler:

- **Live:** keep `kLookahead = 2` (default ramp + bendable window). This is the
  behaviour real VST3 hosts / DAWs expect for asynchronous control changes, and
  it is byte-identical to the verified in-block path.
- **Offline:** keep it lookahead-free and ramp-free (keyframe-linear, block
  latch). Controls are given in advance, so a 2-block implicit ramp would be a
  correctness bug — it would move an explicit automation point.

If a future need arises for per-sample offline automation (e.g. keyframes
closer than one block must render exactly), the right change is a *renderer*
upgrade to a per-sample queue path — not importing the live lookahead into the
offline pass.
