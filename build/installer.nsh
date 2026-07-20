; Justice Launcher - Custom NSIS Installer Script
; Included by electron-builder during build.
; Only contains supported custom macros.

BrandingText "Justice Launcher Installer"

; ---------------------------------------------------------------------------
; Kill every running Justice Launcher / Justice Client instance.
; If the app is still running, the old-version uninstaller can't delete the
; locked .exe and the install fails with:
;   "Failed to uninstall old application files. Please try running the
;    installer again.: 2"
; Kill by image name (NOT /T) so a running game (java.exe) is left alone,
; then wait for Windows to release the file handles.
; ---------------------------------------------------------------------------
!macro justiceKillRunning
  nsExec::Exec 'taskkill /F /IM "${PRODUCT_FILENAME}.exe"'
  Pop $0
  nsExec::Exec 'taskkill /F /IM "Justice Launcher.exe"'
  Pop $0
  nsExec::Exec 'taskkill /F /IM "justice-launcher.exe"'
  Pop $0
  nsExec::Exec 'taskkill /F /IM "Justice Client.exe"'
  Pop $0
  Sleep 2500
!macroend

; Runs at installer init (before the old version's uninstaller is invoked)
!macro customInit
  !insertmacro justiceKillRunning
!macroend

; Runs at uninstaller init (this is the process that actually deletes the old
; files during an update — make sure the app is dead here too)
!macro customUnInit
  !insertmacro justiceKillRunning
!macroend

; Runs after files are extracted
!macro customInstall
  ; Write installed version for local reference
  FileOpen $0 "$INSTDIR\version.txt" w
  FileWrite $0 "${VERSION}"
  FileClose $0
!macroend

; Runs during uninstall
!macro customUnInstall
  Delete "$INSTDIR\version.txt"
!macroend
