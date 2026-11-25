$ErrorActionPreference = "Stop"
Set-Location D:\Hermes\MV\projects\heddle
$log = "D:\Hermes\MV\projects\heddle\temp\runlog_0.1.14c.log"
Remove-Item $log -ErrorAction SilentlyContinue
Start-Transcript -Path $log | Out-Null

$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell 05b31e49 -Arch amd64 -SkipAutomaticLocation | Out-Null

$out = "D:\Hermes\MV\projects\heddle\temp\build_0114c_out.log"
Remove-Item $out -ErrorAction SilentlyContinue
& cmake --build build-msvc-perf2 --config Release --target heddle_proxy_plugin_VST3 -j *> $out
Write-Host "BUILD_EXIT=$LASTEXITCODE"
Write-Host "ERRCOUNT=$((Select-String -Path $out -Pattern 'error C' | Measure-Object).Count)"
Stop-Transcript | Out-Null
