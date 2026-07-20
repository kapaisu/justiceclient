# Justice Launcher — Source (v2.2.7)

The native C++/WebView2 launcher for the Justice Client.

## Layout

```
native/     C++ source (native/src), CMake config, resource script, NSIS installer, build scripts
web/        WebView2 frontend (HTML/JS/CSS) served to the launcher UI
assets/     App icons referenced by the resource script
build/      Installer artwork (icon, header/sidebar bitmaps) used by the NSIS script
```

## Prerequisites

- Windows 10/11 (x64)
- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.20+
- PowerShell

## Build

From the `native/` directory:

```powershell
.\bootstrap.ps1        # fetches the WebView2 SDK, nlohmann/json, miniz, and NSIS
.\build.ps1            # builds Release\JusticeLauncher.exe
.\build-installer.ps1  # builds the exe, assembles the dist folder, and compiles the installer
```

`build-installer.ps1` produces `dist-native\justice-launcher-setup.exe`.

## Notes

- `bootstrap.ps1` downloads third-party dependencies into `native\third_party\`; they are not
  included here.
- The bundled Justice mod jars are distributed separately and are not part of this source drop;
  the launcher builds and runs without them.
