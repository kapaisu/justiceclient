# Build if needed, then launch. Pass --dev to enable devtools.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root 'build\Release\JusticeLauncher.exe'
if (-not (Test-Path $exe)) { & (Join-Path $root 'build.ps1') }
& $exe @args
