# Builds the release exe, assembles the folder, and compiles the NSIS installer.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path   # native

& (Join-Path $root 'package.ps1')

$makensis = Join-Path $root 'third_party\nsis\nsis-3.10\makensis.exe'
if (-not (Test-Path $makensis)) { throw 'makensis not found' }

Push-Location $root
try { & $makensis '/V2' 'installer.nsi' } finally { Pop-Location }

$setup = Join-Path (Split-Path -Parent $root) 'dist-native\justice-launcher-setup.exe'
if (Test-Path $setup) {
    $mb = [math]::Round((Get-Item $setup).Length/1MB, 1)
    Write-Host "[installer] $setup  ($mb MB)" -ForegroundColor Green
} else { Write-Error 'installer not produced' }
