# build_package.ps1 — assemble the Heddle Windows release package (baseline).
#
# Usage (PowerShell, from anywhere):
#   powershell -NoProfile -ExecutionPolicy Bypass -File D:\Hermes\MV\projects\heddle\scripts\build_package.ps1
#
# What it does (idempotent; safe to re-run):
#   1. Stages dist\ from the build tree build-msvc-perf2:
#        bin\       heddle-host.exe, heddle-render.exe, heddle-proxy.exe
#        plugins\   HeddleProxy.vst3, HeddleSmokePlugin.vst3, HeddleBridge.vst3,
#                   HeddleProbeAudio.vst3   (full .vst3 bundles)
#        LICENSE, README.txt
#   2. Builds dist\heddle-<ver>-win-x64.msi via WiX 3.14 (candle -arch x64 + light),
#      per-machine install:  C:\Program Files\Heddle_v<ver>\  (true 64-bit package)
#      + C:\Program Files\Common Files\VST3\  (machine-wide VST3 discovery folder)
#      + Start menu  >  Heddle_v<ver>  >  Heddle Host / Render / Proxy.
#      NOTE: .wxs must use ProgramFiles64Folder (NOT ProgramFilesFolder, which is
#      always the 32-bit "Program Files" inside a 64-bit package — that mismatch
#      is what ICE80 flags).
#      First run of the MSI on a machine does the actual install (VST3 bundles
#      land where DAWs discover them + Start-menu links) — the MSI is the
#      installer for that; the package itself ships it.
#   3. Builds dist\heddle-<ver>-win.7z with py7zr header encryption
#      (filenames encrypted too). Payload = everything staged in dist,
#      including the .msi. Delivery is via Telegram DM (Gmail 552s both
#      .msi/.exe extensions and password archives as a category — see
#      docs/PACKAGING.md); the 7z is the single deliverable archive.
#   4. Verifies: sha256 of every staged file, MSI existence, 7z round-trip
#      decrypt + plaintext-name leak check. Prints a manifest at the end.
#
# Prereqs: WiX 3.14 at D:\Hermes\MV\tools\wix314 (candle.exe/light.exe) and
# the venv python with py7zr (path probed below). Both are already provisioned
# on this host; docs/PACKAGING.md explains how to provision on another box.

$ErrorActionPreference = "Stop"
$REPO   = Split-Path -Parent $PSScriptRoot
$DIST   = Join-Path $REPO "dist"
$BUILD  = Join-Path $REPO "build-msvc-perf2"
$WIX    = "D:\Hermes\MV\tools\wix314"
$LOGDIR = Join-Path $REPO "logs"
New-Item -ItemType Directory -Force -Path $LOGDIR | Out-Null

# ---- version from CMakeLists.txt --------------------------------------------
$verLine = (Get-Content (Join-Path $REPO "CMakeLists.txt") | Select-String 'project\(\S+ VERSION ([\d.]+)' | Select-Object -First 1)
$VER = $verLine.Matches[0].Groups[1].Value
Write-Host "[pkg] Heddle version: $VER"

# ---- 1. stage ----------------------------------------------------------------
$exe = @(
  "host\Release\heddle-host.exe",
  "host\Release\heddle-render.exe",
  "Release\heddle-proxy.exe"
)
$vst3 = @(
  "host\proxy_plugin\heddle_proxy_plugin_artefacts\Release\VST3\HeddleProxy.vst3",
  "host\test_plugin\heddle_smoke_plugin_artefacts\Release\VST3\HeddleSmokePlugin.vst3",
  "host\heddle_bridge\heddle_bridge_plugin_artefacts\Release\VST3\HeddleBridge.vst3",
  "HeddleProbeAudio.vst3"
)
Write-Host "[pkg] staging dist\ ..."
New-Item -ItemType Directory -Force -Path (Join-Path $DIST "bin")     | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $DIST "plugins") | Out-Null
foreach ($e in $exe) {
  $src = Join-Path $BUILD $e
  if (-not (Test-Path $src)) { throw "missing build artifact: $src" }
  Copy-Item $src (Join-Path $DIST "bin\") -Force
}
foreach ($v in $vst3) {
  $src = Join-Path $BUILD $v
  if (-not (Test-Path $src)) { throw "missing build artifact: $src" }
  $dst = Join-Path $DIST "plugins\"
  Copy-Item $src (Join-Path $dst (Split-Path $src -Leaf)) -Recurse -Force
}
Copy-Item (Join-Path $REPO "LICENSE")  (Join-Path $DIST "LICENSE")  -Force
# The user-facing README lives in the repo at docs/README.txt and is staged
# as dist/README.txt (the MSI embeds it too).
$docReadme = Join-Path $REPO "docs\README.txt"
if (-not (Test-Path $docReadme)) { throw "missing repo README: $docReadme" }
Copy-Item $docReadme (Join-Path $DIST "README.txt") -Force

# ---- 2. MSI ------------------------------------------------------------------
$msi = Join-Path $DIST "heddle-$VER-win-x64.msi"
$wxs = Join-Path $REPO "installer\heddle.msi.wxs"
$obj = Join-Path $REPO "installer\obj"
New-Item -ItemType Directory -Force -Path $obj | Out-Null
& (Join-Path $WIX "candle.exe") -arch x64 -dHEDDLE_SRC="$DIST" -out (Join-Path $obj "heddle.wixobj") $wxs
if ($LASTEXITCODE -ne 0) { throw "candle failed" }
& (Join-Path $WIX "light.exe") (Join-Path $obj "heddle.wixobj") `
    -ext WixUIExtension -ext WixUtilExtension -out $msi
if ($LASTEXITCODE -ne 0) { throw "light failed" }
Write-Host "[pkg] MSI built: $msi"

# ---- 3. encrypted-7z of the payload (NOT the .msi) ---------------------------
# Gmail blocks .msi by extension (see docs/PACKAGING.md) and 552s exe-bearing
# archives even when encrypted — so the .msi goes to the user via the channel
# that works (Telegram DM / GitHub release); the 7z is the archive payload.
$py = "C:\Users\boima\AppData\Local\hermes\installs\cc4e6b500c38c37e\environments\e0dde2e42da94bb0bffcaa0671109449\venv\Scripts\python.exe"
if (-not (Test-Path $py)) { throw "venv python with py7zr not found at $py" }
& $py (Join-Path $PSScriptRoot "make_pkg_7z.py") $DIST $VER
if ($LASTEXITCODE -ne 0) { throw "7z build/verify failed" }

# ---- 4. manifest -------------------------------------------------------------
Write-Host ""
Write-Host "==================== PACKAGE MANIFEST ===================="
Get-ChildItem $DIST -Recurse -File | Where-Object { $_.Name -ne "heddle-$VER-win.7z" -and $_.Name -notlike "*.wixpdb" } |
    ForEach-Object {
        $sha = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 12)
        "{0,10}  {1}  {2}" -f $_.Length, $sha, $_.FullName.Substring($DIST.Length + 1)
    }
Write-Host "==========================================================="
Write-Host "[pkg] DONE. Deliverable archive: $(Join-Path $DIST "heddle-$VER-win.7z")"
Write-Host "[pkg] MSI for user install:      $msi (send via Telegram/GitHub, NOT Gmail)"
