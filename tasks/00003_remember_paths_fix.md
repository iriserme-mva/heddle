# Installer "remember‑paths" fix

**Status:** In progress — the 0.1.14 remember-paths fix was correct for FRESH installs, but the user's 2026-11-24 field report ("version is set, the install path is however not preserved") exposed a sequencing bug: the write-side custom action was conditioned `NOT Installed`, which is FALSE on an in-place UPGRADE, so the CA never ran and `HKCU\Software\Heddle\Remembered` was never written. Fixed in `installer/heddle.msi.wxs` (condition → unconditional `1`, with a comment recording the lesson); ships in the 0.1.14b MSI (2026-11-24). Verify on the user's box: after the 0.1.14b upgrade, `~\Heddle\remember.log` must contain a new line and the next install dialog must pre-fill the remembered paths.  
**Target version:** 0.1.14  

**Description:** Update `installer/HeddleRememberPaths.vbs` to write the install directory to the user‑hive `HKCU\Software\Heddle\Remembered\InstallDir|Vst3Dir`. Recompile the MSI (`installer/heddle.msi.wxs`) so the custom action runs before `InstallFinalize`.  
**References:** STATUS.md entry 2026‑11‑22, lines 40–52.  
**Next steps:** Build MSI, test installer pre‑fill, then commit the `.wxs` changes.
