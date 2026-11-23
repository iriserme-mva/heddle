' Heddle MSI remember-paths custom action (deferred, user context).
'
' WHY: the installer remembers the last InstallDir + VST3Dir across reinstalls
' by reading HKCU\Software\Heddle\Remembered (RegistrySearch -> PREV_* ->
' SetDirectory). The 0.1.14 pre-release remembered the paths as rows owned by
' the cmpMenu component under HKCU\Software\Heddle, so the uninstall REMOVED
' them and every reinstall fell back to the Program Files defaults. This key
' is owned by no MSI component, so uninstall never deletes it.
'
' Sequenced as IMMEDIATE just before InstallFinalize in the
' InstallExecuteSequence (WiX ICE77: VBScript custom actions must be immediate
' and sit between InstallInitialize and InstallFinalize). Immediate => it runs
' in the installer UI process, i.e. the installing user's context, so HKCU is
' the installing user's hive. By that point the SetDirectory pre-fill and any
' dialog edits have committed to INSTALLDIR / VST3DIR, so the values written
' are final. Writes the final INSTALLDIR / VST3DIR target paths and appends
' one line to the remember.log field-forensics trace (in the user's home
' Heddle folder), so a remember failure is visible in the field instead of
' silent.
Option Explicit

Function HeddleRememberPaths()
  On Error Resume Next
  Dim sId, sVst, sh, fso
  sId  = Session.Property("INSTALLDIR")
  sVst = Session.Property("VST3DIR")

  Set sh = CreateObject("WScript.Shell")
  If Len(sId) > 0 Then
    Call SetHkuString(sh, "Software\Heddle\Remembered", "InstallDir", sId)
  End If
  If Len(sVst) > 0 Then
    Call SetHkuString(sh, "Software\Heddle\Remembered", "Vst3Dir", sVst)
  End If

  ' Field-forensics trace (best effort -- never fails the install).
  Dim home, lf, ts
  home = sh.ExpandEnvironmentStrings("%USERPROFILE%")
  lf = home & "\Heddle\remember.log"
  Set fso = CreateObject("Scripting.FileSystemObject")
  If Not fso.FolderExists(home & "\Heddle") Then fso.CreateFolder home & "\Heddle"
  ts = FormatDateTime(Now, 5)
  fso.OpenTextFile(lf, 8, True).WriteLine ts & "  remember InstallDir=" & sId & " Vst3Dir=" & sVst
  If Err.Number <> 0 Then Err.Clear

  HeddleRememberPaths = 0
End Function

Sub SetHkuString(sh, keyPath, valueName, value)
  Dim key
  Set key = sh.RegRead("HKCU\" & keyPath & "\" & valueName)
  ' RegRead throws (and leaves Err set) if the value does not exist yet.
  If Err.Number <> 0 Then
    sh.RegWrite "HKCU\" & keyPath & "\" & valueName, value, "REG_SZ"
    Err.Clear
  Else
    sh.RegWrite "HKCU\" & keyPath & "\" & valueName, value, "REG_SZ"
  End If
End Sub
