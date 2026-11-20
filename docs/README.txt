Heddle v0.1.12 - full Windows build (repo: iriserme-mva/heddle)
Commit: 528cbce (route-row hang fix)  Build date: 2026-11-17 (W. Europe)
MSVC x64 Release build clean; core tests green (CI + local ctest).

Contents
  heddle-0.1.12-win-x64.msi   MSI installer (see below) - recommended
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
  to heddle-0.1.12-win-x64.msi before double-clicking. In the loose zip the
  file already carries its real .msi name.

MSI installer (heddle-0.1.12-win-x64.msi)
  Per-machine, 64-bit. Double-click to install (UAC elevation prompt is
  normal - it writes into Program Files). Installs to:
    C:\Program Files\Heddle_v0.1.12\
      bin\    the three executables
      LICENSE, README.txt
  VST3 plugins: the installer has its own "VST3 plugins" location line.
    Default: C:\Program Files\Common Files\VST3  (machine-wide DAW discovery)
    -> change it to your own VST3 folder if you keep plugins elsewhere
       (relative to C:, e.g. "Program Files\Steinberg\VST3")
  Start menu:  Start > All Programs > Heddle_v0.1.12 >  Heddle Host / Heddle Render / Heddle Proxy
  Uninstall:  Settings > Apps > Heddle.
  Upgrades the 0.1.8/0.1.9 install in place (same product UpgradeCode).
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

New in this build (528cbce)
  - CRITICAL FIX: Ableton (and any DAW) froze on opening a session that
    contains the 0.1.9 HeddleProxy / HeddleBridge plugin. The route-table
    loop in the editor's constructor looped "for (i < numRows_)" while
    buildRouteRow() bumped numRows_ every iteration, so the loop bound grew
    with every iteration and never terminated: the message thread was pegged
    creating route rows forever, freezing the whole DAW. A second latent
    double-count left numRows_ at 2x the real row count, which would have
    segfaulted the editor on its first 4 Hz refresh. Fixed: the row count is
    captured up front and counting starts from 0, restoring the invariant
    numRows_ == routeSrc_.size() that the read loops rely on. Both faults were
    introduced by the 0.1.9 on-demand vector route-table refactor (ed4051e)
    and replaced the previously bounded 64-row table loop. Verified with a
    new open-sequence test harness (host/src/repro.cpp, heddle_repro) that
    drives the full DAW open path (instantiate -> prepare -> pump -> setState
    -> createEditor -> addToDesktop -> 6 s message pump) to a clean exit for
    both HeddleProxy and HeddleBridge, plus the core test suite.
  - heddle-proxy.exe: 150 ms pump idle fix (1520a1b) and backoff behaviour
    carried from 0.1.9; binary rebuilt and re-fingerprinted.

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
  heddle-host.exe        cdd42cbb2a931d0f4dc662703382d72571d13d6d081c7c36a9253c845c44aa5b
                         md5 679203fed9d2999f9024036191080ae9   (3,539,968 bytes)
  heddle-render.exe      187e879ce5a0b65f5e951b37f8fa9bd9aa410eb23b495b08231932c57bda046a
                         md5 0e6e79631fc129c0aa7a63f97bf1129a   (4,219,392 bytes)
  heddle-proxy.exe       1903ef26e8db0acab5757ab78b0b4af8ac3fcf53bbd24973cb24306a215866b3
                         md5 c7ced598517d1810865e6030405227ed   (195,072 bytes)
  HeddleProxy.vst3       8a02efb8438587ca5c0536e67995cff4ce5c909fad0eb8d8f68723cdc824219b
                         md5 e264e496f683fb29136e78fdca8c856b   (3,340,288 bytes)
  HeddleSmokePlugin.vst3 3e44ffb4fedcd53a5da4f51944b2acdcffc7a5cc8483f4c154fbc963b1e24c6d
                         md5 d9e2e298fda360d2169cffc849b1db82   (3,021,824 bytes)
  HeddleBridge.vst3      8da6c098b137d3762cc39649fbf9edb3951b8273f123ea7408469ca6640e50ce
                         md5 e6c68690539e7f914dcc527d0ffbbc4f   (3,537,920 bytes)
  HeddleProbeAudio.vst3  8ec6155903079b45fc5478dccae2123cc021b6e22f8346e7066a5f9115db8083
                         md5 fe1d39df58ddbc6658a5059560d295b6   (47,616 bytes)
  LICENSE                617310cac2e15d9033e4fb0b9b42af4b7616bd6b7209d5b31c61bc4c181eac6b
                         md5 44447db2f3bfc0ba6a0ef45f9a2730c7   (39,210 bytes)
  heddle-0.1.12-win-x64.msi   hash in the delivery message (a file cannot carry its own hash;
                         it is the only artifact not fingerprinted above)
