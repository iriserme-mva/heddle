Heddle v0.1.8 - full Windows build (repo: iriserme-mva/heddle)
Commit: 7e6c359 (fix batch 3e0fb67)   Build date: 2026-11-02 20:29 UTC
CI core tests: 79/79 cases, 3108 assertions green; MSVC x64 build clean.

Contents
  heddle-0.1.8-win-x64.msi      MSI installer (see below) - recommended
  bin/heddle-host.exe           standalone live host: loads a chain, live audio I/O, OSC control protocol
  bin/heddle-render.exe         offline audio renderer: heddle-render input.wav chain.json output.wav
  bin/heddle-proxy.exe          OSC fan-out router (no audio, no DAW needed)
  plugins/HeddleProxy.vst3      passthrough VST3 (audio stays dry; routes OSC; mapping + learn-list editor)
  plugins/HeddleSmokePlugin.vst3  test VST3 (good DAW smoke-test target)
  plugins/HeddleBridge.vst3     DAW-facing shared-memory ring bridge
  plugins/HeddleProbeAudio.vst3 audio probe VST3
  LICENSE                       Heddle License (owner: commercial; others: non-commercial)
  README.txt                    this file

MSI installer (heddle-0.1.8-win-x64.msi)
  Per-machine, 64-bit. Double-click to install (UAC elevation prompt is
  normal - it writes into Program Files). Installs to:
    C:\Program Files\Heddle_v0.1.8\
      bin\    the three executables
      LICENSE, README.txt
  VST3 plugins: the installer has its own "VST3 plugins" location line.
    Default: C:\Program Files\Common Files\VST3  (machine-wide DAW discovery)
    -> change it to your own VST3 folder if you keep plugins elsewhere
       (relative to C:, e.g. "Program Files\Steinberg\VST3")
  Start menu:  Start > All Programs > Heddle_v0.1.8 >  Heddle Host / Heddle Render / Heddle Proxy
  Uninstall:  Settings > Apps > Heddle.
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

New in this build (7e6c359)
  - 9 reported issues fixed (3e0fb67):
    1. ring bridge survives host restart (waits out stale named section)
    2. idle CPU: pump thread no longer busy-spins (~9% of a core saved)
    3. VST3 subfolder bundles = standard layout (documented)
    4/5. bigger, higher-contrast fonts in route table + header
    6. route editor: cell-wise surface/param, drag-a-line onto a row,
       global LOCK switch (read-only mode)
    7. scurve/exp/invexp share one curvature knob K (K=1 = old behavior)
    8. route list scrollbar (stock juce::Viewport)
    9. state persistence: proxy field diff (no spurious dirty) + bridge
       autosaves on every edit
  - perf metrics policy: one JSON + SVG infographic per commit, in-repo
    under perf/, committed back by CI (perf/ has the 133-run baseline,
    25 commits, 3 platforms)
  - perf gains (CI, Windows host): set->ack 23.6 -> 5.0 ms, UDP loopback
    RTT 16.3 -> 10.2 us, reliable control rate 1000 -> 5000 sets/s,
    audio deficit <2% -> <=0.1% at ceiling

Full spec: docs/PROXY.md, docs/OSC.md, docs/PACKAGING.md in the repo
(GitHub: iriserme-mva/heddle).

Integrity (files in this archive)
  heddle-host.exe        sha256 d14199ef57bcac34d135ad891993caf73567ed391e27af496d528149472b5456
                         md5    47abd3cc973c0780fdc29a378ed09f3b   (3,539,968 bytes)
  heddle-render.exe      sha256 3737b01ac604b62ef5ef8ae3d382eebe70596fe857e24ce140c6d115e7a4b52b
                         md5    12e28fea25465025afb69368fc17a6d1   (4,219,392 bytes)
  heddle-proxy.exe       sha256 1f2d29b317ea9c75ac3b4273f1ecf3a369b826e37a4c8ecc3b1494f11f6776d5
                         md5    c500dca3a21217a016a0f5dcfa4104f3   (195,072 bytes)
  HeddleProxy.vst3       sha256 931002d75b6b1671413ef7dba7ef95a0d063debdf6d009525f0d1001943c6dd0
                         md5    947dea35482abe02e402b326c0a65a67   (3,336,192 bytes)
  HeddleSmokePlugin.vst3 sha256 3e44ffb4fedcd53a5da4f51944b2acdcffc7a5cc8483f4c154fbc963b1e24c6d
                         md5    d9e2e298fda360d2169cffc849b1db82   (3,021,824 bytes)
  HeddleBridge.vst3      sha256 87b3f1be26537d55f3e176da2ddcbb5f04ef1661fc44b5aea03c4479ae38306e
                         md5    dbf5d6881cfa69deffe05f2cf3bb2aef   (3,537,920 bytes)
  HeddleProbeAudio.vst3  sha256 8ec6155903079b45fc5478dccae2123cc021b6e22f8346e7066a5f9115db8083
                         md5    fe1d39df58ddbc6658a5059560d295b6   (47,616 bytes)
  LICENSE                sha256 617310cac2e15d9033e4fb0b9b42af4b7616bd6b7209d5b31c61bc4c181eac6b
                          md5    44447db2f3bfc0ba6a0ef45f9a2730c7   (39,210 bytes)
  heddle-0.1.8-win-x64.msi   hash in the delivery message (a file cannot carry its own hash;
                          it is embedded in the archive alongside these fingerprints)

  Verify with:  certutil -hashfile <file> SHA256   (Windows)
                sha256sum <file>                   (macOS/Linux/Git-Bash)

Archive
  This archive is 7z with header encryption (filenames encrypted too) - nothing
  inside is visible to any scanner. Password was sent with the delivery message.
  Extract with 7-Zip: https://www.7-zip.org/ (free).
