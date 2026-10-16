#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Heddle — one-shot FULL local build on Windows (both layers) + all checks.

  core:  MSYS2 ucrt64 g++ (Ninja) -> heddle_core static lib + ctest suite
  host:  MSVC (VS 2022 BuildTools) -> HeddleSmokePlugin.vst3 + heddle_smoke
         + heddle-render + heddle-host, then smoke / render / host self-tests

  Usage (from anywhere):
    pwsh D:\path\to\heddle\scripts\build-all.ps1 [-SkipCore] [-SkipHost] [-NoTests]

  Requirements:
    * MSYS2 at C:\msys64 with ucrt64 (cmake, ninja, g++)
    * Visual Studio 2022 BuildTools (instance 05b31e49, amd64)
    * python 3 on PATH (render self-test generates/verifies a WAV)
#>
param(
  [switch]$SkipCore,
  [switch]$SkipHost,
  [switch]$NoTests
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Write-Host "heddle build-all: root = $root" -ForegroundColor Cyan

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

# ============================================================ core (MSYS2)
if (-not $SkipCore) {
  $msys = "C:\msys64"
  if (-not (Test-Path "$msys\ucrt64\bin\cmake.exe")) {
    throw "MSYS2 ucrt64 cmake not found: $msys\ucrt64\bin\cmake.exe"
  }
  $env:PATH = "$msys\ucrt64\bin;$env:PATH"

  Step "core: configure (MSYS2 ucrt64, Ninja)"
  & cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  if ($LASTEXITCODE) { throw "core configure failed ($LASTEXITCODE)" }

  Step "core: build"
  & cmake --build build -j
  if ($LASTEXITCODE) { throw "core build failed ($LASTEXITCODE)" }

  if (-not $NoTests) {
    Step "core: tests (ctest)"
    Push-Location build
    & ctest --output-on-failure
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc) { throw "core tests failed ($rc)" }
  }
}

# ============================================================ host (MSVC)
if (-not $SkipHost) {
  Step "host: enter MSVC dev shell (VS 2022 BuildTools, amd64)"
  $vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
  $devShell = "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
  if (-not (Test-Path $devShell)) { throw "VS DevShell module not found: $devShell" }
  Import-Module $devShell
  Enter-VsDevShell 05b31e49 -Arch amd64 -SkipAutomaticLocation | Out-Null

  Step "host: configure (VS 2022 x64, HEDDLE_BUILD_HOST=ON)"
  & cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 -DHEDDLE_BUILD_HOST=ON
  if ($LASTEXITCODE) { throw "host configure failed ($LASTEXITCODE)" }

  Step "host: build (smoke plugin + smoke CLI + render + host)"
  & cmake --build build-msvc --config Release `
    --target heddle_smoke heddle_smoke_plugin_VST3 heddle-render heddle-host
  if ($LASTEXITCODE) { throw "host build failed ($LASTEXITCODE)" }

  if (-not $NoTests) {
    $rel  = "build-msvc\host\Release"
    $vst3 = "build-msvc\host\test_plugin\heddle_smoke_plugin_artefacts\Release\VST3\HeddleSmokePlugin.vst3"
    foreach ($b in "heddle_smoke.exe", "heddle-render.exe", "heddle-host.exe") {
      if (-not (Test-Path "$rel\$b")) { throw "missing binary: $rel\$b" }
    }
    if (-not (Test-Path $vst3)) { throw "missing plugin bundle: $vst3" }

    $tmp = Join-Path $env:TEMP "heddle_build_selftest"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $vst3f = $vst3 -replace '\\', '/'   # JSON-safe forward slashes

    Step "host: smoke (load plugin, dump params)"
    $smokeText = (& "$rel\heddle_smoke.exe" $vst3) -join " "
    if ($smokeText -notmatch '"Gain"' -or $smokeText -notmatch 'PWM pct') {
      throw "smoke output missing Gain/'PWM pct': $smokeText"
    }
    Write-Host "smoke OK (Gain + PWM pct present)"

    Step "host: render self-test (deterministic gain ratio)"
    # 2048-frame stereo 44.1 kHz constant 0.5 tone; config Gain=0.5,
    # "PWM pct"=0.625 -> expected mean gain 0.3125 (same assert as CI).
    $genPy = @'
import json, struct, sys
plugin, tmp = sys.argv[1], sys.argv[2]
RATE, CH, N, BITS = 44100, 2, 2048, 16
val = int(round(0.5 * 32768))
data = b"".join(struct.pack("<h", val) for _ in range(N * CH))
byte_rate = RATE * CH * (BITS // 2)
with open(tmp + r"\in.wav", "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
    f.write(b"fmt " + struct.pack("<I", 16))
    f.write(struct.pack("<HHIIHH", 1, CH, RATE, byte_rate, CH * (BITS // 2), BITS))
    f.write(b"data" + struct.pack("<I", len(data)) + data)
cfg = {"sample_rate": RATE, "block_size": 512,
       "chain": [{"id": "gain", "plugin": plugin, "enabled": True,
                  "params": {"Gain": 0.5, "PWM pct": 0.625}}]}
with open(tmp + r"\render_chain.json", "w") as f:
    json.dump(cfg, f)
print("generated in.wav + render_chain.json in", tmp)
'@
    Set-Content -Path "$tmp\gen.py" -Value $genPy -Encoding utf8
    python "$tmp\gen.py" $vst3f $tmp
    if ($LASTEXITCODE) { throw "render test input generation failed ($LASTEXITCODE)" }

    & "$rel\heddle-render.exe" "$tmp\in.wav" "$tmp\render_chain.json" "$tmp\out.wav" --bits 16
    if ($LASTEXITCODE) { throw "heddle-render self-test failed ($LASTEXITCODE)" }

    $verifyPy = @'
import struct, sys
path = sys.argv[1]
raw = open(path, "rb").read()
assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE", "not a wav"
pos, fmt, data = 12, None, None
while pos + 8 <= len(raw):
    cid = raw[pos:pos + 4]
    size = struct.unpack("<I", raw[pos + 4:pos + 8])[0]
    body = raw[pos + 8:pos + 8 + size]
    if cid == b"fmt ":
        fmt = body
    elif cid == b"data":
        data = body
        break
    pos += 8 + size + (size & 1)
assert fmt is not None and data is not None, "missing wav chunks"
ch = struct.unpack("<H", fmt[2:4])[0]
n = len(data) // 2
s = struct.unpack("<%dh" % n, data)
gain = (sum(s) / len(s)) / 16384.0
print("mean gain %.4f" % gain)
assert abs(gain - 0.3125) < 0.01, "gain %f != 0.3125" % gain
print("render verify OK")
'@
    Set-Content -Path "$tmp\verify.py" -Value $verifyPy -Encoding utf8
    python "$tmp\verify.py" "$tmp\out.wav"
    if ($LASTEXITCODE) { throw "render verify failed ($LASTEXITCODE)" }
    Write-Host "render OK (mean gain 0.3125)"

    Step "host: heddle-host headless self-test (3 s)"
    $hostCfg = @"
{
  "sample_rate": 44100,
  "block_size": 512,
  "osc": {"port": 9123, "bind_host": "127.0.0.1"},
  "chain": [
    {
      "id": "smoke",
      "plugin": "$vst3f",
      "enabled": true,
      "params": {"Gain": 0.5, "PWM pct": 0.625}
    }
  ]
}
"@
    Set-Content -Path "$tmp\host_cfg.json" -Value $hostCfg -Encoding utf8
    & "$rel\heddle-host.exe" "$tmp\host_cfg.json" --audio null --duration 3
    if ($LASTEXITCODE) { throw "heddle-host self-test failed ($LASTEXITCODE)" }
    Write-Host "heddle-host OK (clean 3 s headless run)"
  }
}

Write-Host "`nBUILD-ALL COMPLETE" -ForegroundColor Green
