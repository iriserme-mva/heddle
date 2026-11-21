param([string]$Target = "heddle_proxy_plugin_VST3")
$ErrorActionPreference = "Stop"
$root = "D:\Hermes\MV\projects\heddle"
Set-Location $root
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell 05b31e49 -Arch amd64 -SkipAutomaticLocation | Out-Null
& cmake --build build-msvc-perf2 --config Release --target $Target -j 2>&1
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
Write-Host "BUILD OK"
