@echo off
setlocal
set BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set JUCE_SRC=D:\Hermes\MV\projects\heddle\build-msvc-perf2\_deps\juce-src

call "%BT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -nologo >nul 2>&1
if errorlevel 1 (
  echo VSVARCMD_FAILED
  call "%BT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)

where cl >nul 2>&1
if errorlevel 1 (
  echo CL_NOT_FOUND
  goto :end
)

cmake -S D:\Hermes\MV\projects\heddle -B D:\Hermes\MV\projects\heddle\build-repro ^
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEDDLE_BUILD_HOST=ON ^
  -DFETCHCONTENT_SOURCE_DIR_JUCE=%JUCE_SRC% ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  > D:\Hermes\MV\projects\heddle\logs\repro_cfg.log 2>&1
if errorlevel 1 (
  echo CONFIG_FAILED
  goto :end
)

cmake --build D:\Hermes\MV\projects\heddle\build-repro --target heddle_repro -j 8 ^
  > D:\Hermes\MV\projects\heddle\logs\repro_build.log 2>&1
if errorlevel 1 (
  echo BUILD_FAILED
  goto :end
)

echo BUILD_OK
for %%f in (D:\Hermes\MV\projects\heddle\build-repro\host\Release\heddle_repro.exe) do (
  if exist "%%f" echo FOUND:%%f
)
:end
endlocal
