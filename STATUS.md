# Heddle — build status

Updated by whoever works on the project (human or agent). **Keep this file
current after every session** — last section first.

## Legend

- ✅ done · 🔨 in progress · ⏸ blocked (state why) · 📅 planned

## Current state (2026-10-16)

| Layer | Status |
|---|---|
| core: param/chain/config/osc/osc_io | ✅ builds, 39/39 test cases, 253/253 assertions |
| core: param time-series (static\|curve) parse/dump/merge/eval | ✅ **CI GREEN** (run 38220293171, all 3 OSes) |
| host: `heddle-render` per-slot curve automation | ✅ **CI GREEN** (run 38220293171: curve mean-gain 0.6875 assert on all 3 OSes) |
| osc: source-address tracking + `sendTo()` reply routing | ✅ **CI GREEN** (run 38220293171, udp loopback test) |
| core: CMake + local build | ✅ (MSYS2 ucrt64 g++ 16.2, cmake 4.4, Ninja) |
| core: CI (GH Actions, 3-OS) | ✅ green 3/3 (macOS lane pinned `macos-15` — `macos-latest`→`macos-26` pool flaky, see log 2026-10-15 (2)) |
| host: JuceBackend (IPluginBackend over JUCE, VST3) | ✅ written, API-verified against JUCE 8.0.4 source |
| host: test VST3 plugin (HeddleSmokePlugin) + smoke CLI | ✅ written (testvst: Gain float 0..1 def 1.0; PWM pct float 0..1 def 1.0 = 1024-sample on/off gate duty cycle) |
| host: `heddle-host` standalone live chain host (headless-ready, OSC-driven) | ✅ **CI GREEN** (run 38525679263, `0fdb134`): `host_main.cpp` 780-line rewrite; null-mode + OSC 4/4 local; see log 2026-10-14 (1) |
| host: `heddle-host` `--gui` param-control window (t3inc2) | ✅ **CI GREEN** (run 38733502466, `de0c36c`): local 4/4 verified 2026-10-15 (core 57/57, smoke, OSC 4/4, `--gui` clean exit) + full CI green: `ChainGuiContent` + `HeddleHostWindow` (ResizableWindow) spliced into `host_main.cpp` (~1143 lines); CMake `+juce_graphics`/`juce_gui_basics` on heddle-host; **deadlock fix**: `guiStart()` before audio-thread start (GUI ctor takes `chainLock_` blocking once; audio thread try-locks per 512-sample block → post-startup GUI uses `GenericScopedTryLock` only) |
| host: CMake (JUCE 8.0.4 sha256-pinned, opt-in HEDDLE_BUILD_HOST) | ✅ pushed; FORMATS VST3 wrapper fix pending CI verification |
| host: CI (3-OS matrix + smoke param assertions) | ✅ **GREEN 6/6** (run 37984831984, `f6c9d22`): plugin loads, no crash, smoke assertions pass on Linux/macOS/Windows; `crash_diag` instrumentation removed |
| cli: `heddle-render` offline audio→audio (item 8) | ✅ **CI GREEN** (run 38021890348, `df334c1`): param-push + stride-invariant render + CMake + CI render job (WAV selftest + `DryMix=0.5` gain-ratio assert on all 3 OSes). First compile red 3/3 (bare `int64` + const `createWriterFor`) fixed in the same commit |
| prebuilt binaries / distribution | ✅ **CODE-ONLY policy** (decision 2026-10-15, Miha): all binary assets purged from GitHub (3 release assets from v0.1.5 + 15 workflow artifacts deleted 2026-10-15); `release.yml` removed; new manual-only `build.yml` (workflow_dispatch, 3-OS, 7-day artifact retention) + `weft-ci-artifact-fetch` skill (fetch locally → delete from GH); README re-written (no "quick start binaries"; License section documents JUCE AGPL/commercial + VST3 SDK constraint). Long term: code-only, or buy JUCE commercial licence if we ever distribute commercially |
| packaging (item 8f) | ✅ root LICENSE (MIT), `--version` + CMake version stamp 0.1.0, README render docs + JUCE AGPLv3/commercial license correction |
| docs (README/CONFIG/OSC/ARCHITECTURE) | ✅ re-synced to code 2026-10-11 (reply format, curve params, layout, target names) |
| logo + name (Heddle) | ✅ **2026-10-16: renamed Weft→Heddle** (Miha's call — resolves the Oiko Audio "Weft" VST3 collision) + logo v3 done: VLM-verified mark / mark_light / mark_transparent / banner / avatar_400 in assets/logo/ (v1 palette cyan 0,229,255 → violet 124,92,255, rounded heddle frame, 5 slots center-tallest, control segments at slot bases); delivered to chat via direct Bot API after 3/3 gateway `MEDIA:` sends were lost (sending-media skill, mode 3). Source rename complete — zero `weft` outside dated history; repo rename + v0.1.7 release pending |
| host: t5 bridge (`heddle-host --audio ring` + HeddleBridge VST3 + `heddle-bridge-test`) | ✅ written + local green (re-verified post-rename 2026-10-16 after clean reconfigure — stale CMakeCache pitfall, see log 2026-10-16 (3)): shared-memory SPSC ring pair (`heddle.bridge.in/.out`, heddle_core `AudioRing`) + OSC control on the headless host; `heddle-bridge-test` (heddle_core only, no JUCE) spawns the host, asserts Gain-0.5 round-trip (mean 0.25, both channels) + OSC `/chain/query` → `/reply/chain` + `/chain/quit` clean exit. Debugged: JUCE 6-arg `copyFrom` is single-channel (dropped stereo ch1) + `fail()` no-exit cascade masked the OSC check. CI job added (3-OS) — **CI pending** (next push) |

## Log

- **2026-10-16 (3)** — **Weft → Heddle: full source rename + logo v3 + v0.1.7 bump; local build fully green.**
  Naming decision (Miha): rename to **Heddle** — resolves the Oiko Audio "Weft"
  VST3 collision found 2026-10-16 (1). Executed: `git mv` of
  `core/include/weft/` → `core/include/heddle/` and `host/weft_bridge/` →
  `host/heddle_bridge/`; every source, test, doc, and workflow rewritten —
  namespace `weft::`→`heddle::`, targets `weft_*`/`weft-*` → `heddle_*`/
  `heddle-*` (heddle_core, heddle-host, heddle-render, heddle_smoke,
  heddle_smoke_plugin, heddle_bridge, heddle-bridge-test), ring names
  `weft.bridge.in/.out` → `heddle.bridge.in/.out`, CMake option
  `WEFT_BUILD_HOST` → `HEDDLE_BUILD_HOST`, plugin product
  `WeftSmokePlugin` → `HeddleSmokePlugin`, window `WeftHostWindow` →
  `HeddleHostWindow`. `git grep -il weft` outside this file's dated history =
  empty. **Pitfall (cost one red build):** `git mv` leaves a stale CMakeCache
  still pointing at `host/weft_bridge/` — `rm -rf build-msvc` + full
  reconfigure is mandatory after a rename (recorded in weft-project-ops).
  Post-rename local verification (MSVC, clean reconfigure): all targets build;
  core **57/57 tests, 1689 assertions**; smoke VST3 load OK (Gain + PWM pct);
  bridge round-trip **PASS** (ratio=0.5000, ch0=ch1=0.250000, OSC OK).
  Logo v3 regenerated + VLM-verified (qwen3-vl): mark, mark_light,
  mark_transparent, banner, avatar_400 in assets/logo/ (old weft_*.png
  removed). Chat delivery: 3/3 gateway `MEDIA:` sends lost to stream
  interruption (sending-media-to-chat skill, mode 3) → delivered via direct
  Telegram Bot API (`send_tg_direct.py`, file_ids captured, 5/5).
  Version: CMake `project(heddle_core VERSION 0.1.7)` (single-sourced into
  `heddle_version.hpp.in` via `@CMAKE_PROJECT_VERSION@`). Remaining: commit +
  push, CI, GitHub repo rename (weft → heddle), v0.1.7 release.
- **2026-10-16 (2)** — **t5 bridge: headless shared-memory ring IPC + WeftBridge VST3 + end-to-end test; local green, CI pending.**
  The live-host↔plugin IPC bridge lands: `weft-host --audio ring` runs the
  chain headless (no audio device) with audio crossing process boundaries
  over a shared-memory SPSC ring pair (`weft.bridge.in` / `weft.bridge.out`,
  `weft::AudioRing` from weft_core, 16384-frame capacity, 1024-sample blocks,
  48 kHz stereo) and OSC/UDP as the control surface (port auto-allocated,
  printed on ready). `host/weft_bridge/` adds the WeftBridge VST3 — a thin
  plugin whose DSP is dry passthrough; it exists so a DAW-side plugin and
  the host can share the ring (status editor shows ring state). `weft-bridge-test`
  (`host/src/bridge_test.cpp`, links **only weft_core** — no JUCE, so it runs
  on a bare headless runner) is the end-to-end check: spawns
  `weft-host --audio ring --osc-port <auto>`, waits for the ready line,
  writes a 0.5 constant (both channels, 8192 frames = 8 full blocks) into
  `weft.bridge.in`, reads `weft.bridge.out`, and asserts the round-trip mean
  is 0.25 with the smoke plugin at Gain=0.5, "PWM pct"=1.0 (gate wide open:
  0.5 × 0.5 × 1.0). Then drives OSC: `/chain/query` → `/reply/chain` (must
  list the "smoke" slot as loaded) and `/chain/quit` → `/reply/status
  "quitting"` + host exit 0.
  **Two real bugs found via instrumentation, both fixed:**
  (1) *Stereo ch1 silently zeroed* — `runBlockLocked` ended with
  `workOut_.copyFrom(0, 0, *cur, 0, 0, frames)`. JUCE's 6-arg `copyFrom`
  (`destChannel, destStart, source, sourceChannel, sourceStart, numSamples`)
  is **single-channel** — there is no buffer-to-buffer multi-channel
  overload — so only channel 0 was copied and `workOut_` ch1 stayed cleared.
  Symptom: per-channel means 0.25/0.0, histogram `0.25=8192 0.0=8192`
  (exactly half the samples at the right value, the rest zero). Fixed with a
  per-channel loop. (2) *`fail()` no-exit cascade* — the test's `fail()`
  printed + killed the host but did not exit, so a gain failure cascaded
  into the OSC section against a dead host and misreported an OSC failure
  (the host OSC path itself was verified working by an independent Python
  probe with the 4-byte length prefix). `fail()` now `std::exit(1)`s after
  cleanup. Local (MSVC, VS 2022): build clean, `bridge-test: PASS
  (ring round-trip ratio=0.5000, OSC query+quit OK)`, exit 0. CI:
  `ci.yml` now also builds `weft-host` + `weft-bridge-test` +
  `weft_bridge_plugin_VST3` (compile check), and the host job gained a
  **Bridge round-trip** step that runs `weft-bridge-test` against the smoke
  VST3 on all 3 OSes; the plugin-locate step now targets the smoke bundle by
  path prefix (two VST3s are built).
- **2026-10-16 (1)** — **Logo VLM inspection + name-collision finding (no code changed).**
  Per Miha's note ("use a vision model for logo inspection"), ran all three
  assets in `assets/logo/` through the live VLM
  (`qwen3-vl-30b-a3b-instruct`, gpustack) instead of heuristic pixel checks:
  (1) *mark* — 4-overlapping-circles concept is right for a chain host but
  fails at favicon scale: inner dots vanish, cyan→purple gradient muddies to
  one flat color, overlap reads as a blob; (2) *banner* — "WEFT" legible but
  tagline "audio chain host" half-hidden behind the W and the circles
  (hierarchy inverted); (3) *avatar* — thin pastel strokes on white melt into
  a fuzzy oval below ~120px. Common fix thread: thicker strokes,
  solid/high-contrast color (drop soft gradient), fewer elements, vector
  master + simplified favicon variant. Then did the "ask Google" leg:
  **name collision is the bigger issue** — Oiko Audio "Weft" is an active
  open-source **VST3/CLAP** spectral filter (oikoaudio.com, own GitHub, top
  hit for "weft audio plugin VST3"): same name *and* same plugin format.
  Additionally 870 GitHub repos named "weft" (AI language 2k★, task manager
  516★, video editor 226★ …). Trademark risk low while both OSS, but Oiko
  commercializing "Weft" later would be an avoidable fight. Decision pending
  (Miha): keep name (accept collision) vs rename (weaving metaphor still fits
  a chain host, so rename candidates should keep that soul). Logo rework is
  held until the naming call — regenerating now would be wasted work if the
  name changes.

- **2026-10-15 (2)** — **CI macOS lane pinned to `macos-15` (GitHub runner-pool flake, not a code bug).**
  `macos-latest` now resolves to `macos-26-arm64` (image shipped 2026-10-13).
  Its public pool cancelled the trivial `core (macos)` job 3× in a row at
  exactly 15:00 with **no log blob** (`BlobNotFound` — runner died before
  producing output), while the identical build passes in <60 s on
  Windows/Ubuntu and `host (macos)` on the same image succeeded (~2 min).
  Fix `94603c9`: pin both `ci.yml` matrices to `macos-15` (stable GA arm64
  label; `macos-14` was retired 2026-10-01). Result: run 38767811066
  **GREEN 6/6** — `core (macos-15)` passed in 22 s. `build.yml`
  (manual-dispatch) intentionally left on `macos-latest`; re-run there if it
  ever flakes. v0.1.6 release unaffected (tag points at `2923c31`).

- **2026-10-15** — **Code-only distribution policy + t3inc2 GUI verified.**
  (a) *Policy change (Miha's decision)*: no binaries on GitHub. JUCE 8.0.4 is
  dual-licensed AGPL-3.0/commercial and the VST3 SDK is Steinberg
  proprietary/GPLv3 — legal public binary distribution needs a commercial JUCE
  licence or an AGPL relicense, neither of which we had. Executed purge:
  3 `weft-render` assets on the v0.1.5 release deleted (v0.1.0–v0.1.4 had
  none), all 15 non-expired workflow artifacts deleted, `release.yml`
  removed. New `.github/workflows/build.yml` is `workflow_dispatch`-only
  (3-OS matrix, 7-day artifact retention, nothing published to releases);
  companion `weft-ci-artifact-fetch` skill documents build-on-CI → download
  locally → delete-from-GH. `README.md`: "Quick start (no compilation)"
  replaced by "Getting a build" (source-only + `scripts/build-all.ps1`),
  License section rewritten (code-only policy, both license constraints,
  long-term plan = buy JUCE commercial licence if we ever distribute
  commercially; MIT stays). **v0.1.6 tagged + published** (2026-10-15,
  0 assets by design, full changelog in release body). One-shot local build script
  `scripts/build-all.ps1` (core MSYS2 ucrt64 + host MSVC VS2022 + ctest +
  smoke + render gain-ratio self-test + 3s headless weft-host run).
  (b) *t3inc2 `--gui`*: `ChainGuiContent` (live param sliders + slot list) +
  `WeftHostWindow` (juce::ResizableWindow, auto-close → `stopAll()`) spliced
  into `host_main.cpp`; `weft-host` now links `juce_graphics` +
  `juce_gui_basics`. Debugged a **hang** on `--gui`: the window ctor took
  `chainLock_` with a blocking `ScopedLock` while the audio thread
  try-locked the same lock every 512-sample block — one-shot blocking
  acquisition starved → fix: call `guiStart()` **before** the audio thread
  starts; post-startup GUI uses non-blocking `GenericScopedTryLock` only.
  Local verification on the probe-free binary: core 57/57, smoke (Gain/PWM
  pct/Bypass), OSC 4/4, `--gui` 6 s run clean exit 0 (window opens, timer
  polls chain, clean shutdown).

- **2026-10-14 (1)** — **t3inc1: `weft-host` standalone live chain host (headless-ready, OSC-driven).**
  Full clean rewrite of `host/src/host_main.cpp` (780 lines): the live chain
  host loads a config, instantiates one plugin instance per slot, and runs
  either with a real audio device or headless (null audio, software clock)
  with OSC as the control surface — `/chain/query` → `/reply/chain`, set by
  name (float) or by id (int+float) → `/reply/<slot>/<param>`,
  `/chain/<slot>/reset` → `/status`, `/chain/quit` → exit, `/change/<slot>/
  <param>` push on value change. JUCE 8 device control is
  `addAudioCallback`/`removeAudioCallback` (no `start()`/`isDeviceRunning()`);
  `JUCE_MODAL_LOOPS_PERMITTED=1` on the weft-host target. Verified local:
  MSVC build OK, null-mode OSC check 4/4. Committed `0fdb134`, pushed; CI
  run 38525679263 **completed success** (all 3 OSes).

- **2026-10-14 (2)** — **testvst: smoke plugin DSP rewritten to Gain + PWM duty gate; CI render asserts rewritten to match.**
  (a) *plugin* (`host/test_plugin/plugin.cpp`): the old DryMix/Mode smoke is
  replaced by two continuous 0..1 params, both default 1.0 — `Gain` (linear
  output gain) and `PWM pct` (duty cycle of a fixed-period 1024-sample on/off
  gate; `out[n] = in[n] * Gain * gate[n]`). The gate phase is a member that
  survives across `processBlock` calls (reset in `prepareToPlay`), so the
  waveform is continuous across the host's 512-frame blocks and across
  renderer blocks; the renderer calls `setProcessSpec` (→ `prepareToPlay`)
  once before its block loop, so a render starts at phase 0. Params are read
  live in `processBlock` (a cached copy would ignore config values pushed
  after `prepareToPlay`). For constant-amplitude input, mean-out/mean-in =
  Gain × duty, so the render job asserts that product and proves BOTH config
  params reach the live instance (defaults are 1.0/1.0; ignoring either would
  change the ratio).
  (b) *CI* (`ci.yml`): smoke assertions now require `Gain` + `PWM pct`
  present, both stepCount 0, default 1.0 (JUCE-injected Bypass still
  tolerated). Render static case: input constant 0.5, config Gain=0.5 +
  "PWM pct"=0.625 → expected ratio 0.3125 (2048-frame file = exactly two
  1024-sample gate periods; 640 of 1024 ON each). Render curve case:
  `Gain` ramps 0.5→1.0 over 2048 frames (block-start values 0.5/0.625/
  0.75/0.875 → mean 0.6875) with "PWM pct"=1.0 (gate transparent, 1024/1024
  ON) → expected ratio 0.6875.
  (c) *Pitfall (compile red 1/1 locally)*: JUCE 8.0.4 `AudioParameterFloat`
  has NO 1-arg display-lambda constructor — the overloads take either a
  2-arg `stringFromValue` (`String(float,int)`), an
  `AudioParameterFloatAttributes` object, or no 5th arg. The attempted
  `[](float v){...}` lambda matched none → C2661. Also: a `parameterLabel`
  is appended to the name the host sees via `getName()`, which would break
  the host's exact-title OSC match on `PWM pct` — so the param has no label
  and the title itself carries the unit.
  (d) *Local verification (all green before push)*: full MSVC build OK;
  core 57/57; smoke JSON shows both params stepCount 0 def 1.0; render
  static ratio 0.312481≈0.3125, curve 0.687481≈0.6875 (16-bit quantization
  <1e-4); headless weft-host + OSC 4/4 (query, set-by-name Gain, set-by-id
  "PWM pct" — the space round-trips because the core OSC tokenizer splits on
  '/' only —, reset). Product name `WeftSmokePlugin` + target
  `weft_smoke_plugin` kept unchanged so every `find` pattern in CI still
  resolves.
  (e) *Backfill*: `t3inc1` (`0fdb134`, `weft-host` headless + OSC rewrite,
  780-line `host_main.cpp`) was committed + pushed 2026-10-14 and CI run
  38525679263 went green, but its STATUS.md row/entry was never written —
  recorded in the table now.

- **2026-10-11 (1)** — **Param time-series (static|curve), render curve automation, OSC reply routing; docs re-synced.**
  (a) *core time-series* (`054302f`): any config param may now be a static
  number or `{"curve": [[frame:int≥0, value], ...]}` — frames are absolute
  sample positions, sorted ascending, ≥1 keyframe, values clamped [0,1],
  linear interp with end-clamping, frame 0 = load-time value. `ParamSpec`
  (curve.hpp) wraps both; config parse/dump/merge handle it; `test_curve.cpp`
  covers eval + validation.
  (b) *weft-render curves* (`3d3c9fd`): the renderer evaluates per-slot
  params at every block over the offline timeline, so curves automate params
  during a render; static params unchanged.
  (c) *OSC reply routing* (`feea2d2`): `OscUdp::poll(msg, &src)` captures
  the sender (ip+port) of the last packet; `sendTo(msg, src)` replies to an
  explicit address — a host can answer whichever client asked. Loopback test
  in `test_osc_udp.cpp`.
  (d) *docs re-sync*: test counts 39/39 + 253/253 everywhere; README plugin
  target fixed to `weft_smoke_plugin_VST3`, stale `cli/` row removed
  (renderer lives in `host/`), layout now lists `curve.hpp` + `host/`;
  OSC.md reply format corrected (7 fixed tab columns + one per `listItems`
  label — no `units` column); CONFIG.md documents the curve form;
  ARCHITECTURE.md layering/roadmap/test list match the code.
  Local: 39/39 test cases, 253/253 assertions (MSYS2 g++ 16.2). All three
  code commits **unpushed — CI pending**.
  (e) *2026-10-12*: pushed (`530c94e` + 3 code commits). First CI run
  (38209447580) failed 3/3 host jobs on a CI-script bug from `3d3c9fd`:
  the curve-case `cfg2` dict in the render step's Python heredoc was missing
  its closing brace — SyntaxError before `weft-render` ran (core green).
  Fixed in `2ffe9b8` (one line; all 3 heredocs re-verified to compile,
  YAML validated). Re-run **38220293171 GREEN 6/6**.

- **2026-10-10 (2)** — **Prebuilt binaries: `release.yml` + first releases.**
  README now leads with "Quick start (no compilation)". Release workflow:
  3-OS build matrix (mirrors ci.yml host job) + packaging + one GitHub
  Release per `v*` tag. Bumpy first cut — every failure was at
  *packaging/publish*, all 3 OS builds were green each time:
  v0.1.0 tag run: binary at `build/host/Release/` not `build/Release/`
  (MSVC multi-config) → fix `f966f84`; dispatch re-run:
  `action-gh-release` requires a tag (dispatch has none) → fresh tags
  only (force-push banned); v0.1.1: `tar -a -cf x.zip` wrote a raw TAR
  with a .zip name (verified by downloading: starts with the entry name,
  ends with zero blocks) → pwsh `Compress-Archive`; v0.1.2: no `zip(1)`
  in the runner's Git-Bash (exit 127) → split package step pwsh/bash;
  v0.1.3: `Copy-Item A B dest` bash habit → `Copy-Item A, B -Destination`.
  v0.1.4 = first clean candidate. Asset verification = download +
  python `zipfile`/`tarfile` open (see weft-project-ops skill).

- **2026-10-10 (1)** — **Item 8: `weft-render` offline CLI + packaging (8f) built;
  first CI compile red, fixed, re-pushed.** Work of the day:
  (a) *param-push fix* (8a): `Chain::load` → `JuceBackend::setParams` →
  `setValueNotifyingHost`, so per-slot config `params` actually reach the live
  instances; test_plugin reads `DryMix` live; 27/27 test cases, 144/144
  assertions local (MSYS2 g++ 16.2).
  (b) *weft-render* (8b): stride-invariant offline renderer — reads a WAV at
  its own rate (no resampling, mono→stereo upmix), processes full
  block-size blocks through `JuceBackend::process` at the file's true rate
  (`setProcessSpec`), writes 16/24/32-bit WAV via
  `WavAudioFormat::createWriterFor` (6-arg, writer owns the stream). CMake
  target `weft-render` + `--version` (CMake `configure_file` version stamp,
  root `project VERSION 0.1.0`).
  (c) *CI* (8c): render job — Python WAV selftest (gen+parse+ratio assert),
  then render with the smoke plugin at `DryMix=0.5` and assert
  out/in mean ratio ≈ 0.5 on all 3 OSes.
  Pushed `495e331` → run 38019353657 **RED 3/3 host**, core green. Two shared
  root causes (both compile, all OSes): (1) bare `int64` — not a global type;
  MSVC C4430/C2146, clang `unknown type name 'int64'` (JUCE 8 defines
  `juce::int64`, GNU compilers expose bare `int64` — local core build never
  hit it). Fixed → `int64_t` + `#include <cstdint>` (3 sites).
  (2) `createWriterFor` is **non-const** (juce_WavAudioFormat.h:300/307) but
  was called on `const juce::WavAudioFormat wav` → fixed by dropping `const`.
  (d) *8f packaging*: root MIT `LICENSE`, README render/build docs + status
  table, JUCE license corrected to **AGPL-3.0 / commercial** (was wrongly
  GPL-3.0; JUCE 8.0.4 is dual-licensed, fetched via FetchContent, not
  vendored). Fix commit `df334c1` → **run 38021890348 GREEN 3/3** (Linux/
  macOS/Windows): weft-render compiles and the CI render job passes — WAV
  selftest + `DryMix=0.5` gain-ratio assertion confirmed the config param
  actually reaches the live plugin through the full host chain. **Item 8
  (offline audio→audio CLI + packaging) COMPLETE.**
- **2026-10-09 (14)** — **CI GREEN 6/6** (run 37984831984, `f6c9d22`): host +
  core all success on Linux/macOS/Windows. The VST3 host chain is verified
  end-to-end: plugin build → bundle packaging → headless load → param
  enumeration → JSON dump → assertions, all three OSes. Follow-up: removed
  the temporary `crash_diag.hpp` instrumentation (file + all
  `weft_smoke_diag::` call sites in `JuceBackend.cpp`/`smoke.cpp`) — it did
  its job (the step markers + macOS backtrace are how the teardown UAF was
  found). Host layer DONE; next is the offline audio→audio CLI (item 8).
- **2026-10-09 (13)** — **Smoke assertions fixed (test-only, 2nd round).** Run
  37983104167 (`4caad2d`) proved the superset name-check passed on all 3
  OSes, then tripped on two float/format expectations, both test bugs:
  (a) DryMix `defaultValue` round-trips through JSON as
  `0.800000011920929` — VST3 params are float32, so `== 0.8` fails in
  Python float64; now compared with tolerance 1e-5. (b) Mode `listItems`
  is `[]`, not `["Soft","Hard","Off"]`: our backend loads the plugin via
  the **VST3 format**, which returns JUCE `VST3Parameter` wrappers whose
  `getAllValueStrings()` returns `{}` (verified in
  `juce_VST3PluginFormat.cpp:2574`); raw VST3 `ParameterInfo` carries no
  value strings at all. So a choice param hosted through VST3 always
  reports empty labels — asserted `[]` and the discrete `stepCount == 3`.
  Also corrected the misleading comment in `JuceBackend.cpp`. No
  host/plugin behavior changed.
- **2026-10-09 (12)** — **SEGFAULT RESOLVED** (run 37976368540, from `7389a84`,
  the double-ownership fix). All 3 host jobs now get PAST `findAllTypesForFile`:
  plugin loads, no signal, valid params JSON printed on Linux/macOS/Windows.
  Remaining failure (all 3 OSes, identical) is the smoke assertion, not a
  crash: `AssertionError: {'DryMix', 'Mode', 'Bypass'}`. Root cause: the
  **plugin-side** JUCE VST3 wrapper auto-injects a standard "Bypass"
  `AudioParameterBool` (id `'byps'` = 0x62797073) whenever the processor
  provides none — VST3 spec requires a bypass export
  (`juce_audio_plugin_client_VST3.cpp:600-612`). The smoke assertion was
  hardcoded to `names == {"DryMix","Mode"}` (written before the wrapper's
  injection behavior was observed). **Fixed:** assertion now checks
  `{"DryMix","Mode"} <= names` (superset) plus the per-param value checks;
  injected extras tolerated. No host/plugin code change.
- **2026-10-09 (11)** — **ROOT CAUSE OF THE VST3-LOAD SEGFAULT CONFIRMED + FIXED.**
  CI run 37943716551 (from `d1f444b`, after dropping `juce_audio_plugin_client`
  from the host) compiled clean 47/47 and STILL segfaulted (exit 139) on all 3
  OSes at the same step marker (`format matched, enumerating types in bundle`).
  The macOS job's backtrace came back **fully symbolicated** and is decisive —
  the crash is NOT at instantiate (the long-held theory) but at **teardown of
  the host's throwaway instance** during enumeration:
  ```
  weft::JuceBackend::loadSlot
  → juce::VST3PluginFormat::findAllTypesForFile
  → juce::DescriptionLister::findDescriptionsSlow   (host creates temp IComponent)
  → [plugin] JuceVST3Component::release() + 40      (VSTComSmartPtr → refcount 0)
  → [plugin] JuceVST3Component::~JuceVST3Component() + 192
  → [plugin] JuceAudioProcessor::~JuceAudioProcessor() + 140
  → [plugin] weft_smoke::SmokeProcessor::~SmokeProcessor() + 84
  → [plugin] juce::AudioProcessor::~AudioProcessor() + 60
  → [plugin] juce::AudioProcessorParameterGroup::~AudioProcessorParameterGroup() + 100  ← SIGSEGV
  ```
  **Mechanism (source-verified in JUCE 8.0.4, juce_AudioProcessor.cpp):**
  `AudioProcessor::addParameter(AudioProcessorParameter*)` wraps the raw
  pointer in a `unique_ptr` and adds it to the processor's `parameterTree` —
  the processor TAKES OWNERSHIP and deletes it in `~AudioProcessor` (header
  doc: "managed and deleted automatically by the AudioProcessor"). Our
  `SmokeProcessor` ctor also held the same two params in
  `std::unique_ptr<AudioParameterFloat/Choice>` members and passed
  `.get()` to `addParameter` → **double ownership**. On destruction the
  derived `~SmokeProcessor` frees both params first, then the base
  `~AudioProcessor` destroys `parameterTree` → `~AudioProcessorParameterGroup`
  deletes the already-freed params → use-after-free → SIGSEGV. Identical on
  all 3 OSes (pure C++ lifetime bug, no platform involvement).
  **Fix:** params are now plain `new` + raw-pointer members (never deleted by
  us; owned by the processor), matching JUCE's canonical examples
  (`examples/Plugins/GainPluginDemo.h`: `addParameter (gain = new ...)`).
  The earlier `juce_audio_plugin_client` drop (`93dac4a`) was still correct
  (host must not link the plugin-side module) but was a red herring for THIS
  crash. `crash_diag.hpp` instrumentation (step markers + backtrace) did its
  job and can be dropped once the smoke is green 3/3.
  **NOTE for future hosts:** any JUCE `AudioProcessor` subclass must never
  smart-pointer-own params added via `addParameter`/`addParameterGroup`.
- **2026-10-09 (10)** — CI runs 37853305618 / 37853454652 (from `1b22ca3`):
  the `JUCE_PLUGINHOST_VST3=1` fix **worked** — the old `no plugin format can
  load` error is gone, build 47/47, smoke binary linked, the VST3 format
  manager now *finds* the plugin. But the failure advanced one more stage
  deeper, to a **segfault (exit 139)** inside the VST3 load/instantiate, and it
  reproduces on **all 3 OSes** (Ubuntu 113571887354, macOS 113571887593,
  Windows 113571887206) → a shared code path, not platform-specific.
  **Root-cause hunt (JUCE 8.0.4 source, read end-to-end):** every host-side init
  layer is fully null-defensive (`VST3ModuleHandle::create`,
  `VST3ComponentHolder::initialise`, `VST3PluginInstance::initialise`,
  `prepareToPlay`, `findDescriptionsSlow`, `RefCountedDllHandle::getHandle`);
  the test plugin is headless (no GUI/editor); `JUCE_VST3_HOST_CROSS_PLATFORM_UID`
  is a red herring (referenced only in the example CMake, never in host code).
  The one structural difference between `weft_smoke` and JUCE's own working
  `AudioPluginHost` is that we **link `juce::juce_audio_plugin_client`** — the
  *plugin-side* module. Linking it compiles a second copy of the VST3 SDK base
  into the host executable, which conflicts (symbol interposition / ODR on the
  shared `FUnknown`/`IPluginFactory` base) with the SDK already compiled into
  the loaded `.vst3` plugin and segfaults at instantiate. Fix: drop that module
  (the host uses no client API) AND add diagnostic instrumentation
  (`host/src/crash_diag.hpp`): step markers around each JUCE load stage + a
  SIGSEGV/SIGABRT/SIGFPE backtrace handler, so if it still crashes the next CI
  log names the exact function. Re-run pending.
- **2026-10-09 (9)** — CI run 37845591328 (from `00769c9`): `00769c9` **worked**
  — the old `plugin file not found` error is gone, the format-manager slow path
  now runs, and all 3 OSes still build/link/package cleanly (build 47/47, smoke
  binary linked, .vst3 bundle present). Failure advanced one stage deeper, to
  the format manager itself, which printed
  `error: no plugin format can load: …/WeftSmokePlugin.vst3`.
  **Root cause confirmed from the Linux CI log (job 113545530340):** the
  `weft_smoke` compile flags carried `-DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0`
  but **no** `-DJUCE_PLUGINHOST_VST3=1`. The `AudioPluginFormatManager`
  registers `VST3PluginFormat` only when that per-target define is set, so the
  smoke host had zero formats and load() failed. Fix (`1b22ca3`): add
  `JUCE_PLUGINHOST_VST3=1` to `weft_smoke`'s `target_compile_definitions`,
  exactly matching JUCE's own AudioPluginHost. Link-safety verified against the
  JUCE 8.0.4 tree: `_juce_module_sources` compiles only the top-level module TU
  (`juce_audio_processors.cpp`), which `#include`s
  `format_types/juce_VST3PluginFormat.cpp` (line 216); that TU pulls in the VST3
  SDK base sources transitively under `JUCE_PLUGINHOST_VST3`. JUCE's shipping
  `AudioPluginHost` example links with only this define + the module (no
  explicit SDK base sources), proving the module system handles the SDK
  transparently — so `weft_smoke`, which links the same `juce_audio_processors`
  module, links identically. Re-run triggered as `37853305618`.
- **2026-10-08 (8)** — CI run 37810148573 (from `a7d2748`): the two packaging
  fixes from (7) **both worked** — all 3 OSes now compile, link, and package
  the complete VST3 bundle (build reached 47/47; the `moduleinfo.json`-removal
  step ran; the CI shell confirmed the bundle *directory* exists on disk).
  Failure advanced to the smoke test, which printed
  `error: plugin file not found: …/WeftSmokePlugin.vst3` — despite the bundle
  dir being present. Root cause = a bug in my smoke host, not the packaging:
  `JuceBackend::loadSlot` gated on `file.existsAsFile()`, but a VST3 plugin is
  a bundle *directory* (`WeftSmokePlugin.vst3/Contents/…`), so `existsAsFile()`
  is false and the load bailed before reaching the format manager. Fix
  (`00769c9`): accept file **or** directory via `file.exists()`, then let the
  format manager decide. Verified against JUCE 8.0.4 source that the whole
  directory-bundle chain works: `fileMightContainThisPluginType`
  (juce_VST3PluginFormat.cpp:4217) = `hasFileExtension(".vst3") && f.exists()`
  → true for a dir; `findAllTypesForFile` (4095) fast-path empty (no
  moduleinfo.json) → `findDescriptionsSlow` (4128); `createVST3Instance`
  (4153) same gate → true. CI-fix cycle 5.

- **2026-10-08 (7)** — CI run 37780116943 (from `3a1745c`): both prior C++
  errors (String ctor, private getValue) are **fixed** — all 3 OSes now compile
  and the VST3 **links**. Failures moved to the JUCE packaging stage, 2 new
  independent errors, both source-verified against JUCE 8.0.4:
  - Linux (compile, `juce_core.cpp` compiled *into* `weft_smoke_plugin`):
    `fatal error: curl/curl.h`. `JUCE_USE_CURL` defaults to 1 via a `#ifndef`
    guard (juce_core.h:151) and the runner lacks libcurl dev headers. We use
    no networking; curl *linking* is separately opt-in (`NEEDS_CURL`, default
    off) so there is nothing to unlink. Fix: `target_compile_definitions(...
    JUCE_USE_CURL=0)` on both `weft_smoke_plugin` and `weft_smoke` (module
    sources compile into each target with that target's own defs).
  - Windows + macOS (post-build): `juce_vst3_helper -create` (writes
    moduleinfo.json) crashes — Windows 0xC0000005 (MSB3073), macOS SIGSEGV
    139. No matching upstream issue found. 8.0.15 rewrote the helper
    (shared → per-plugin, inherits plugin compile defs) but also split
    `juce_audio_processors` into `_headless` — reorg risk too high; staying on
    the 8.0.4 pin. JUCE's own sanctioned workaround: `VST3_AUTO_MANIFEST` is a
    documented `juce_add_plugin()` keyword (one_value arg, property default
    TRUE, gates the helper invocation) → set FALSE. The smoke host is
    unaffected: `VST3PluginFormat::findAllTypesForFile` falls back to slow-path
    factory enumeration when moduleinfo.json is absent
    (`getLibraryPaths` → `getPluginFactory` → `findDescriptionsSlow`, verified
    at juce_VST3PluginFormat.cpp:4095-4131) — instance creation
    (`createVST3Instance` → `VST3ModuleHandle::create`) never reads it either.
  Pushed; next CI run is the source of truth. CI-fix cycle 4.

- **2026-10-08 (6)** — CI run 37761767553 (from `67bfd8d`): core green 3/3,
  host red 3/3 — but down to just **2 distinct errors**, both source-verified
  against JUCE 8.0.4:
  - JuceBackend.cpp:99 (Windows C2228 + C2660 cascade): `juce::String(path)`
    with `path` a `std::string`. JUCE 8 `String` has **no `std::string` ctor**
    (only `const char*`/`const wchar_t*`/char8_t), so the `File` ctor failed
    and `file` became an error-type — which is why MSVC then misreported
    `.existsAsFile` (C2228) and `findAllTypesForFile` as "1 argument" (C2660);
    the call site itself was correct (2-arg). Fix: `juce::String(path.c_str())`.
    (Linux/macOS never compiled JuceBackend.cpp because plugin.cpp failed first,
    so only MSVC surfaced this.)
  - plugin.cpp:33 (Linux + macOS): `AudioParameterFloat::getValue()` is
    **private** in JUCE 8; the public accessors are `get()` / `operator float()`.
    Fix: `dryMixParameter->get()`. (Note: `p->getValue()`/`p->getDefaultValue()`
    at JuceBackend.cpp:35-36 are the *public* virtuals on the
    `AudioProcessorParameter*` base pointer and are unaffected.)
  Pushed; next CI run is the source of truth. CI-fix cycle 3/3.

- **2026-10-08 (5)** — CI run 37727941884 (from `1d0849d`): host still red 3/3,
  but now at the C++ layer. Parsed the exact compiler errors from all three job
  logs and fixed each against the JUCE 8.0.4 source:
  - plugin.cpp (Linux/macOS, abstract-class + 4 errors): added the 6 missing
    pure-virtual overrides (getName, getTailLengthSeconds, acceptsMidi,
    producesMidi, getStateInformation, setStateInformation); removed the
    `getLatencySamples` override (not virtual in JUCE 8); getProgramName now
    returns `const String`; prepareToPlay reads getValue() instead of the
    private AudioParameterFloat::getDefaultValue(); processBlock uses the
    1-arg all-channels applyGain(gain) (2-arg overload doesn't exist).
  - JuceBackend.cpp (Windows, 6 errors): String::toInteger64 → new
    paramIdFromJuce() helper (getLargeIntValue, decimal VST3 ids);
    RangedAudioParameter getMin/MaxValue → getNormalisableRange().start/.end;
    File::fromString → File(String) ctor; fileMightContainThisPluginType /
    findAllTypesForFile take a String → pass getFullPathName();
    createPluginInstance(desc,…) — descs[0] is a pointer, dereferenced to a
    `const PluginDescription&`.
  - smoke.cpp: initialiseJuce_GUI is declared in juce_events — added the
    include (it was a compile error, not a link error; juce_events was already
    linked via juce_audio_processors).
  - CMake (Linux/macOS gtk/gtk.h + macOS WebKit): JUCE_WEB_BROWSER defaults to
    1 and the juce_gui_extra module .cpp files compile INTO our targets, so
    `JUCE_WEB_BROWSER=0` is set on both weft_smoke and weft_smoke_plugin
    (exactly how JUCE's own AudioPluginHost does it). No JUCE_VIDEO setting
    exists in 8.0.4 — web browser is the only gate for the GTK/WebKit blocks.
  - Note: `juce::AudioBuffer<float>` is still the type name in 8.0.4
    (AudioSampleBuffer is a newer internal rename; the Linux error itself named
    juce::AudioBuffer<float> and only complained about the applyGain call).
  Pushed; next CI run is the source of truth. CI-fix cycle 2/3.

- **2026-10-08 (4)** — CI run 37726704308: core green 3/3, host red 3/3.
  Two independent root causes (both source-verified against JUCE 8.0.4 +
  runner image readme):
  (1) Linux+macOS: `ninja: unknown target 'weft_smoke_plugin_VST3'` —
  `juce_add_plugin` parses formats via the NAMED `FORMATS` multi-arg
  (JUCEUtils.cmake cmake_parse_arguments); our positional `VST3` was
  dropped, so JUCE_FORMATS was empty and no wrapper target was ever
  created. Fix: `FORMATS VST3` in host/test_plugin/CMakeLists.txt.
  (2) Windows: runner image is windows-2025-vs2026 (VS Enterprise 2026,
  CMake 4.4.3, NO VS 2022) → generator "Visual Studio 17 2022" found no
  instance. Fix: generator "Visual Studio 18 2026" in ci.yml (CMake ≥4.2).
  Local core sanity re-green (26/26). Pushed; new run is the source of truth.
  CI-fix cycle 1/3.

- **2026-10-08 (3)** — CI run 37721075424: core green 3/3, host red 3/3.
  Diagnosed from full job logs (all 3 = CI environment issues, not code):
  (1) Windows picked MinGW; JUCE 8.0.4 removed MinGW support → Windows host
  job now uses Visual Studio 17 2022 / x64. (2) Linux: juceaide needs
  freetype/fontconfig/X11 headers → added 13 dev packages to apt deps.
  (3) macOS: root project() declared CXX only, JUCE's cmake needs C enabled
  (CMAKE_C_COMPILE_OBJECT) → project now LANGUAGES C CXX. Local sanity
  build re-green (26/26). Pushed; new CI run is the source of truth.

- **2026-10-08 (2)** — JUCE host layer written by assistant (not subagent — two
  research-only subagents had burned ~38 min on JUCE API investigation). All
  CMake/API facts verified directly against JUCE 8.0.4 source: juce_add_plugin
  topology (STATIC shared code + VST3 wrapper MODULE; shared code auto-links
  only juce_audio_plugin_client → modules linked explicitly),
  juce_generate_juce_header ordering (after juce_add_plugin), BusesProperties
  via constructor (no overridable method in JUCE 8), getParameters() returns
  const Array<AudioProcessorParameter*>&, getAllValueStrings() is const.
  Local sanity build green (core 26/26, host OFF). Pushed in 3 commits
  (3460a13, 0081059, 8f9305e); CI run 37721075424 building the first VST3
  on 3 OSes. Bounded CI-fix subagent to follow if red.
- **2026-10-08** — Project scaffolded. Core C++17 library written
  (param model, Chain + IPluginBackend, JSON config parse/dump/merge, OSC 1.0
  wire codec + `/chain` addressing, portable UDP transport). 26 targeted test
  cases / 142 assertions all passing locally. Docs + example config + CI
  workflow written. First public GitHub push in progress.
  - Known local quirk: a stale multi-config CMake cache from an early failed
    configure lived in `build/` for a while; clean reconfigure fixed it.
    (Superseded: build dir is regenerated in CI.)

## Conventions for agents working on this repo

1. **This file is the source of truth** for progress. Update the "Current
   state" table and append a dated log entry at the end of every session.
2. Commit every substantial change with a clear message; no giant squashed
   commits.
3. Tests must stay targeted and meaningful — do NOT add filler tests to look
   productive. Keep `ctest` green before committing test-affecting changes.
4. Core stays platform-agnostic C++17, MIT, no plugin SDK dependencies.
   Plugin SDK work happens in `host/` (JUCE) only.
5. Never commit secrets. Never push to anything but this repo's `main`.
6. If blocked, record it in the table with the reason — do not silently stop.
