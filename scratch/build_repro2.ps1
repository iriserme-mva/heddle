$ErrorActionPreference = "Continue"
$BT = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$vcvars = "$BT\VC\Auxiliary\Build\vcvars64.bat"

$envOut = cmd /c "call `"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envOut) {
  if ($line -match '^([^=]+)=(.*)$') {
    Set-Item -Path "env:$($matches[1])" -Value $matches[2]
  }
}
$cl = Get-Command cl -ErrorAction SilentlyContinue
Write-Output "CL: $($cl.Source)"

$src = "D:\Hermes\MV\projects\heddle"
$bd  = "D:\Hermes\MV\projects\heddle\build-msvc-perf2"

& cmake -S $src -B $bd > "$src\logs\repro_cfg2.log" 2>&1
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIG_FAILED"; exit 1 }
Write-Output "CONFIG_OK"

& cmake --build $bd --target heddle_repro --config Release -j 8 > "$src\logs\repro_build2.log" 2>&1
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD_FAILED"; exit 1 }
Write-Output "BUILD_OK"
$exe = Get-ChildItem -Recurse -Path $bd -Filter heddle_repro.exe -ErrorAction SilentlyContinue | Select-Object -First 1
Write-Output "EXE: $($exe.FullName)"
