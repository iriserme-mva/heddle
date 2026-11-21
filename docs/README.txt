Heddle v0.1.13 - full Windows build (repo: iriserme-mva/heddle)
Commit: 876c43d (bridge-restore fix + drag forensics + stable install path)  Build date: 2026-11-21 (W. Europe)
MSVC x64 Release build clean; core tests green (CI + local ctest, 79/79).

Contents
  heddle-0.1.13-win-x64.msi   MSI installer (see below) - recommended
  bin/heddle-host.exe         standalone live host: loads a chain, live audio I/O, OSC control protocol
  bin/heddle-render.exe       offline audio renderer: heddle-render input.wav chain.json output.wav
  bin/heddle-proxy.exe        OSC fan-out router (no audio, no DAW needed)
  plugins/HeddleProxy.vst3    passthrough VST3 (audio stays dry; routes OSC; mapping + learn-list editor)
  plugins/HeddleSmokePlugin.vst3  test VST3 (good DAW smoke-test target)
  plugins/HeddleBridge.vst3   DAW-facing shared-memory ring bridge
  plugins/HeddleProbeAudio.vst3 audio probe VST3
  LICENSE                     Heddle License (owner: commercial; others: non-commercial)
  README.txt                  this file

NOTE on the MSI filename in mail-delivered archives
  Inside a mail-delivered 7z the installer is stored under a neutral name
  (heddle-installer-x64, no ".msi" extension) because some mail scanners
  reject the .msi extension by pattern. After extracting, RENAME the file
  to heddle-0.1.13-win-x64.msi before double-clicking. In the loose zip the
  file already carries its real .msi name.

MSI installer (heddle-0.1.13-win-x64.msi)
  Per-machine, 64-bit. Double-click to install (UAC elevation prompt is
  normal - it writes into Program Files). Installs to:
    C:\Program Files\Heddle\
      bin\    the three executables
      LICENSE, README.txt
  VST3 plugins: the installer has its own "VST3 plugins" location line.
    Default: C:\Program Files\Common Files\VST3  (machine-wide DAW discovery)
    -> change it to your own VST3 folder if you keep plugins elsewhere
       (relative to C:, e.g. "Program Files\Steinberg\VST3")
  Start menu:  Start > All Programs > Heddle >  Heddle Host / Heddle Render / Heddle Proxy
  Uninstall:  Settings > Apps > Heddle.
  Upgrades 0.1.8-0.1.12 installs in place (same product UpgradeCode). This
  release moves the install folder from C:\Program Files\Heddle_vX.Y.Z to the
  stable C:\Program Files\Heddle - the old versioned folder is left behind
  and can be deleted after the upgrade.
  (If you prefer loose files over the installer: copy bin\* to any folder
  and the plugins\*.vst3 bundles into your DAW's VST3 folder.)

Quick start (standalone CLI, no config file needed)
  heddle-proxy --listen 9100 --mode only-control \n
    --target 127.0.0.1:9001 --target 192.168.1.40:9001 \n
    --route "/k/0" 0 smoke1 Gain --route "/k/0" 1 comp Threshold
  heddle-proxy --query 127.0.0.1:9001     discover a target's slots/params
  heddle-proxy -h / --help                full usage (includes --map, --curve, --learn-file)

  heddle-host chain.json --audio system --osc-port 9001   live host
  heddle-render input.wav chain.json output.wav           offline render

VST3: drop HeddleProxy.vst3 into your VST3 folder, insert on a track,
open its editor UI to configure routes/mapping visually (save writes the config
JSON next to the patch).

New in this build (876c43d)
  - FIX (bridge): the DAW-facing HeddleBridge VST3 no longer forgets the
    module list. It saved the slot/param list as raw base64 but read it back
    with a decoder that expects a "<bytecount>." prefix, so every restore
    silently failed and the list looked empty on reopen (slots=0). The
    decoder now matches the encoder; the restore result is written to the
    crash-surviving field log (HeddleBridge\debug.log) so you can confirm
    "restored N slots" instead of a silent slots=0.
  - FORENSICS (proxy): the drag crash that freezes the HeddleProxy editor is
    not yet fixed, but this build instruments every drag event so the crash
    site is identified from the field log. Both list drag sources log
    DRAG-START (source=discovered|learn, row, rowsInList); the route-table
    drop zone logs DRAG-ENTER / DRAG-MOVE (throttled) / DRAG-EXIT / DROP /
    DROP-CB-DONE; the editor timer logs a TICK-DRAG heartbeat while a drag
    is active. The last line written before the freeze is the crash site.
    The crash reproduces from BOTH the DISCOVERED list (large, 500+ rows)
    and a RECEIVED ("sensor surface") element drag.
  - INSTALLER: stable install path. The MSI now installs to
    C:\Program Files\Heddle\ instead of C:\Program Files\Heddle_vX.Y.Z\, so
    future upgrades no longer leave a pile of versioned folders. The
    per-machine product UpgradeCode is unchanged - 0.1.8-0.1.12 installs
    upgrade in place; the old Heddle_vX.Y.Z folder is left behind and can
    be deleted after the upgrade.

Earlier (v0.1.9, ed4051e): vector route table + editor UI (continuous #
index, tN column, +20% fonts, 3x taller surfaces) + NULL-deref crash fix.
(v0.1.8, 7e6c359): ring-bridge restart survival, VST3 subfolder bundle
layout, cell-wise surface/param editor + drag-a-line + LOCK switch, shared
curvature knob K, route scrollbar, state persistence, per-commit perf
infographics. Full history: git log in the repo.

Full spec: docs/PROXY.md, docs/OSC.md, docs/PACKAGING.md in the repo
(GitHub: iriserme-mva/heddle).

Integrity (sha256 / md5 / size; verify with
  certutil -hashfile <file> SHA256   (Windows)
  sha256sum <file>                   (macOS/Linux/Git-Bash))
  NOTE: the .vst3 fingerprints below are of the inner x86_64-win module exe
  (the file a DAW actually loads), not the whole bundle directory.
  heddle-host.exe        2410f25a9943194f1be8584e23b0589a2d1b67bc87344219b666a7cf1c16bf22
                         md5 b516345f5f0935ac8c446f52522a99ed   (3,539,968 bytes)
  heddle-render.exe      60433b3ee458a4e0d77c2ecaf5e18acc05346760b5aa0e9371893aed5e901501
                         md5 3715b200a47e6e2e2b10b622f36f9ca2   (4,219,392 bytes)
  heddle-proxy.exe       1903ef26e8db0acab5757ab78b0b4af8ac3fcf53bbd24973cb24306a215866b3
                         md5 c7ced598517d1810865e6030405227ed   (195,072 bytes)  [unchanged in 0.1.13]
  HeddleProxy.vst3       36aafc3cce1381d8efcc9aeca26bde66fb559fbfa5de3ab3b3e18f276c6839e4
                         md5 361a1165009e008d8accd619d0fde33b   (3,347,968 bytes)
  HeddleSmokePlugin.vst3 14c9efffe9393346e12ef19a9da0f66c53f398bcb6a29f7d31ddb291f08dfd81
                         md5 b1a230e3841c934d460b73547ce0f737   (3,021,824 bytes)
  HeddleBridge.vst3      c3c57a152b2c3ea8c6e393f54a5ad2aac1cd15b551ef6442fa9765c0033fcf31
                         md5 516da78ab939042fd650053989298573   (3,542,016 bytes)
  HeddleProbeAudio.vst3  3c24734f8d1776f46d5b4950eeb8cc093a15b51ffacdbb51ab9ef8f6bfe5bb29
                         md5 5a636311ee1c9ee4c46a76093f30fec6   (47,616 bytes)
  LICENSE                617310cac2e15d9033e4fb0b9b42af4b7616bd6b7209d5b31c61bc4c181eac6b
                         md5 44447db2f3bfc0ba6a0ef45f9a2730c7   (39,210 bytes)
  heddle-0.1.13-win-x64.msi   hash in the delivery message (a file cannot carry its own hash;
                         it is the only artifact not fingerprinted above)
