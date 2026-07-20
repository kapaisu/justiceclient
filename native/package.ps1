# Builds a release exe and assembles a self-contained distributable folder:
#   <Code>\dist-native\Justice Launcher\
#     JusticeLauncher.exe   (static CRT, embedded icon)
#     src\  assets\  bundled-mods\
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path      # native\
$code = Split-Path -Parent $root                              # Code\

& (Join-Path $root 'build.ps1')
$exe = Join-Path $root 'build\Release\JusticeLauncher.exe'
if (-not (Test-Path $exe)) { throw 'Build did not produce JusticeLauncher.exe' }

$out = Join-Path $code 'dist-native\Justice Launcher'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null

Copy-Item $exe (Join-Path $out 'JusticeLauncher.exe')
Copy-Item (Join-Path $code 'web')          (Join-Path $out 'src')          -Recurse
Copy-Item (Join-Path $code 'assets')       (Join-Path $out 'assets')       -Recurse
Copy-Item (Join-Path $code 'bundled-mods') (Join-Path $out 'bundled-mods') -Recurse
# Drop editor backups from the package.
Get-ChildItem $out -Recurse -Include *.bak | Remove-Item -Force -ErrorAction SilentlyContinue

$size = [math]::Round(((Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host ""
Write-Host "[package] Done -> $out" -ForegroundColor Green
Write-Host "[package] Folder size: $size MB"
Write-Host "[package] Run: `"$out\JusticeLauncher.exe`""
