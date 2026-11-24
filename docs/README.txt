Heddle v0.1.14 - full Windows build (repo: iriserme-mva/heddle)
Commit: dcba74c + 2026-11-24 run-marked logging (bridge + proxy)  Build date: 2026-11-24 (W. Europe)
MSVC x64 Release build clean; bridge round-trip test green (PASS ratio=0.5000).

Contents
  heddle-0.1.14-win-x64.msi   MSI installer (see below) - recommended
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
  to heddle-0.1.14-win-x64.msi before double-clicking. In the loose zip the
  file already carries its real .msi name.

MSI installer (heddle-0.1.14-win-x64.msi)
  Per-machine, 64-bit. Double-click to install (UAC elevation prompt is
  normal - it writes into Program Files). A license agreement dialog is
  shown before the directory step (owner: commercial; others:
  non-commercial - the full text is LICENSE in the install folder).
  Installs to:
    C:\Program Files\Heddle\
      bin\    the three executables
      LICENSE, README.txt
  VST3 plugins: the installer has its own "VST3 plugins" location line.
    Default: C:\Program Files\Common Files\VST3  (machine-wide DAW discovery)
    -> change it to your own VST3 folder if you keep plugins elsewhere
       (relative to C:, e.g. "Program Files\Steinberg\VST3")
  Install + VST3 locations are REMEMBERED: reinstalling/upgrading
  pre-fills both fields with your previous choices instead of resetting
  them to the Program Files defaults.
  Start menu:  Start > All Programs > Heddle >  Heddle Host / Heddle Render / Heddle Proxy
  Uninstall:  Settings > Apps > Heddle.
  Upgrades 0.1.8-0.1.13 installs in place (same product UpgradeCode).
    (If you prefer loose files over the installer: copy bin\* to any folder
    and the plugins\*.vst3 bundles into your DAW's VST3 folder.)

  Testing this build (field-test procedure)
    You have no access to the machine these builds are made on - the MSI you
    receive by email IS the test material; do not ask for anything from the
    build box. For the drag fix, please capture a run log while testing:
      1. Create a folder, e.g. C:\HeddleTest\logs
      2. Set two environment variables for this session (PowerShell):
           setx HEDDLE_LOGDIR "C:\HeddleTest\logs"
           setx HEDDLE_LOGNAME "03d_debug.log"
         (or, one-off per shell: $env:HEDDLE_LOGDIR="C:\HeddleTest\logs"
         $env:HEDDLE_LOGNAME="03d_debug.log") - then START the DAW from that
         shell / after a relog so the DAW inherits them.
      3. Do the drag tests (the ones that used to crash/hang).
      4. Send back C:\HeddleTest\logs\03d_debug.log (and debug.log, the
         bridge's, if present). Each line now carries epoch + wall-clock +
         PID, and every plugin instance writes RUN-START / RUN-END banners,
         so the log self-attributes: a RUN-START without a RUN-END is the
         DAW instance that died, and its last line is the crash site.
    Without the env vars the logs fall back to the old locations
    (C:\Users\<you>\HeddleProxy\debug.log and \HeddleBridge\debug.log) -
    those work too, just say which machine/DAW session they came from.

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

New in this build (0.1.14)
  - FIX (proxy): drag-release crash (0.1.14b). Field log 2026-11-24 showed a
    LEARNED-row drag dying the DAW ~250 ms after the mouse-up while a
    DISCOVERED-row drag released clean. Root cause: JUCE only counts a mouse
    source as "dragging" while the button is down, but the drag image lives
    on ~320 ms after that (120 ms fade + 200 ms delete timer). Our 4 Hz
    learn-list refresh ran on the first tick after release; with live OSC
    traffic the learned list changed, recycling the learn rows the drag
    teardown was still touching. Fix: freeze the learn list while
    isDragAndDropActive() is true (drag image alive), plus a
    "learn-refresh-DEFERRED" edge log so the next field log proves the guard
    fired. (Discovered drags were already clean since 0.1.14.)
  - FIX (installer): remember-paths now also runs on UPGRADE. The 0.1.14
    remember custom action was conditioned "NOT Installed", which is false
    on an in-place upgrade, so the HKCU\Software\Heddle\Remembered key was
    never written after an upgrade and the next install dialog showed the
    defaults. Now unconditional (before InstallFinalize). Verify: after this
    upgrade, ~\Heddle\remember.log gets a new line.
  - NEW (proxy + bridge): run-marked field logging. Every debug log line now
    carries ms-epoch + wall-clock + PID, and each plugin instance writes
    RUN-START / RUN-END banners (plugin/ver/pid) at construction and
    destruction - so a log that contains several DAW sessions still splits
    cleanly into runs, and a RUN-START without a RUN-END is the DAW
    instance that crashed (its last line = the crash site). Log location
    can be redirected per run: HEDDLE_LOGDIR env var (the DAW inherits it,
    so both plugins land in the same folder) + HEDDLE_LOGNAME for the
    proxy's file name. See "Testing this build" above for the exact
    procedure that captures C:\HeddleTest\logs\03d_debug.log.
  - FIX (bridge): drag crash/hang hardening. Every file-drag callback in the
    HeddleBridge editor (enter/exit/move/drop) is now exception-proof and
    trace-logged to the field-forensics log (HeddleBridge\debug.log): JUCE
    drives these from the OLE COM drop manager with no catch in between, so
    an escaping exception crashed most DAWs on drag-enter/release and, on
    others, wedged the drag manager (first drop works, second hangs). The
    new trace logs enter/exit/drop/drop-done per file, so the next hang
    self-reports the exact stuck phase instead of guessing.
  - FIX (installer): remember-paths. Reinstalling/upgrading now pre-fills
    the install-dir AND VST3-dir fields with your previous choices. The
    earlier version stored them in rows that (a) the uninstall deleted and
    (b) were written in the wrong registry hive to be read back - so the
    fields always reset to the Program Files defaults. The install now
    writes the final paths itself to HKCU\Software\Heddle\Remembered, a key
    no component owns, which no uninstall removes.
  - FIX (installer): license agreement dialog. The MSI now shows the
    Heddle License (owner-exclusive commercial / third parties
    non-commercial) as an agreement step before the directory page, with an
    explicit "I agree / I do not agree" choice.
  - NEW (binaries): version metainfo. The three executables (host/render/
    proxy) and the probe plugin now carry a proper Windows version resource,
    so the file "Details" tab shows 0.1.14 instead of 1.0.0.0.
  - (proxy + host: the JUCE plugin VERSION fields of the three VST3s now
    read 0.1.14, matching the release.)

Earlier (v0.1.13, 74e9b6c): bridge module-list restore (base64 format) +
drag forensics + stable install path C:\Program Files\Heddle (was
Heddle_vX.Y.Z). (v0.1.12, 2d1c405): drag-start crash guard + bridge list
clobber fix + field forensics log. (v0.1.11, 542b19c): editor drag-crash +
button-overlap + bridge list persistence. Full history: git log in the repo.

Full spec: docs/PROXY.md, docs/OSC.md, docs/PACKAGING.md in the repo
(GitHub: iriserme-mva/heddle).

Integrity (sha256 / md5 / size; verify with
  certutil -hashfile <file> SHA256   (Windows)
  sha256sum <file>                   (macOS/Linux/Git-Bash))
  NOTE: the .vst3 fingerprints below are of the inner x86_64-win module exe
  (the file a DAW actually loads), not the whole bundle directory.
  heddle-host.exe        60043959df21f4aba407194c4fda91b1606c9a3b6a1edca2b1e4f26862be4d01
                         md5 6e88aae6193807a5e0de3231b8c8f263   (3,540,992 bytes)
  heddle-render.exe      fb96eb4104060dd830982d26759d0014ad37928bc6e6f7146c5cd657af0e2cd9
                         md5 2583f23a5c8ce11f742507294a4db68d   (4,220,416 bytes)
  heddle-proxy.exe       d6ac08311c40c17cba6f30a2b9d749c647e364f6507a36e5995121ae32b89853
                         md5 6c261bd722500cb91fa59a01a69df94f   (196,096 bytes)
  HeddleProxy.vst3       37a34b3a5e97616ebbaeffff2abdd388d4ca1e0f076fb11b6b441d736f49180e
                         md5 2bec5d3c0be89dbfe8cb3029639899b7   (3,352,064 bytes)
  HeddleSmokePlugin.vst3 364103849378509f9a379953c7d108ac9d228879f46f1a908b58e8331353d0a6
                         md5 2816e2cf0d019fae5d696938b689d4f8   (3,021,824 bytes)
  HeddleBridge.vst3      9aa31004f031ccf0a0e9727574c851db4a89d130e2c76f599986dc12736b7c1f
                         md5 af1a7c5946e1842a84904c5b89aa6b32   (3,544,576 bytes)
  HeddleProbeAudio.vst3  911d95d7d10f7d2845859fc1b48fd1d72d2328f8159b01cf0f7018e37cff9aaa
                         md5 d0f94c56118cd0787287c09c48acabf5   (48,640 bytes)
  LICENSE                617310cac2e15d9033e4fb0b9b42af4b7616bd6b7209d5b31c61bc4c181eac6b
                         md5 44447db2f3bfc0ba6a0ef45f9a2730c7   (39,210 bytes)
  heddle-0.1.14-win-x64.msi   hash in the delivery message (a file cannot carry its own hash;
                         it is the only artifact not fingerprinted above)
