# Configure + build the C++/WebView2 host. Runs bootstrap first.
param([switch]$Debug)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

& (Join-Path $root 'bootstrap.ps1')

$src = (Resolve-Path (Join-Path $root '..\web')).Path -replace '\\', '/'
$build = Join-Path $root 'build'
$config = if ($Debug) { 'Debug' } else { 'Release' }

cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64 "-DJUSTICE_SRC_DIR=$src"
cmake --build $build --config $config

$exe = Join-Path $build "$config\JusticeLauncher.exe"
if (Test-Path $exe) { Write-Host "[build] OK -> $exe" -ForegroundColor Green }
else { Write-Error "[build] Executable not produced." }
