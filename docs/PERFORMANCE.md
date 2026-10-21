# Heddle — OSC performance

Measured performance of the OSC control path, at two layers:

1. **Core layer** (`heddle_osc_perf`) — the OSC codec and the
   `classify → resolveSlot → setParam` path, with no JUCE, no audio device,
   no X server. Runs on every CI runner in the `core` job.
2. **Host layer** (`heddle-osc-perf-host`) — the same path as a *client
   actually experiences it*: a real `heddle-host` (`--audio ring`, live
   audio-thread pump, live smoke VST3) driven over UDP OSC. Measures
   set→ack latency as a function of set rate, UDP set loss, and the
   audio-throughput cost of lock contention under sustained load.

Both are **measurements, not thresholds** — a slow number is not a
failure. CI runs them with `continue-on-error: true` and archives the
report as an artifact (`osc-perf-core-<os>`, `osc-perf-host-<os>`).

> Numbers below are from a single local run (see *Environment*). They are
> representative, not a guarantee: absolute values move with CPU, OS, and
> build flags. The *shape* (core path ≪ host drain ceiling; audio cost
> stays flat below the ceiling) is the stable, meaningful result.

---

## Headline

- The **core OSC path is not the bottleneck**. One `setParam` through
  `classify → resolveSlot → setParam` costs **~0.2 µs** (sustained
  **~4.7 Msets/s**). UDP loopback set→ack RTT at the core layer is
  **~16 µs**.
- What a live-rig client actually feels is set by the **host's OSC drain
  cadence**, not the core path. Against a running `heddle-host`,
  set→ack latency floors at **~23.6 ms** (the drain wait + live VST3
  setParam + two UDP hops), and the practical sustainable control rate
  is **~1 000 sets/s**.
- **Below the ceiling the control path is essentially free for audio** —
  the param path is lock-free end to end (JUCE atomic cached values +
  dispatcher queue), so the audio side never blocks or skips while a
  client is firing sets. (An earlier revision of the host held a
  `chainLock_` with a try-lock from the audio side; that design was
  removed and the deficit column in the table is from that era. The
  shape — flat and negligible below the ceiling, rising only in the
  socket-overflow region — holds in the lock-free design; the OSC drain
  cadence, not the param path, is what saturates at ~2 000+ sets/s.)
- **Above the ceiling** (~2 000+ sets/s) the socket receive buffer
  overflows: UDP sets are lost, set→ack RTT degrades by orders of
  magnitude, and lock contention starts to cost audio (deficit rises to
  ~19 % at 10 000 sets/s). This is the *continuous param-adjustment
  smoothness* signal — how much real-time audio is sacrificed while a
  client is turning a knob.

**Bottom line for the use cases** (live rig control, live automation,
viz data source): the OSC control path comfortably handles realistic
controller/automation rates (dozens of params/s, even hundreds). It is
dimensioned for human and curve-automation rates, not for
megahertz-grade param flooding.

---

## Core layer — `heddle_osc_perf`

`heddle_osc_perf` (core, no JUCE). 64-param surface, 200 000 iters,
40 000 loopback samples.

### OSC codec throughput (encode + decode of a 64-param reply)

| stat | value |
|---|---|
| mean | 68.67 µs |
| median | 66.10 µs |
| p99 | 92.80 µs |
| max | 164.80 µs |
| stddev | 8.42 µs |
| throughput | 0.9 Mparam-msg/s |
| wire | 47.88 MB/s |

A full `query-all` (64 params → 64 reply messages) is therefore well
within a millisecond. The codec is cheap; wire format (OSC strings,
4-byte alignment) dominates the per-message size, not the encode.

### Per-op cost of one OSC set (`classify → resolveSlot → setParam`)

This is the path a *continuous param adjustment* exercises — one
`/chain/<slot>/<param> (float)` per op.

| stat | value |
|---|---|
| mean | 0.22 µs |
| median | 0.20 µs |
| p99 | 0.40 µs |
| max | 19.60 µs |
| stddev | 0.11 µs |
| throughput | 4.7 Msets/s |
| jitter (p99/mean) | ×1.86 |
| max rate @ 1 ms/op budget | 4.65 ksets/s |

Sub-microsecond. The core set path is **orders of magnitude** below any
realistic control rate. The only thing that could bound a core-only
client is the 1 ms/op budget (4.65 ksets/s), which no controller or
curve automation approaches.

### UDP loopback (127.0.0.1) one-way + set→ack RTT

| direction | mean | median | p99 | max | stddev |
|---|---|---|---|---|---|
| one-way delivery | 7.82 µs | 7.70 µs | 12.60 µs | 37.90 µs | 1.15 µs |
| set→ack RTT | 16.28 µs | 16.20 µs | 18.20 µs | 63.90 µs | 0.84 µs |

Tight, low-jitter. This is the *network floor* of one control hop on
loopback — the host-layer numbers below show how much the running host
adds on top of it.

---

## Host layer — `heddle-osc-perf-host`

`heddle-osc-perf-host` spawns a real `heddle-host`
(`--audio ring`, live pump, live smoke VST3; block=1024, sr=48000) and
ramps sustained set-rates, 3 s per rate. The client sends
`/chain/smoke/Gain` at each rate and records: set→ack RTT, UDP set loss
(`1 − acks/sent`), and audio output throughput deficit vs the no-load
baseline.

**Baseline** (no OSC load): **504.6 blocks/s** (≈ 504.6 × 1024 / 48000
≈ 10.8× real-time; the ring-mode pump is a 1 ms poller, not a
real-time-locked clock, so baseline throughput is > 1× real-time — see
*Interpretation*).

| rate (sets/s) | sent | acks | loss (%) | RTT median | RTT p99 | blk/s | audio deficit (%) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 50 | 150 | 149 | 0.667 | 23.66 ms | 31.37 ms | 503.6 | 0.2 |
| 200 | 598 | 595 | 0.502 | 23.65 ms | 31.66 ms | 514.1 | −1.9 |
| 500 | 1 497 | 1 489 | 0.534 | 23.36 ms | 31.71 ms | 500.0 | 0.9 |
| **1 000** | 2 996 | 2 979 | **0.567** | **23.65 ms** | **31.70 ms** | 504.0 | **0.1** |
| 2 000 | 5 984 | 3 041 | 49.181 | 747.9 ms | 1 464.4 ms | 495.3 | 1.8 |
| 5 000 | 14 976 | 3 041 | 79.694 | 1 205.3 ms | 2 374.4 ms | 465.1 | 7.8 |
| 10 000 | 29 984 | 3 025 | 89.911 | 1 362.7 ms | 2 683.8 ms | 407.2 | 19.3 |

**Max reliable rate** (loss < 1 % *and* audio deficit < 10 %):
**1 000 sets/s**.

Peak input-ring backlog hit the full 16 384-frame capacity at every
rate (the producer keeps the ring topped up; the pump is the consumer).
The backlog is *not* the saturation signal — audio deficit and RTT are.

### Reading the table

- **50 → 1 000 sets/s: lossless, flat latency, flat audio cost.** RTT
  holds at ~23.6 ms (median) regardless of rate; loss stays < 1 %;
  audio deficit stays within ±2 % of baseline. This is the healthy
  operating region. A real controller or curve automation lives here.
- **The ~23.6 ms RTT floor** is the *effective host drain latency* —
  see *Interpretation* for why it is well above the nominal 5 ms OSC
  timer. It is constant across the healthy region because it is set by
  the drain cadence, not by queuing.
- **2 000 → 10 000 sets/s: saturation.** Loss jumps to 49–90 %, RTT
  median jumps ~30× (23.6 ms → ~0.75–1.36 s) as sets queue in the socket
  buffer ahead of the next drain, and the measured audio deficit climbs
  (1.8 % → 19.3 %) as the message thread's tick fills with drain + apply
  work. This region is beyond any realistic control rate and is
  shown to bound the design, not to target it.

---

## Interpretation

### Why the host RTT floor (~23.6 ms) ≫ the core loopback RTT (~16 µs)

The core-layer loopback RTT (16 µs) is *just* the codec + two UDP hops
with no running host in between. The host adds a **drain wait**: the
host does not process OSC the instant a datagram lands — it polls the
socket inside a JUCE `Timer` callback on the message thread
(`startTimer(5)`, nominally 5 ms), serviced by the message loop
(`runDispatchLoopUntil(20)`). A set therefore waits up to one drain
interval before it is classified, resolved, and applied.

The **measured floor is ~23.6 ms**, which is well above the nominal 5 ms
timer. The gap is not yet fully attributed, but the leading candidates
are:

- the **20 ms message-loop dispatch cadence** (`runDispatchLoopUntil(20)`)
  dominating over the 5 ms timer, i.e. the loop wakes on its 20 ms
  timeout more often than on the 5 ms timer message;
- per-tick message-thread work — `refreshModel()` (polls every slot's
  params) and `guiPull()` (GUI sync) run in the same callback;
- Windows message-queue timer granularity.

Whichever dominates, the practical effect is the same: **one control
hop against a running host costs ~24 ms**, and that is the realistic
"turn a knob" latency a live-rig client experiences. It is *not* the
core OSC path (16 µs) and *not* the set cost (0.2 µs).

> **Optimization lever (not done):** tightening the drain cadence —
> e.g. servicing the OSC socket on a dedicated thread, or shortening the
> `runDispatchLoopUntil` window / moving OSC handling off the
> message-loop timer — would cut the ~23.6 ms floor toward the ~16 µs
> core floor. That is a host-side latency improvement, not a core one,
> and it is deliberately out of scope for the current build: 24 ms is
> well inside a human/automation control budget, so the cost of the
> change is not yet justified by the use cases. Tracked here so the
> measurement is on record if it ever matters.

### Why the ceiling is ~1 000 sets/s (not 4.7 Msets/s)

The core path sustains 4.7 Msets/s, but the *host* drains OSC on a
~24 ms cadence (≈ 42 drains/s) and applies each drained set on the
message thread while holding `chainLock_`. Throughput is bounded by the
drain + apply cadence and the socket receive buffer, not by the core
path. ~1 000 sets/s is where the drain can no longer keep up and the
buffer starts to overflow (loss, RTT blow-up). Again: this ceiling is
set by the host's drain design, far above realistic control rates.

### Audio throughput deficit — the "smoothness of continuous adjustment" signal

In the current (lock-free) design the audio side never blocks on the
param path, so this column from the table above reflects the earlier
`chainLock_` host. The saturation mechanism that still applies today is
the OSC drain: at ~2 000+ sets/s the socket receive buffer overflows and
the message thread spends its whole 5 ms tick draining and applying sets,
which is where the measured deficit (1.8 % → 19.3 % across 2 k→10 k
sets/s) comes from. The conclusion is unchanged:

- **< 2 % up to 1 000 sets/s** — continuous adjustment is effectively
  free for real-time audio.
- Rises to **~19 % at 10 000 sets/s** — at extreme flooding rates the
  message thread is starved of everything but the socket. No realistic
  controller operates there.

This is exactly the "how much real-time audio processing is sacrificed
while turning a knob" question from the use-case review: the answer is
*negligible in the operating region*.

### Note on the >1×-real-time baseline

`heddle-host --audio ring` paces the pump with a **1 ms poller**
(`AudioRingPumpThread`), not a sample-accurate real-time clock. The
baseline therefore runs at ~504.6 blocks/s ≈ 10.8× real-time. This is
fine for the *relative* deficit measurement (all rates are compared
against the same baseline) but means `blk/s` is not a wall-clock
real-time guarantee — in a real DAW the pump is driven by the DAW's
audio thread at exact real-time rate. The deficit *percentage* is the
meaningful, platform-independent number.

---

## In-block curve scheduling — `heddle-osc-ramp-test`

A separate question from the set-rate ceiling above: **how faithfully does a
live param curve render** when waypoints arrive over OSC at various rates?
`heddle-osc-ramp-test` (host) measures it end to end: a running
`heddle-host` (in-block mode) + live smoke VST3, 40 960-sample linear gain
ramp (0.05 → 1.00) delivered as paced real-time OSC sets at a controlled
**sets-per-block (ppb)** rate, 81+ waypoints per run. The output is
line-fitted over the ramp region; `max_err` is the largest absolute
deviation (normalized gain units, full-scale 1.0) and R² measures ramp
shape integrity.

### Headline — the critical rate is 1 set per block

The binding requirement for a block-size-B curve is **at least one
waypoint per block** (B samples apart at 48 kHz = 48000/B sets/s). Below
that, blocks with no waypoint hold flat and the staircase deviates from
the true chord; at and above it, error is a flat floor set by two
quantizations, not by scheduling:

| block | ppb | max_err (full scale) | R² | | block | ppb | max_err | R² |
|---:|---:|---:|---:|---|---:|---:|---:|---:|
| 128 | 0.1 | 0.0248 | 0.9988 | | 256 | 1.0 | 0.0105 | ≥ 0.9994 |
| 256 | 0.1 | 0.0403 | 0.9957 | | 256 | 10 | 0.0106 | ≥ 0.9994 |
| 512 | 0.1 | 0.0714 | 0.9809 | | 512 | 40 | 0.0096 | ≥ 0.9994 |
| 128 | 1.0 | 0.0126 | ≥ 0.9994 | | 1024 | 10 | 0.0140 | ≥ 0.9994 |

(18 configs, blocks 128–2048 × ppb 0.1/1/10/40; the two missing ultra-sparse
cells — 1024/2048 at ppb 0.1 — are skipped by the `nSends ≥ 8` guard.)

- **Below 1 set/block** the error scales with the gap: 2.5 % (128) →
  7.1 % (512) at ppb 0.1 — the classic underrun.
- **At/above 1 set/block** the error is a **flat 0.8–1.6 % floor,
  independent of rate**: R² ≥ 0.9994 everywhere. The floor is waypoint
  *pacing* (a set stamped at the block the audio thread hasn't started
  yet, 2-block lookahead) plus block-boundary offset quantization — a
  staircase artifact of discrete blocks, not a dropped or delayed
  command.
- **No samples lost, no sets dropped at any rate**: all 21 320 waypoints
  acked and all drained (the 512-deep SPSC ring per param absorbs bursts;
  paced sends are the controller's responsibility).

A dump-level check (per-sample output vs the true chord) confirms the
render is a genuine sample-accurate sub-block staircase (flat-run length =
substep, jump = slope × substep) — the measured "deviation" at high ppb is
a global line-fit reference artifact, not a block-resolution defect.

### Dynamic automation cadence

The host's **legacy** (per-block curve) automation tick is therefore
dynamic: it runs at **2 × sampleRate / blockSize Hz** (2 ticks per block —
double the 1/block critical rate, constant headroom at every block size:
750 Hz @128, 187.5 @256, 93.75 @512, 46.9 @1024), computed as a period and
gated inside the constant 5 ms OSC timer so it never slows OSC polling,
and re-scaled when the audio device reports its real block size. It
pushes **only params whose curve still has an upcoming keyframe**
(`hasUpcoming`) — static params and finished curves cost nothing, so the
work scales with the number of curves in motion, not the plugin's
parameter count (real plugins expose hundreds of controls).

### Harness

- Target: `heddle-osc-ramp-test`, links `heddle_core` only — it *spawns*
  the child `heddle-host`.
- Source: `host/src/osc_ramp_test.cpp`.
- Usage:
  ```
  heddle-osc-ramp-test <heddle-host> <smoke-vst3> \
      [--blocks 128,256,512,1024,2048] [--ppb 0.1,1,10,40] \
      [--substep 4] [--settle 1200] [--legacy]
  ```
  `--legacy` runs the same harness against a `--no-inblock` host
  (immediate-latch, no in-block curve) for the A/B comparison.
  `HEDDLE_RAMP_DUMP=<dir>` writes per-config per-sample windows
  (`ramp_<block>_<ppb*10>.txt`) for offline analysis.
- Emits a human table plus a
  `@@HEDDLE_OSC_RAMP@@ … @@END@@` machine block.
- Exit 0 = every requested (block, ppb) config completed.

---

## Harness

### `heddle_osc_perf` (core)

- Target: `heddle_osc_perf`, links `heddle_core` only (no JUCE).
- Source: `core/tools/osc_perf.cpp`.
- Sections: `[codec]` (encode+decode a 64-param reply), `[param]`
  (per-op `classify→resolveSlot→setParam`), `[loopback]` (UDP 127.0.0.1
  one-way + set→ack RTT).
- Emits a human table plus a machine-readable
  `@@HEDDLE_OSC_PERF@@ … @@END@@` block (tab-separated `key<TAB>value`).
- Runs headless on any CI runner (no audio device, no X server).
  Exit 0 on success.

### `heddle-osc-perf-host` (host)

- Target: `heddle-osc-perf-host`, links `heddle_core` only (no JUCE) —
  it *spawns* the child `heddle-host` (which does the JUCE/audio work),
  like `heddle-bridge-test`.
- Source: `host/src/osc_perf_host.cpp`.
- Usage:
  ```
  heddle-osc-perf-host <heddle-host> <smoke-vst3> \
      [--rates 50,200,500,1000,2000,5000,10000] \
      [--duration 3] [--block 1024] [--rate 48000]
  ```
- Per rate: sustained send of `/chain/smoke/Gain`, records set→ack RTT
  (mean/median/p99/max), UDP set loss, audio output throughput + deficit
  vs baseline, peak input-ring backlog.
- Emits a human table plus a
  `@@HEDDLE_OSC_PERF_HOST@@ … @@END@@` machine block.
- Runs on a bare headless runner (no GUI, no audio device, no Xvfb).
  Exit 0 = all rates measured.

### CI wiring

- **`core` job** — `Perf (core OSC benchmark)` step (bash,
  `continue-on-error: true`): locates `heddle_osc_perf` in `build/`,
  runs it, cats the report, uploads `osc-perf-core-<os>`.
- **`host` job** — `Perf (OSC against a real live host)` step (bash,
  `continue-on-error: true`): locates `heddle-osc-perf-host` +
  `heddle-host`, runs it against the built smoke VST3, cats the report,
  uploads `osc-perf-host-<os>`.
- Both upload the report as an artifact *regardless of outcome*
  (`if: always()`), so the numbers are archived even when a slow/flaky
  runner is the cause of a `continue-on-error` non-green.

---

## Environment

| | core run | host run |
|---|---|---|
| date (UTC) | 2026-10-17 16:41 | 2026-10-17 16:41 |
| OS | Windows 10 | Windows 10 |
| toolchain | MSYS2 ucrt64 g++ 16.2.0 | MSVC 2022 (VS 17) |
| build type | Release (CMake 4.4.4, Ninja) | Release (CMake, VS 17 2022) |
| host | — | `heddle-host` v0.1.7, `--audio ring`, smoke VST3, block=1024, sr=48000 |

Reproduce locally:
```bash
# core (MSYS2 ucrt64)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target heddle_osc_perf
./build/heddle_osc_perf

# host (MSVC, HEDDLE_BUILD_HOST=ON)
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release -DHEDDLE_BUILD_HOST=ON
cmake --build build-msvc --config Release --target heddle-host heddle_smoke_plugin_VST3 heddle-osc-perf-host
./build-msvc/host/Release/heddle-osc-perf-host.exe \
    build-msvc/host/Release/heddle-host.exe \
    "<path to> HeddleSmokePlugin.vst3"
```

---

*Related: [OSC.md](OSC.md) (the OSC surface), [ARCHITECTURE.md](ARCHITECTURE.md)
(threading + lock model behind the deficit measurement),
[ALIGNMENT.md](ALIGNMENT.md) (which use cases these numbers serve).*
