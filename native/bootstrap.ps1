# Downloads third-party dependencies needed to build the C++/WebView2 host:
#   - Microsoft.Web.WebView2 SDK (headers + static loader lib)
#   - nlohmann/json single-header
# Idempotent: skips anything already present.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tp   = Join-Path $root 'third_party'
New-Item -ItemType Directory -Force -Path $tp | Out-Null

# --- WebView2 SDK ------------------------------------------------------------
$wv2Dir = Join-Path $tp 'webview2'
$wv2Hdr = Join-Path $wv2Dir 'build/native/include/WebView2.h'
if (-not (Test-Path $wv2Hdr)) {
    Write-Host '[bootstrap] Resolving latest WebView2 SDK version...'
    $idx = Invoke-RestMethod 'https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/index.json'
    $ver = $idx.versions | Where-Object { $_ -notmatch '-' } | Select-Object -Last 1
    Write-Host "[bootstrap] Downloading Microsoft.Web.WebView2 $ver ..."
    $nupkg = Join-Path $tp "webview2.$ver.zip"
    Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$ver/microsoft.web.webview2.$ver.nupkg" -OutFile $nupkg
    if (Test-Path $wv2Dir) { Remove-Item -Recurse -Force $wv2Dir }
    Expand-Archive $nupkg -DestinationPath $wv2Dir
    Remove-Item $nupkg
    Write-Host '[bootstrap] WebView2 SDK ready.'
} else {
    Write-Host '[bootstrap] WebView2 SDK already present.'
}

# --- miniz (zip read/extract) ------------------------------------------------
$mzDir = Join-Path $tp 'miniz'
New-Item -ItemType Directory -Force -Path $mzDir | Out-Null
if (-not (Test-Path (Join-Path $mzDir 'miniz.c'))) {
    Write-Host '[bootstrap] Downloading miniz 3.0.2 ...'
    $mzZip = Join-Path $tp 'miniz.zip'
    Invoke-WebRequest 'https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip' -OutFile $mzZip
    $tmp = Join-Path $tp 'miniz_tmp'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive $mzZip -DestinationPath $tmp
    Get-ChildItem -Recurse $tmp -Include 'miniz.c','miniz.h' | ForEach-Object { Copy-Item $_.FullName $mzDir -Force }
    Remove-Item -Recurse -Force $tmp; Remove-Item $mzZip
    Write-Host '[bootstrap] miniz ready.'
} else {
    Write-Host '[bootstrap] miniz already present.'
}

# --- nlohmann/json -----------------------------------------------------------
$njDir = Join-Path $tp 'nlohmann'
New-Item -ItemType Directory -Force -Path $njDir | Out-Null
$jsonHpp = Join-Path $njDir 'json.hpp'
if (-not (Test-Path $jsonHpp)) {
    Write-Host '[bootstrap] Downloading nlohmann/json v3.11.3 ...'
    Invoke-WebRequest 'https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp' -OutFile $jsonHpp
} else {
    Write-Host '[bootstrap] nlohmann/json already present.'
}

Write-Host '[bootstrap] Done.'
