

BrandingText "Justice Launcher Installer"

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


!macro customInit
  !insertmacro justiceKillRunning
!macroend


!macro customUnInit
  !insertmacro justiceKillRunning
!macroend


!macro customInstall

  FileOpen $0 "$INSTDIR\version.txt" w
  FileWrite $0 "${VERSION}"
  FileClose $0
!macroend

!macro customUnInstall
  Delete "$INSTDIR\version.txt"
!macroend
