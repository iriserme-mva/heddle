# Heddle — solution ↔ use-case alignment

This document analyses how the Heddle design fits the use cases it was built
for, where the fit is exact, and where it only approximates. It is a
design review, not a test report — measured numbers live in
[PERFORMANCE.md](PERFORMANCE.md).

## The use cases (from README + ARCHITECTURE)

1. **Live rig control from a DAW / host** — drive every plugin param of a
   running chain over OSC (Ableton Max, Live, a Python script, a hardware
   controller bridged to OSC). Set by name or id, query the whole surface,
   receive `/change` pushes when the plugin moves its own param.
2. **Versionable rig description** — one JSON file describes the whole chain
   (slots, plugin paths, per-param values), diff-able in git, hot-applicable
   without reloading unchanged plugins.
3. **Offline render** — audio→audio rendering (CLI, no device) with static
   *and* time-series (curve) params, so a designed automation can be baked
   to WAV.
4. **Live automation** — curve params in the config drive params on the
   running chain in real time (the "draw the envelope, watch it happen"
   workflow).
5. **Visualization data source** — the normalized param surface
   (query-all → one structured line per param; set → ack; plugin → push) is
   a clean machine-readable event stream for external viz tooling
   (token spaces, training-run plots, distribution monitors).

## How each maps to the design

### 1. Live rig control — fit: exact, with two known cost points

- The **core/app split** (inner wrapper describes, outer app transports)
  is exactly what a DAW-facing rig needs: the protocol is transport-agnostic
  (OSC over UDP today; the `OscUdp` layer is a thin, replaceable facade),
  while the param model (`ParamSet`) is stable and SDK-free.
- **Set by name or id** covers both human-authored clients (name) and
  machine-generated ones (id, from a prior query-all). The disambiguation
  rule (1 float = by name; int+float = by id) is simple and documented in
  [OSC.md](OSC.md).
- **Query-all returns the full metadata line** (id, title, value, min/max,
  steps, kind, enum labels) — a client can build its whole control surface
  from one round trip, no hardcoding.
- **Pushes** (`/change/<slot>/<param>`) close the loop for plugin-internal
  changes; the host polls the live plugin values on its 5 ms tick and only
  emits pushes on actual change, so the stream stays quiet.
- **Known cost points** (measured, not speculative — see PERFORMANCE.md):
  - the OSC drain is a **5 ms timer**, not edge-triggered: worst-case a set
    waits up to 5 ms to be noticed, and sustained rates above ~1–2 ksets/s
    overflow the socket buffer (drops). For DAW use this is far below
    human/automation relevance; it *is* the ceiling for max reliable rate.
  - the audio path **try-locks** the chain lock and skips a block on
    contention (silence / re-tick). Under pathological set rates this shows
    up as an audio throughput deficit — measured as a function of set rate.

### 2. Versionable rig description — fit: exact

- `ChainConfig` ↔ JSON is a plain bijective mapping (`config.cpp`
  parse/dump round-trip is tested). Slot `id`s are stable keys, plugin paths
  are values — diff-able.
- **Hot-apply semantics** are deliberately conservative: unchanged paths are
  never reloaded (no audible glitch, no plugin state loss), new slots are
  appended, absent slots stay loaded (deterministic for live sessions). This
  matches "edit the JSON in the DAW session, `/chain/apply`, keep mixing".
- Normalized `[0,1]` values keep the JSON portable across plugins that
  express the same concept in different units.

### 3. Offline render — fit: exact

- `heddle-render` re-uses the *same* `JuceBackend` load/process/param-push
  path as the live host, so config semantics are identical online and
  offline (the param-push regression is locked by a CI ratio test).
- Curves are first-class in the config (`{"curve": [[frame, value], ...]}`)
  and evaluated per block — the CI render test proves a non-trivial ramp
  reaches the plugin with the correct mean gain.

### 4. Live automation — fit: exact

- Curve params are evaluated in the audio callback at block resolution
  (1024 frames @ 48 kHz ≈ 21 ms), with change detection (the backend is only
  called when the value actually moves) and **user-override precedence**:
  an OSC `set` locks the param against automation until reset/apply, which
  is the expected "hand takes over from the envelope" behavior.

### 5. Visualization data source — fit: good, one structural note

- The protocol is already an event stream: structured query replies, acks
  carrying the *post-set* value, and change pushes. A viz client needs no
  extra plumbing — bind, query, subscribe.
- **Structural note:** there is no *batch* message (one datagram setting N
  params). For viz *ingestion* that is irrelevant (events are already
  per-param, which is the right granularity). For high-rate *control* it is
  the natural next optimization (see PERFORMANCE.md, "where the time goes").

## Where the design deliberately stops

- **No GUI in this snapshot.** The host app (IO linking, ASIO / virtual
  cables) is on the roadmap; today the control surface is OSC + JSON, which
  is what use cases 1–5 actually consume.
- **No parameter *routing* graph** (chain is linear). A sellable product may
  want branching (parallel chains, send/return). The `Chain` model is
  single-pass by construction; adding a DAG would be a core change, not a
  host tweak.
- **Plugin param changes are polled, not event-driven.** JUCE has no
  change-callback in this build, so plugin-internal moves surface within
  one 5 ms tick. Fine for live use; a polling-rate knob would help
  low-latency-viz consumers.

## Bottom line

| Use case | Fit | Main caveat |
|---|---|---|
| Live rig control (OSC) | exact | 5 ms drain cadence; socket-buffer ceiling at extreme rates |
| Versionable rig JSON | exact | hot-apply keeps absent slots loaded (intentional) |
| Offline render (static + curve) | exact | none |
| Live automation (curves) | exact | block-resolution granularity (~21 ms @ 1024/48k) |
| Viz data source | good | no batch message (granularity is per-param by design) |

The design's center of gravity — *normalized param model + transport-free
core + JSON rig + OSC event surface* — is exactly where all five use cases
converge. The measured performance headroom (PERFORMANCE.md) shows the core
path is ~1000× faster than any realistic control rate, so the use-case
constraints are dominated by the two deliberate policy choices (5 ms drain,
try-lock audio), both of which are documented, tunable, and — for DAW-paced
control — far from the measured ceilings.
