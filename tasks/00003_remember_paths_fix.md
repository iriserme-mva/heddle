# Installer "remember‑paths" fix

**Status:** In progress  
**Target version:** 0.1.14  

**Description:** Update `installer/HeddleRememberPaths.vbs` to write the install directory to the user‑hive `HKCU\Software\Heddle\Remembered\InstallDir|Vst3Dir`. Recompile the MSI (`installer/heddle.msi.wxs`) so the custom action runs before `InstallFinalize`.  
**References:** STATUS.md entry 2026‑11‑22, lines 40–52.  
**Next steps:** Build MSI, test installer pre‑fill, then commit the `.wxs` changes.
