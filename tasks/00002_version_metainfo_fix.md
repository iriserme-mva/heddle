# Version metainfo fix for binaries

**Status:** In progress  
**Target version:** 0.1.14  

**Description:** Re‑run CMake configure and rebuild `heddle-host.exe`, `heddle-render.exe`, and `heddle-proxy.exe` so that `VS_VERSION_INFO` contains the 0.1.14 string. Commit `version.rc.in` changes after the build.  
**References:** STATUS.md entry 2026‑11‑22, lines 28‑39.  
**Next steps:** Verify version info with `dumpbin /headers`, then tag v0.1.14.
