Heddle v0.1.8 - full Windows baseline (repo: iriserme-mva/heddle, commit 472925d, CI green 40152029920)

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
    C:\Program Files\Common Files\VST3\
              all four VST3 bundles - the machine-wide VST3 discovery
              folder, so DAWs find them with no extra configuration
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

New in this build (472925d)
  - per-route value mapping: arg (multi-float select), in_min/in_max rescale
    (reversed range inverts), curve (identity/exp/invexp/scurve) - config file or editor UI
  - RECEIVED learn list in the editor (ARG / MIN / MAX / CURVE columns)
  - OSC wire framing documented: 4-byte big-endian length header + OSC 1.0 body (docs/OSC.md)

Full spec: docs/PROXY.md, docs/OSC.md, docs/PACKAGING.md in the repo (GitHub: iriserme-mva/heddle).

Archive
  This archive is 7z with header encryption (filenames encrypted too) - nothing
  inside is visible to any scanner. Password was sent with the delivery message.
  Extract with 7-Zip: https://www.7-zip.org/ (free).
