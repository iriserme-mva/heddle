# HeddleProxy drag-crash: trial log & current theory

Living document (task 00001). Update after every build round: one row in the
trials table + append evidence to the relevant section. Goal: pin the root
cause of the proxy-editor crash on row drag, using field evidence, not theory.

## Status (2026-11-26)

**0.1.14c field test (26 Nov 14:26–14:28, pid 38780):** build confirmed live
(`CRASH-DUMP-ARMED (SEH vector handler)` + `bld=0.1.14c` banner). One
discovered-row drag (row 4, ProQ3 "Band 1 Dynamic Range", discRows=517 —
the same slot family that killed pids 14964/54224) SURVIVED: 3.2 s, 13
TICK-DRAGs, ended `DRAG-OP-END (container teardown clean)` — the FIRST
clean drag completion in the whole trial history. No dump, because no
exception happened inside the DLL — the handler did its job correctly by
having nothing to catch.

**Interpretation (needs one more data point):** either the crash is
intermittent (consistent with the clean 0.1.14 discovered drag, 24 Nov
16:41) OR the death is EXTERNAL to HeddleProxy.dll (DAW crash or DAW
killing the plugin) — in which case a VEH in our DLL never fires and a
FATAL drag on 0.1.14c with NO dump is itself decisive evidence pointing
at the DAW side. Next step: fatal drag on 0.1.14c + WER Application
Error event (faulting module) + what dies (whole DAW vs plugin slot).

## Status (2026-11-24 evening)

**Crash character (field-proven, stable across 0.1.14 → 0.1.14b):**
MID-DRAG death. In every recorded crash the last log line is
`TICK-DRAG buttonDown=1 dragSrc=1` — button still down — 130–720 ms after
`DRAG-OP-START`, and BEFORE the pointer reaches the route table (no
`DRAG-ENTER`/`DROP` lines). Affects BOTH list flavors (learn rows and
discovered rows) and both with the 0.1.14b learn-refresh guard active.

**Next instrument (0.1.14c):** SEH vectored exception handler →
`MiniDumpWriteDump` → `%USERPROFILE%\HeddleProxy\crash-<pid>-<ms>.dmp` +
`CRASH-DUMP code=0x… written` log line. Symbolicate with the PDB next to
`HeddleProxy.vst3` in `build-msvc-perf2` (WinDbg/cdb:
`cdb -z crash-*.dmp -y "path;C:\Program Files (x86)\Windows Kits\10\Debuggers\x64" -i path -c ".ecxr; kv; lm; q"`).
The dump is the arbiter — stop guessing stacks before it arrives.

## Suspected locations (ranked, to be confirmed by dump)

1. **JUCE drag machinery itself** — `DragImageComponent::mouseDrag` /
   `findTarget` / `checkForExternalDrag`
   (`juce_gui_basics/mouse/juce_DragAndDropContainer.cpp`). Basis: crash in
   both list flavors, before any drop-target contact, mid-drag with button
   held; 0.1.14b changed nothing in this path yet the discovered-drag
   regression appeared (run 2 of 24-Nov log).
2. **Drop-target side** — `RouteTable` (plugin.cpp L~610–812,
   `itemDragEnter/Move/Exit`) is our code but its hooks never log before
   death ⇒ less likely, not excluded (death could precede the first
   `itemDragEnter`).
3. **Learn-list refresh** — FALSIFIED as sole cause (guard firing, crash
   persists). Keep the guard + `learn-refresh-DEFERRED` edge log (useful
   evidence, harmless).

## Trials

| Build  | Date       | Hypothesis / change                                        | Field result                                                    | Verdict                          |
|--------|------------|------------------------------------------------------------|-----------------------------------------------------------------|----------------------------------|
| 0.1.13 | 2026-11-23 | baseline (drag + runlog)                                   | crash on drag (first field report)                               | baseline broken                  |
| 0.1.14 | 2026-11-24 | run-marked logging (HEDDLE_LOGDIR/NAME, RUN-START/END, PID) | discovered drag clean at 16:41; learn-row drag "crashed on release" | logging proven; crash confirmed |
| 0.1.14b| 2026-11-24 | learn-list freeze during drag (`dragActive` gate + DEFERRED edge log) + wxs remember-paths condition → `1` | ALL 4 runs crashed MID-drag (0.13–0.72 s after DRAG-OP-START, buttonDown=1); guard fired in all 4; discovered drag ALSO crashed (regression) | **FALSIFIED** — crash not in learn-list refresh |
| 0.1.14c| 2026-11-26 | SEH VEH minidump handler (RelWithDebInfo build, `bld=` token); NO functional change | Reinstalled; 1 discovered drag (row 4 ProQ3 "Band 1 Dynamic Range", 517 rows) survived 3.2 s → `DRAG-OP-END (container teardown clean)` — first clean completion in history; handler armed, no in-DLL exception → no dump (correct) | Crash not reproduced in this attempt; a FATAL drag with no dump would prove the death is external to the DLL (→ WER event) |

## Field logs studied

- `doc_75129f6c` (24 Nov 15:xx, 2 runs): pid 4980 clean discovered drag;
  pid 42740 learn-row `/fader1` drag → death. (Initially read as
  release-time crash — WRONG, see below.)
- `000_debug.log` (24 Nov 22:05–23:21, 4 runs, pids 25440/14964/16608/65464):
  all four die mid-drag on `TICK-DRAG buttonDown=1 dragSrc=1`;
  `learn-refresh-DEFERRED` present in all four; run 2 = discovered
  (discRows=517, learnRows=0) crash; run 4 contains a cancelled drag
  (no DRAG-START) before the fatal one.
- `000_debug.log` + `debug.log` (26 Nov 14:01–14:28): pid 54224 (14:02)
  died mid-drag on the OLD binary (no `bld=` token — DAW had cached it
  pre-reinstall); pid 38780 (14:26, `bld=0.1.14c`, `CRASH-DUMP-ARMED`)
  dragged the same ProQ3 slot family for 3.2 s → clean `DRAG-OP-END`.
  Bridge side: editor closed (clean) ~10–13 s BEFORE the fatal drags in
  every run (also before the clean 14:28 one) — likely DAW window
  behavior, not a signal.

## Key evidence rules

- `fieldLog` is synchronous append (flushed per line) ⇒ the LAST line of a
  crash log is trustworthy evidence of where the process was alive.
- `DRAG-OP-END` only fires when the `DragImageComponent` is actually
  destroyed (dtor → `dragOperationEnded`) ⇒ its absence = unclean death.
- `isDragAndDropActive()` = `dragImageComponents.size() > 0` (no locking).
- `DragImageComponent` ctor starts a 200 ms timer; `mouseUp` →
  `dismissWithAnimation` (120 ms fade).

## Build pitfalls (0.1.14c crash-dump block — do not repeat)

1. **`#if JUCE_WINDOWS` is FALSE before `JuceHeader.h`.** `JUCE_WINDOWS` is
   only defined inside JUCE's `juce_TargetPlatform.h` (pulled in by
   JuceHeader.h). It is NOT in the project's `/D` preprocessor defines. So a
   top-of-file `#if JUCE_WINDOWS` block (before the JuceHeader include) is
   silently skipped — `windows.h`/`dbghelp.h` never included → later
   `C2065 PEXCEPTION_POINTERS/LONG/CreateFileW undeclared`. Guard top-of-file
   Windows includes on **`WIN32`** (defined by the project), not JUCE_WINDOWS.
   The crash-dump block itself (after JuceHeader, in-namespace) is fine with
   `#if JUCE_WINDOWS` because JUCE_WINDOWS is defined there.
2. **`MINIDUMP_EXCEPTION_INFORMATION` layout** (minidumpapiset.h):
   `{DWORD ThreadId; PEXCEPTION_POINTERS ExceptionPointers; BOOL
   ClientPointers;}` — pass `info` (the `PEXCEPTION_POINTERS`) whole as the
   second member, not its fields.
3. **`MiniDumpWriteDump` takes non-const** `PMINIDUMP_EXCEPTION_INFORMATION`
   → declare the local without `const` or you get C2664.
4. **Hex-format a DWORD with `String::toHexString((int64)c)`**, not the
   `(int)` `String` ctor (0xC0000409 > INT_MAX → negative hex) and not
   `String(DWORD,16)` (ambiguous overloads in this JUCE fork → C2440).
5. **`dbghelp.h` must be included in GLOBAL scope** (not inside
   `namespace heddle_proxy {`), else the SDK declarations pollute the
   namespace → C1003 (100+ errors).

## Open questions

- Why did the 0.1.14 discovered drag (16:41) pass but 0.1.14b's (22:20)
  crash? (Possible: different DAW state, plugin count, or the 0.1.14b
  timer/gate code itself perturbing the drag path — the minidump will show.)
- ~~Where did `000_debug.log` come from~~ RESOLVED (2026-11-26): user set
  `HEDDLE_LOGNAME=000_debug.log` in user env vars; `03d` was meant as an
  int index, the name is cosmetic.
- NEW: is the death external to HeddleProxy.dll? A fatal drag on 0.1.14c
  with no dump (no in-DLL exception while armed) would prove it. If so:
  WER Application Error event → faulting module names the real victim
  (DAW exe/dll). User's reports ("DAW dies vs slot blanks", "crashes when
  it touches some element") already hint at DAW-side drag handling.
- Does the crash need a specific route-table width / number of learned
  rows? (runs 1/3/4 = 1–2 learn rows; run 2 = 0 learn / 517 discovered.)

## Procedure for the next round (0.1.14c)

1. User installs, starts DAW from the shell with
   `HEDDLE_LOGDIR`/`HEDDLE_LOGNAME` set, reproduces the drag crash once.
2. User sends the `crash-*.dmp` + the log (expect `CRASH-DUMP … written`).
3. Symbolicate locally (command above). Expect: exact function + line.
4. Update this doc: verdict on the ranked locations, next fix.
