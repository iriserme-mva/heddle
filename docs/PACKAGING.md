# Packaging & delivery (Windows first, mac/nix investigated)

How Heddle release artifacts are built, what goes where, and — critically —
how they get to the user given provider policy. This is the baseline as of
v0.1.8 (2026-10-26); everything here is reproducible via
`scripts/build_package.ps1`.

## Windows package (implemented, `scripts/build_package.ps1`)

Staged payload (`dist/`):

| path | what |
|---|---|
| `heddle-<ver>-win-x64.msi` | the MSI installer (see below) — **inside** the 7z too |
| `bin/heddle-host.exe` | standalone live host (chain + OSC control) |
| `bin/heddle-render.exe` | offline audio renderer |
| `bin/heddle-proxy.exe` | OSC router / proxy (TouchOSC + DAW bridge) |
| `plugins/HeddleProxy.vst3` | passthrough VST3 with the mapping/learn editor |
| `plugins/HeddleSmokePlugin.vst3` | CI smoke plugin (useful as a DAW test target) |
| `plugins/HeddleBridge.vst3` | DAW-facing ring bridge plugin |
| `plugins/HeddleProbeAudio.vst3` | audio probe plugin |
| `LICENSE`, `README.txt` | |

### MSI installer (`installer/heddle.msi.wxs`, WiX 3.14)

Per-machine, **true 64-bit** package (UAC elevation on install is normal for
Program Files; uninstall via Settings › Apps › Heddle):

- Install dir `C:\Program Files\Heddle_v0.1.8\` (editable in the UI),
  `bin\` with the three executables (x64) + `LICENSE`, `README.txt`
- **Plugin directory configurable at install time** (its own line in the
  Install Directory page, property `VST3PLUGINSDIR`, default
  `Common Files\VST3` → `C:\Program Files\Common Files\VST3`, the
  machine-wide VST3 discovery folder). Users with their own VST3 path type
  it there (relative to `C:\`, e.g. `Program Files\Steinberg\VST3`). All four
  `<Name>.vst3` bundles land in the chosen dir, so no DAW configuration is needed.
- **Start › All Programs › Heddle_v0.1.8** › Heddle Host / Heddle Render /
  Heddle Proxy (shortcuts key on an HKCU registry value per ICE43)

Build: `candle.exe -arch x64 -dHEDDLE_SRC="<dist>" installer\heddle.msi.wxs`
then `light.exe obj\heddle.wixobj -ext WixUIExtension -ext WixUtilExtension`.
Components use fixed GUIDs (auto-generated ones are not allowed on
multi-file / Common Files / RemoveFolder components); `MajorUpgrade` with a
fixed UpgradeCode gives in-place upgrades.

**ICE80 gotcha (cost a debugging cycle — do not repeat):** in a 64-bit
package, `ProgramFilesFolder` is *always* the 32-bit "Program Files"
(WoW64 view). A `-arch x64` component then legitimately clashes with a
32-bit directory tree → `LGHT0204 ICE80: 64BitComponent uses 32BitDirectory`
on every component. The `.wxs` must root at **`ProgramFiles64Folder`**.
`Win64="yes"/"no"` on Directory is not supported in WiX 3.14 and only moves
the error around. Verify bitness after candle: the wixobj
`_SummaryInformation` PID 15 must be `2` (64-bit), PID 7 `x64`.

Tooling: WiX 3.14.1 portable at `D:\Hermes\MV\tools\wix314` (binaries zip
from the wixtoolset/wix3 release — the WiX 7 CLI msi and the winget MSI
install both fail in this non-interactive shell: msiexec 1602/83, no UAC
prompt possible). The shipped candle/light are x86 .NET tools; that is
normal and fine — WiX 3.x's candle `-arch` flag (not the tool's own
bitness) selects the target architecture. WiX 3.14 is the last MSI-era
release; its output MSIs are still current and supported on Win10/11.

### Delivery: why everything gets 552'd by Gmail, and what to do

**Verified against Google's live policy page**
(`support.google.com/mail/answer/6590`, fetched 2026-10-26) — this is
provider policy, not a consent/formatting issue:

1. **Blocked extensions** (attachment refused outright, even renamed-in-zip
   the *contents* are still scanned): `.exe .msi .bat .cmd .com .pif .scr
   .js .jse .vbs .vbe .wsf .ws .hta .lnk .jar .cpl .ins .msc .msp .mst
   .dll .sys .lib .msix .appx …` (full list on the page). So the **MSI can
   never be a Gmail attachment** — by extension.
2. **Password-protected archives with archived content** are explicitly
   blocked as a category, *in addition* to the extension list. Observed on
   this account 2026-10-25/26, all three 552'd:
   - plain `.zip` with exes → 552
   - `pyzipper` WZ_AES `.zip` (contents encrypted, **names plaintext**) → 552
     (scanner reads the plaintext `.exe` names in the central directory)
   - `py7zr` header-encrypted `.7z` (contents **and names** encrypted) → 552
     (the "password-protected archive" category rule fires regardless)

   Practical consequence: **Gmail from this account will not carry
   executable-bearing archives at all, encrypted or not.** "Encrypted
   filenames" is the *correct* technique for a generic mail scanner and is
   what the archive is built with, but it does not override Gmail's
   password-protected-archive ban. (The user's hunch that encrypted
   filenames is the standard for sending potentially-malicious exe/bat via
   email is right in principle — it's how people defeat *content* scanners;
   Gmail's ban here is a *category* rule layered on top.)

**Channels that work** (in preference order):
1. **Telegram DM** — `D:/Hermes/MV/scratch/send_tg_direct.py <file> --chat 1975747592`,
   exit 0 + `file_id` = delivered. Both the 7z and the MSI go here.
2. **GitHub release + link** — upload the artifact to a release (PAT in
   URL for `curl -4` uploads), send the link by email. Email carries only
   text + link, which is fine.
3. Gmail attachment of the archive — **does not work** (see above). Plain
   text mail with a link is fine.

## macOS / Linux: feasibility, complexity, what it would take

Investigated 2026-10-26. Short answer: **medium complexity on macOS, low
on Linux; neither needs new *code*, only packaging plumbing + signing
questions you may not want to answer for a personal project.**

### macOS (complexity: medium)

- **Format:** a `.app` bundle (or plain folder) + a DMG, optionally a
  `.pkg`. The VST3 is already a macOS-compatible bundle layout once built
  (`Contents/MacOS/HeddleProxy.vst3` + `Contents/Resources/`); JUCE handles
  the bundle on that OS. The three CLIs bundle as plain executables (or
  tiny `.app`s).
- **Tooling:** all free, scriptable, no GUI: `hdiutil` (DMG),
  `create-dmg`, `pkgbuild`/`productbuild` (.pkg), `codesign`,
  `stapler`/`spctl --assess`. CI: GitHub Actions `macos-latest`.
- **The hard parts (honest list):**
  1. **Code signing.** Unsigned binaries are quarantined: Gatekeeper
     rejects with "cannot be opened"; `xattr -dr com.apple.quarantine`
     (or right-click → Open on macOS 12+, ad-hoc `codesign -s -`) is the
     user workaround. **Ad-hoc signing (`codesign --force --deep -s -`)
     costs nothing and works for personal use** — no Apple ID needed.
     Developer-ID signing ($99/yr) removes the warning but requires
     notarization to `spctl`-pass, which is a real pipeline (notarytool,
     stapling) — overkill here.
  2. **VST3 discovery:** macOS DAWs scan `/Library/Audio/Plug-Ins/VST3`
     and `~/Library/Audio/Plug-Ins/VST3` — a per-user installer writing
     to the user dir is exactly what we want and needs no root.
  3. **Architecture:** JUCE builds `universal2` (arm64e + x86_64) with
     `-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64` — one build, both Mac types.
  4. **Existing friction on this host: none** — the build must run on a
     mac (or CI). Windows cannot cross-compile JUCE/VST3 for macOS.
- **Effort estimate:** ~1–2 h of script (a `build_package_mac.sh`
  mirroring the ps1: copy artifacts → `codesign` ad-hoc → `hdiutil` DMG →
  7z header-encrypt), assuming a mac/CI mac runner exists.

### Linux (complexity: low)

- **Format:** the natural unit is a **tarball / AppImage / .deb**. For a
  personal, non-distributed tool a **tar.gz (or the same encrypted 7z)**
  with `bin/` + `plugins/` and an `install.sh` is the pragmatic baseline.
  An **AppImage** (`linuxdeploy` + `appimagetool`, both free CLIs) gives a
  single-file, no-root, versioned artifact and is ~30 lines of config.
  A **`.deb`** (`dpkg-deb --build` on a hand-rolled control/ tree) is
  trivially buildable without apt and gives proper menu/desktop entries
  (`/usr/share/applications/heddle-*.desktop` + icon) — the Linux
  equivalent of the Start-menu links.
- **The hard parts:** essentially none for personal use.
  1. **No signing requirement** for personal use (AppArmor/Flatpak
     policies aside, a tarball you extract yourself just runs; the
     `noexec`/`setuid` quirks don't apply to normal binaries).
  2. **VST3 discovery:** `~/.vst3/` (user, no root) or
     `/usr/local/lib/vst3` (root). VST3 GUIs on Linux need X11 — the
     proxy's editor already ships GUI modules; `VSTGUI_LINUX`/X11 libs
     (`libx11-dev libxtst-dev`) are a CMake/CI dependency list, not code.
  3. **GLIBC version floor:** build on a reasonably old base (Ubuntu
     20.04/22.04) or it won't run on older distros; CI ubuntu-latest
     (24.04) binaries work on modern systems.
- **Effort estimate:** ~1 h of script (`build_package_nix.sh`): stage →
  `tar` or `appimagetool` → 7z header-encrypt. Runs on CI
  `ubuntu-latest`, no signing.

### Recommended sequencing

1. **Now:** Windows baseline (done — MSI + encrypted 7z + scripts in-repo).
2. **Next (cheap win):** Linux tarball/`install.sh` via CI `ubuntu-latest`
   — no signing, pure plumbing.
3. **After:** macOS ad-hoc-signed DMG via CI `macos-latest` — ad-hoc
   signing only; revisit Developer-ID + notarization only if it ever
   leaves your hands.
4. **Never:** emailing raw executables. Every platform's artifacts go to
   the user via Telegram DM / GitHub release link; encrypted-header 7z is
   the archive format of record (Gmail's category ban makes mail a
   link-only channel regardless).

## Reproducing this on another machine

- **WiX 3.14:** download `wix314-binaries.zip` from
  `github.com/wixtoolset/wix3/releases/download/wix3141rtm/` and extract
  to `D:\Hermes\MV\tools\wix314` (no installer needed; winget's MSI fails
  non-interactively).
- **py7zr:** `uv pip install --python <venv-python> py7zr` (system pip
  fails on this host).
- **Build tree:** `build-msvc-perf2` is VS-cmake-only (generator lock —
  see `heddle-project-ops` skill); artifacts at the paths in
  `build_package.ps1`.
- **Gmail creds:** `D:\Hermes\MV\.gmail_creds` (address/password/
  recovery_email) — text+link mail only; never attach executables.
