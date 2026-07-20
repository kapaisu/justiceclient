; Justice Launcher — clean per-user installer (run makensis from the native\ dir)
Unicode true
SetCompressor /SOLID lzma
!include "MUI2.nsh"
!include "FileFunc.nsh"

!define APPNAME    "Justice Launcher"
!define COMPANY    "Justice Client"
!define VERSION    "2.2.7"
!define EXENAME    "JusticeLauncher.exe"
!define REGUNINST  "Software\Microsoft\Windows\CurrentVersion\Uninstall\JusticeLauncher"

Name "${APPNAME}"
OutFile "..\dist-native\justice-launcher-setup.exe"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\Justice Launcher"
InstallDirRegKey HKCU "${REGUNINST}" "InstallLocation"
BrandingText "Justice Launcher ${VERSION}"
VIProductVersion "2.2.7.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "CompanyName" "${COMPANY}"
VIAddVersionKey "FileDescription" "Justice Launcher Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "© Justice Client"

; --- UI ----------------------------------------------------------------------
!define MUI_ICON   "..\build\icon.ico"
!define MUI_UNICON "..\build\icon.ico"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "..\build\installerHeader.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP   "..\build\installerSidebar.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "..\build\uninstallerSidebar.bmp"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXENAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Justice Launcher"
!define MUI_FINISHPAGE_LINK "justiceclient.org"
!define MUI_FINISHPAGE_LINK_LOCATION "https://justiceclient.org"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; --- kill running instances so files aren't locked ---------------------------
!macro KillRunning
  nsExec::Exec 'taskkill /F /IM "${EXENAME}"'
  nsExec::Exec 'taskkill /F /IM "Justice Launcher.exe"'
  nsExec::Exec 'taskkill /F /IM "justice-launcher.exe"'
  Sleep 1500
!macroend

Function .onInit
  !insertmacro KillRunning
FunctionEnd

; --- install -----------------------------------------------------------------
Section "Install"
  !insertmacro KillRunning

  Delete "$SMPROGRAMS\Games\Justice Launcher.lnk"
  Delete "$SMPROGRAMS\Games\Justice Client.lnk"
  Delete "$SMPROGRAMS\Justice Launcher.lnk"
  Delete "$SMPROGRAMS\Justice Client.lnk"
  Delete "$DESKTOP\Justice Client.lnk"
  RMDir "$SMPROGRAMS\Games"

  StrCpy $R5 0
  oldrescan:
    StrCpy $R0 0
    oldscan:
      EnumRegKey $R1 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall" $R0
      StrCmp $R1 "" oldscandone
      IntOp $R0 $R0 + 1
      StrCmp $R1 "JusticeLauncher" oldscan
      ReadRegStr $R2 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\$R1" "DisplayName"
      StrCmp $R2 "Justice Launcher" oldmatch 0
      StrCmp $R2 "Justice Client" oldmatch oldscan
      oldmatch:
        ReadRegStr $R3 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\$R1" "InstallLocation"
        StrCmp $R3 "$INSTDIR" oldkill
        ReadRegStr $R4 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\$R1" "QuietUninstallString"
        StrCmp $R4 "" 0 oldrun
          ReadRegStr $R4 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\$R1" "UninstallString"
          StrCmp $R4 "" oldkill
          StrCpy $R4 '$R4 /S'
        oldrun:
        ExecWait '$R4'
      oldkill:
        DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\$R1"
        IntOp $R5 $R5 + 1
        IntCmp $R5 6 oldscandone
        Goto oldrescan
    oldscandone:

  RMDir /r "$INSTDIR"
  SetOutPath "$INSTDIR"
  File /r "..\dist-native\Justice Launcher\*.*"
  File "/oname=Justice Launcher.exe" "..\dist-native\Justice Launcher\JusticeLauncher.exe"

  FileOpen $0 "$INSTDIR\version.txt" w
  FileWrite $0 "${VERSION}"
  FileClose $0

  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXENAME}" "" "$INSTDIR\assets\icon.ico"
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${EXENAME}" "" "$INSTDIR\assets\icon.ico"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr   HKCU "${REGUNINST}" "DisplayName"     "${APPNAME}"
  WriteRegStr   HKCU "${REGUNINST}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKCU "${REGUNINST}" "Publisher"       "${COMPANY}"
  WriteRegStr   HKCU "${REGUNINST}" "DisplayIcon"     "$INSTDIR\${EXENAME}"
  WriteRegStr   HKCU "${REGUNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKCU "${REGUNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${REGUNINST}" "URLInfoAbout"    "https://justiceclient.org"
  WriteRegDWORD HKCU "${REGUNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${REGUNINST}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${REGUNINST}" "EstimatedSize" "$0"

  ; On a silent in-app update (installer run with /S by the updater), relaunch the
  ; app automatically. The interactive installer uses the Finish page's Launch
  ; button instead, so only do this when silent.
  IfSilent 0 +2
    Exec '"$INSTDIR\${EXENAME}"'
SectionEnd

; --- uninstall ---------------------------------------------------------------
Section "Uninstall"
  nsExec::Exec 'taskkill /F /IM "${EXENAME}"'
  Sleep 1000
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "${REGUNINST}"
  ; app data (~/.justice-launcher) is intentionally kept, matching the old app.
SectionEnd
