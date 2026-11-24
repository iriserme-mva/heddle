# Rebuild and repackage MSI for v0.1.14

**Status:** Planned  
**Target version:** 0.1.14  

**Description:** After the drag crash, version info, and remember‑paths fixes are merged, run a clean CMake configuration, rebuild host/render/proxy executables and the bridge VST3, then package a new MSI using `installer/heddle.msi.wxs`. Verify the MSI installs correctly and send the artifact via email/Telegram.  
**References:** STATUS.md entry 2026‑11‑54‑55.  
**Next steps:** Schedule a CI job to generate the MSI, then deliver to the user.
