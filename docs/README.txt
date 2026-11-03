Heddle v0.1.9 - full Windows build (repo: iriserme-mva/heddle)
Commit: ed4051e (vector route table)  Build date: 2026-11-03 (W. Europe)
MSVC x64 Release build clean; core tests green (CI).

Contents
  heddle-0.1.9-win-x64.msi      MSI installer (see below) - recommended
  bin/heddle-host.exe           standalone live host: loads a chain, live audio I/O, OSC control protocol
  bin/heddle-render.exe         offline audio renderer: heddle-render input.wav chain.json output.wav
  bin/heddle-proxy.exe          OSC fan-out router (no audio, no DAW needed)
  plugins/HeddleProxy.vst3      passthrough VST3 (audio stays dry; routes OSC; mapping + learn-list editor)
  plugins/HeddleSmokePlugin.vst3  test VST3 (good DAW smoke-test target)
  plugins/HeddleBridge.vst3     DAW-facing shared-memory ring bridge
  plugins/HeddleProbeAudio.vst3 audio probe VST3
  LICENSE                       Heddle License (owner: commercial; others: non-commercial)
  README.txt                    this file

NOTE on the MSI filename in mail-delivered archives
  Inside a mail-delivered 7z the installer is stored under a neutral name
  (heddle-installer-x64, no ".msi" extension) because some mail scanners
  reject the .msi extension by pattern. After extracting, RENAME the file
  to heddle-0.1.9-win-x64.msi before double-clicking. In the loose zip the
  file already carries its real .msi name.

MSI installer (heddle-0.1.9-win-x64.msi)
  Per-machine, 64-bit. Double-click to install (UAC elevation prompt is
  normal - it writes into Program Files). Installs to:
    C:\Program Files\Heddle_v0.1.9\
      bin\    the three executables
      LICENSE, README.txt
  VST3 plugins: the installer has its own "VST3 plugins" location line.
    Default: C:\Program Files\Common Files\VST3  (machine-wide DAW discovery)
    -> change it to your own VST3 folder if you keep plugins elsewhere
       (relative to C:, e.g. "Program Files\Steinberg\VST3")
  Start menu:  Start > All Programs > Heddle_v0.1.9 >  Heddle Host / Heddle Render / Heddle Proxy
  Uninstall:  Settings > Apps > Heddle.
  Upgrades the 0.1.8 install in place (same product UpgradeCode).
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

New in this build (ed4051e)
  - Route table is now a RESIZABLE std::vector (unbounded), matching the core
    data model (core uses std::vector<ProxyRoute>; there was never a 64 cap in
    the engine). The editor's kMaxRoutes=64 fixed-row budget is gone: add rows
    without limit; the # index renumbers; memory is released when a row is
    removed. Also fixed a stale-index bug in the per-row remove buttons (they
    now resolve their row at click time, so removing a row can no longer shift
    a button onto the wrong row).
  - Editor UI (6181645): continuous # index column on the left of each route
    row, tN target-ID column, +20% fonts throughout, 3x taller surfaces/params
    lists, per-panel caps (12 routes / 4 targets per panel) with inner
    scrolling, wider target panel.
  - Crash fix carried forward: route rows are built on demand and all editor
    pointers are null-guarded; drop/fill indices are clamped to the live row
    count (prevents the NULL-deref that took the DAW host down on a drag onto
    an empty route row).
  - Idle CPU: pump thread no longer busy-spins at idle (~9% of a core saved).

Earlier (v0.1.8, 7e6c359): ring-bridge restart survival, VST3 subfolder bundle
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
  heddle-host.exe        cdd42cbb2a931d0f4dc662703382d72571d13d6d081c7c36a9253c845c44aa5b
                         md5 679203fed9d2999f9024036191080ae9   (3,539,968 bytes)
  heddle-render.exe      187e879ce5a0b65f5e951b37f8fa9bd9aa410eb23b495b08231932c57bda046a
                         md5 0e6e79631fc129c0aa7a63f97bf1129a   (4,219,392 bytes)
  heddle-proxy.exe       db90bcc3a2a7efee03eb2d23f3520b6630cb83cd5e4eeb0a9f6fb6e5d7cb53a1
                         md5 9552c097910740356437e72b023bc021   (195,072 bytes)
  HeddleProxy.vst3       7d580e311e6faa60f40ddc5e8226bca6c1a72ee87695c4f87e1c532a4b359bf3
                         md5 8c958c974ef4e526b95006fb277ddff4   (3,340,288 bytes)
  HeddleSmokePlugin.vst3 3e44ffb4fedcd53a5da4f51944b2acdcffc7a5cc8483f4c154fbc963b1e24c6d
                         md5 d9e2e298fda360d2169cffc849b1db82   (3,021,824 bytes)
  HeddleBridge.vst3      c8caa2c26efa385c8ac4f20f48bbef558fbbcc6746aa1a09374a158be9bf26b3
                         md5 0d70dd843acc15c2b8012dd93517d62b   (3,537,920 bytes)
  HeddleProbeAudio.vst3  8ec6155903079b45fc5478dccae2123cc021b6e22f8346e7066a5f9115db8083
                         md5 fe1d39df58ddbc6658a5059560d295b6   (47,616 bytes)
  LICENSE                617310cac2e15d9033e4fb0b9b42af4b7616bd6b7209d5b31c61bc4c181eac6b
                         md5 44447db2f3bfc0ba6a0ef45f9a2730c7   (39,210 bytes)
  heddle-0.1.9-win-x64.msi   hash in the delivery message (a file cannot carry its own hash;
                         it is the only artifact not fingerprinted above)
