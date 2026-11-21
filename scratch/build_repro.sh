# Configure + build ONLY heddle_repro (JUCE host modules) with MSVC BuildTools 2022.
set -e
export PATH="/c/msys64/ucrt64/bin:$PATH"
BT="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
"$BT/Common7/Tools/VsDevCmd.bat" -arch=x64 -host_arch=x64 > /dev/null 2>&1 || {
  # fallback: vcvars64
  "$BT/VC/Auxiliary/Build/vcvars64.bat" > /dev/null 2>&1
}
cmake -S /d/Hermes/MV/projects/heddle -B /d/Hermes/MV/projects/heddle/build-repro \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEDDLE_BUILD_HOST=ON \
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl > /d/Hermes/MV/projects/heddle/logs/repro_cfg.log 2>&1
cmake --build /d/Hermes/MV/projects/heddle/build-repro --target heddle_repro -j 8 \
  > /d/Hermes/MV/projects/heddle/logs/repro_build.log 2>&1
echo "BUILD_DONE rc=$?"
ls -la /d/Hermes/MV/projects/heddle/build-repro/host/Release/heddle_repro.exe 2>/dev/null || \
  find /d/Hermes/MV/projects/heddle/build-repro -name "heddle_repro.exe" 2>/dev/null
