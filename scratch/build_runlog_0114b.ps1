$ErrorActionPreference = "Stop"
Set-Location D:\Hermes\MV\projects\heddle
$log = "D:\Hermes\MV\projects\heddle\temp\runlog_0.1.14b.log"
Start-Transcript -Path $log -Append | Out-Null

$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell 05b31e49 -Arch amd64 -SkipAutomaticLocation | Out-Null

# Reconfigure so CMake re-emits JuceHeader (macro JucePlugin_VersionString already present)
& cmake -S . -B build-msvc-perf2 -G "Visual Studio 17 2022" -A x64 -DHEDDLE_BUILD_HOST=ON
Write-Host "CONFIGURE_EXIT=$LASTEXITCODE"

& cmake --build build-msvc-perf2 --config Release `
  --target heddle_bridge_plugin_VST3 heddle_proxy_plugin_VST3 -j
Write-Host "BUILD_EXIT=$LASTEXITCODE"

$errCount = (Select-String -Path $log -Pattern 'error C|error LNK|error MSB' | Measure-Object).Count
Write-Host "ERROR_LINES=$errCount"
Stop-Transcript | Out-Null
