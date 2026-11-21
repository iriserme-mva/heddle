$ErrorActionPreference = "Continue"
$BT = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$vcvars = "$BT\VC\Auxiliary\Build\vcvars64.bat"

# Import MSVC x64 env
$envOut = cmd /c "call `"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envOut) {
  if ($line -match '^([^=]+)=(.*)$') {
    Set-Item -Path "env:$($matches[1])" -Value $matches[2]
  }
}
$cl = Get-Command cl -ErrorAction SilentlyContinue
Write-Output "CL: $($cl.Source)"
$cmk = Get-Command cmake -ErrorAction SilentlyContinue
Write-Output "CMAKE: $($cmk.Source)"

$src = "D:\Hermes\MV\projects\heddle"
$builddir = "D:\Hermes\MV\projects\heddle\build-repro"
$juce = "D:\Hermes\MV\projects\heddle\build-msvc-perf2\_deps\juce-src"

& cmake -S $src -B $builddir -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DHEDDLE_BUILD_HOST=ON `
  -DFETCHCONTENT_SOURCE_DIR_JUCE=$juce `
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl `
  2>&1 | Tee-Object -FilePath "$src\logs\repro_cfg.log"
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIG_FAILED"; exit 1 }

& cmake --build $builddir --target heddle_repro -j 8 `
  2>&1 | Tee-Object -FilePath "$src\logs\repro_build.log"
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD_FAILED"; exit 1 }

$exe = Get-ChildItem -Recurse -Path $builddir -Filter heddle_repro.exe -ErrorAction SilentlyContinue | Select-Object -First 1
Write-Output "BUILD_OK EXE: $($exe.FullName)"
