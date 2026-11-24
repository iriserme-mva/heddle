# Bridge drag crash/hang hardening

**Status:** In progress — hardening committed (`dcba74c`); run-marked logging added 2026-11-24, **both VST3s rebuild clean** (bridge + proxy, build-msvc-perf2 Release, 2026-11-24 12:35); awaiting field test. Uncommitted.  
**Target version:** 0.1.14  
**Delivery:** The user has NO access to the build host — every change that needs a field test ships as a **fresh MSI via email** (see `docs/README.txt` / task 00004); never reference local build-tree artifacts as test material.  

**Description:** Exception‑proof all `FileDragAndDropTarget` overrides in `host/heddle_bridge/bridge.cpp`, add trace logging for drag phases, and rebuild `HeddleBridge.vst3`. Field logs additionally showed the drag crash/hang repros are proxy-plugin in-app drags — so run banners + `HEDDLE_LOGDIR` support were added to BOTH plugins (see appendix §5–6).  
**References:** STATUS.md entry 2026‑11‑22, currently open.  
**Next steps:** Field test with `HEDDLE_LOGDIR=…\temp\run_v.0.1.14\logs` + `HEDDLE_LOGNAME=03d_debug.log` (the proxy's log lands at `temp\run_v.0.1.14\logs\03d_debug.log` as requested; the bridge keeps `debug.log` in the same dir), study the run-marked log, commit the logging changes, repackage MSI.

## Appendix — Code‑linked analysis (verified against working tree, 2026‑11‑24)

### 1. JUCE side: the unprotected OLE → JUCE callback chain

Vendored JUCE 8.0.4 (`build-msvc-perf2/_deps/juce-src/`):

- `modules/juce_gui_basics/native/juce_Windowing_windows.cpp`, lines **1982–2027** — `HWNDComponentPeer::FileDropTarget` (an `IDropTarget` COM object). Every COM callback funnels straight into the peer with **no try/catch anywhere in between**:
  - `DragEnter` (L1986) → `updateFileList()` (L2087: `DragQueryFile` over the `HDROP`, builds `String` per path) → `DragOver` → `peer.handleDragMove(dragInfo)` (L2011)
  - `DragOver` (L2005) → `peer.handleDragMove` (L2011) — runs on **every mouse move** while dragging
  - `DragLeave` (L1996) → `peer.handleDragExit` (L2001)
  - `Drop` (L2016) → `peer.handleDragDrop` (L2024)
  These run **inside the OLE COM dispatch thread**; an exception escaping any of them is uncatchable by JUCE and takes the DAW down (or wedges the drag manager → "second drop hangs").
- `modules/juce_gui_basics/windows/juce_ComponentPeer.cpp`, lines **478–582**:
  - `handleDragMove` (L478) calls `DragHelpers::findDragAndDropTarget` → `isInterested(info, c)`, which invokes our **`isInterestedInFileDrag()` synchronously** — this is the "crash on drag‑enter / over the list" path.
  - `handleDragDrop` (L542) invokes `filesDropped()` via `MessageManager::callAsync` (L566–575) — so a hang in `filesDropped` occurs on the **message thread** and stalls the entire plugin UI; the OLE callback itself has already returned, which is why the drag manager "appears" to hang on the next attempt.

### 2. Heddle side: the overrides that used to be exposed (now hardened, `dcba74c`)

`host/heddle_bridge/bridge.cpp` (line numbers after the 2026-11-24 logging rework):

| Callback | Lines | What it does | Old risk / current state |
|---|---|---|---|
| `BridgeEditor` class decl, inherits `juce::FileDragAndDropTarget` | L665-667 | the editor is the drop target | - |
| `dragOver_` highlight in `paint()` | L795 | `0x6600e5ff` over `0xff2a2a2a` while dragging | - |
| OLE no-try/catch comment (doc-to-code link) | L837-850 | why every override is wrapped | verified against build-tree JUCE 8.0.4 |
| `dragLog()` | L851-853 | thin wrapper over `bridgeLog("drag ...")` | - |
| `isInterestedInFileDrag` | L854-865 | scans extensions, accepts `.vst3/.vst/.dll/.so/.json` | ran **synchronously inside the OLE callback**; now wrapped, returns `false` on throw |
| `fileDragEnter` | L866-872 | sets `dragOver_`, `repaint()` | now logs `enter n=<k>` + try/catch |
| `fileDragExit` | L873-879 | clears highlight | now logs `exit` + try/catch |
| `filesDropped` | L880-901 | per file: `.json` -> `loadConfigFromFile()` (L1127: `loadFileAsString()` + `parseConfigText()` L545 + `showWarn()` **modal** L927 on parse error); plugin -> `addSlotFromPath()` (L1021: dup check + `persistSlots()` L1010 -> JSON build + Base64 + `settings_.store()`); then `list_.updateContent()` + `repaint()` | the **heaviest** callback - modal `AlertWindow` and disk I/O on the message thread via `callAsync`; now every phase logged (`drop n=...`, `drop[i] ...`, `drop done slots=...`), catch resets `dragOver_` |

Shared logging (0.1.14): `bridgeLogFile()` L162 (HEDDLE_LOGDIR override), `bridgePid()` L175, `bridgeLog()` L180; RUN-START/RUN-END banners at processor ctor/dtor L212/L220 and editor ctor/dtor L769/L788.

### 3. Why the second drop "hangs" (root‑cause model, from the 2026‑11‑22 STATUS entry)

Pre‑hardening, any throw in `isInterestedInFileDrag`/`fileDragEnter`/`fileDragExit` (e.g. a malformed path from the OLE data object, or `File` ops on a path with bad characters) escaped into COM → fatal in most DAWs. In DAWs where OLE tolerates it, the peer's `dragAndDropTargetComponent` / `lastDragAndDropCompUnderMouse` state (ComponentPeer.cpp L481–539) gets left half‑updated, so the next `DragEnter` never re‑arms the target → **first drop works, second drop does nothing** = the observed hang.

### 4. Diagnostic signature

`debug.log` lives next to the bridge settings file (`BridgeSettings::file().getParentDirectory()`). A `drag drop n=…` line **without** a following `drag drop done slots=…` pins the hang to one of the per‑file handlers (`loadConfigFromFile` or `addSlotFromPath`) — the next field report will say which one, and at which file index.

### 5. Field-log study (the 8 logs sent by the user, 2026-11-21 → 11-23)

Copies archived under `~\AppData\Local\hermes\profiles\mv\cache\documents\`.
All proxy lines come from `host/proxy_plugin/plugin.cpp` (`fieldLog`), the
bridge lines from `host/heddle_bridge/bridge.cpp`. Timestamps are
`Time::currentTimeMillis()` (UTC epoch).

| File (short) | Wall time (UTC) | Plugin | What it shows |
|---|---|---|---|
| `doc_d0f664a51179` | 11-21 09:45 | bridge | load + 2 stores, clean |
| `doc_d604e4a7abfc` | 11-21 09:47 | proxy | 1× `DRAG-START` (discovered, ProQ3 Band1 Used) — log ends |
| `doc_d740e967dc02` | 11-21 11:10 | proxy | "crash surface — once dragged out of the list": ends at `DRAG-START source=learn /fader1` + one TICK — no `DRAG-OP-START` (pre-0.1.14 container hooks) |
| `doc_c480aa8854f2` | 11-21 11:20 | proxy | "crash tgt — once drag starts": ends at `DRAG-START source=discovered row=3 … Band 1 Gain` — no container hooks yet |
| `doc_11a931344807` | 11-21 11:33 | proxy | long session; 26× `TICK-DRAG-START` edge events; learn-rebuilds `0→1→2` repeatedly; ends mid-drag (line 171, `TICK-DRAG … discRows=517`) |
| `doc_be670db01dfd` | 11-23 10:38 | bridge | load → `restore OK slots=1 stale=0 decodedBytes=250` → 2 stores — restore path healthy after 0.1.13 base64 fix |
| `doc_1362bd3092b6` | 11-23 10:38 | proxy | **0.1.14 container hooks present**: 2× `DRAG-OP-START` + matching `DRAG-OP-END (container teardown clean)` — in-app drag lifecycle now fully logged and clean |
| `doc_9c71f4526577` | 11-23 11:51 | proxy | **the hang signature**: 3× `DRAG-START` (discovered ProQ3 Band1 Threshold ×2, learn /xy1 ×1) each followed by `DRAG-OP-START`, and **zero `DRAG-OP-END`** — the drag image is never torn down |

Key conclusions:

1. **The drag crash/hang is in the PROXY plugin, not the bridge.** Every
   drag-event log line (`DRAG-START/TICK-DRAG*/learn-rebuild/DRAG-OP-*`) is
   proxy-side; the bridge logs only `load/store/restore/drag` (file-drop).
   The JUCE OLE-FileDropTarget analysis (§1) still applies to the bridge's
   file drops, but the reproducible crash/hang is the proxy's **in-app**
   `DragAndDropContainer` lifecycle: `DragImageComponent::~DragImageComponent`
   (`juce_DragAndDropContainer.cpp` L75–89) is the only place
   `dragOperationEnded` fires — if that dtor never runs (the desktop drag
   source outlives the window, or the DAW kills the plugin mid-drag), the
   log dies exactly as in `doc_9c71f` (3 STARTs, 0 ENDs).
2. **The 11-23 10:38 session (`doc_1362b`) is the first fully green one** —
   both drags `OP-START` → `OP-END` cleanly, 517 discovered rows live.
   The 11-23 11:51 session (`doc_9c71f`) then reproduced the hang 13 min
   later: drags started, container never ended.
3. **No app-start markers existed** — runs blend into one file, so `doc_11a93`
   (26 press/drag edges over ~15 min) can't be split into test runs, and a
   crash's last line can't be attributed to a specific DAW launch. That is
   what this task's logging change fixes (§6).
4. `learn-rebuild rows N→M` with `buttonDown=0` is the normal 4 Hz tick
   behavior; only rebuilds during a press are the 0.1.12 hazard (none appear
   with `buttonDown=1` in these logs — the guard is holding).

### 6. Logging changes made for this task (2026-11-24)

Both plugins now:
- **Resolve the log file** from the `HEDDLE_LOGDIR` env var first
  (`<dir>\<name>.log` — dir must exist), else the old default
  (`~\HeddleProxy\debug.log` / `~\HeddleBridge\debug.log`).
  `HEDDLE_LOGDIR` is inherited by the DAW and every plugin it loads, so ONE
  var covers both plugins and the host. The proxy's file NAME additionally
  honors `HEDDLE_LOGNAME` (default `debug.log`); the bridge always writes
  `debug.log` in that dir — that way the task's requested
  `temp\run_v.0.1.14\logs\03d_debug.log` is exactly the proxy's run log,
  next to the bridge's, with no name collision.
  Harness: `mkdir temp\run_v.0.1.14\logs` then set `HEDDLE_LOGDIR` and
  `HEDDLE_LOGNAME=03d_debug.log` before launching the DAW.
- **Append a wall-clock** to every line: `<ms-epoch>  <YYYY-MM-DD HH:MM:SS>  <msg>`
  — epoch stays for sorting, wall-clock makes multi-run files readable.
- **Write RUN-START / RUN-END banners** with `plugin=, ver=<JucePlugin_VersionString>, pid=`
  from BOTH the processor ctor/dtor (true instance span — covers DAW
  enumeration instances that never open an editor) AND the editor ctor/dtor
  (window actually opened/closed). A `RUN-START` with no matching `RUN-END`
  in the same pid = that DAW instance was killed (crash); the last
  `RUN-START` before a crash line identifies the run.

Expected field log after the next test,
`temp\run_v.0.1.14\logs\03d_debug.log` (proxy; `HEDDLE_LOGNAME=03d_debug.log`)

```
1795600000000  2026-11-26 10:00:00  RUN-START plugin=HeddleProxy ver=0.1.14 pid=1234 instance created
1795600000120  2026-11-26 10:00:00  RUN-START plugin=HeddleProxy ver=0.1.14 pid=1234 editor open (DAW instance start)
1795600000130  2026-11-26 10:00:00  RUN-START plugin=HeddleBridge ver=0.1.14 pid=1234 instance created
1795600000140  2026-11-26 10:00:00  load chainJsonBytes=336 hostExe=D:\Program Files\samples\Heddle\bin\heddle-host.exe
1795600000145  2026-11-26 10:00:00  restore OK slots=1 stale=0 decodedBytes=250
1795600000150  2026-11-26 10:00:00  RUN-START plugin=HeddleBridge ver=0.1.14 pid=1234 editor open (DAW instance start)
... (test activity; last line before a kill is the crash site)
1795600100000  2026-11-26 10:01:40  RUN-END plugin=HeddleProxy ver=0.1.14 pid=1234 editor closed (clean)
```

### 7. Remaining verification

- Field test on the user's box: launch the DAW with
  `HEDDLE_LOGDIR=<repo>\temp\run_v.X.Y.Z\logs` (create the dir first),
  reproduce the drags, send the single `debug.log` back. The run banners
  attribute every line; the `RUN-START` without `RUN-END` pins the killed
  instance, and the last line inside it is the crash site.
- `HeddleBridge.vst3` / `HeddleProxy.vst3` rebuilt clean 2026-11-24
  (build-msvc-perf2, Release) — the new binaries carry the run banners;
  repackage the MSI once the field test confirms the fix (task 00004).
- Uncommitted working-tree: `bridge.cpp` + `proxy_plugin/plugin.cpp`
  (logging rework), `tasks/`, plus the 0.1.14 `docs/README.txt` refresh.
  Commit together with the next status pass.
