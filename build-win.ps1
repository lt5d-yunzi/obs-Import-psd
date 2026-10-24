# SPDX-License-Identifier: GPL-2.0-or-later
#
# One-command Windows x64 build for the obs-psd plugin.
#
#   powershell -ExecutionPolicy Bypass -File build-win.ps1
#   powershell -ExecutionPolicy Bypass -File build-win.ps1 -Install
#   powershell -ExecutionPolicy Bypass -File build-win.ps1 -Install -SelfTest
#
# The macOS build stays on ./build.sh; the platform choice is the script you
# run (this one only makes sense on Windows).
param(
  [string]$ObsDir = 'D:\64\obs-studio',
  [string]$QtRoot = '',
  [string]$ObsHeaders = '',
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
  [string]$Configuration = 'Release',
  [switch]$Install,
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root
try {
  $tp = Join-Path $root 'third_party'

  if (-not (Test-Path (Join-Path $tp 'obs-src\libobs\obs.h'))) {
    throw @"
Missing OBS headers.  Fetch the pinned OBS source (public headers only):

  git clone --depth 1 --branch 32.2.2 https://github.com/obsproject/obs-studio third_party/obs-src
"@
  }
  if (-not (Test-Path (Join-Path $tp 'qt6\include'))) {
    throw @"
Missing Qt 6 SDK.  OBS 32.2.2 is built with obs-deps 2026-07-15; download

  https://github.com/obsproject/obs-deps/releases/download/2026-07-15/windows-deps-qt6-2026-07-15-x64.zip

and extract it so that third_party\qt6\include, third_party\qt6\lib and
third_party\qt6\bin exist.  Or pass -QtRoot <Qt 6 msvc2022_64 prefix>.
"@
  }
  if (-not (Test-Path (Join-Path $tp 'miniz\miniz.c'))) {
    throw @"
Missing third_party\miniz and third_party\stb_image_write.h.

  git clone --depth 1 https://github.com/richgel999/miniz third_party\miniz-src
  (copy miniz.c/.h, miniz_tdef.*, miniz_tinfl.*, miniz_zip.*, miniz_common.h into third_party\miniz)
  third_party\stb_image_write.h: https://github.com/nothings/stb/blob/master/stb_image_write.h
"@
  }

  if (-not (Test-Path (Join-Path $tp 'obs-libs\x64\obs.lib'))) {
    Write-Output '== generating OBS import libraries =='
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'cmake\make-import-libs.ps1') -ObsBin (Join-Path $ObsDir 'bin\64bit')
    if ($LASTEXITCODE -ne 0) { throw 'make-import-libs.ps1 failed' }
  }

  $buildDir = Join-Path $root 'build-win'
  $configure = @('-S', $root, '-B', $buildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DOBS_DIR=$($ObsDir -replace '\\','/')")
  if ($QtRoot) { $configure += "-DQT_ROOT=$($QtRoot -replace '\\','/')" }
  if ($ObsHeaders) { $configure += "-DOBS_HEADERS=$($ObsHeaders -replace '\\','/')" }
  if ($SelfTest) { $configure += '-DOBS_PSD_SELFTEST=ON' } else { $configure += '-DOBS_PSD_SELFTEST=OFF' }

  Write-Output '== configure =='
  & cmake @configure
  if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

  Write-Output '== build =='
  & cmake --build $buildDir --config $Configuration -- /m /nologo
  if ($LASTEXITCODE -ne 0) { throw 'build failed' }

  $dll = Join-Path $buildDir "$Configuration\obs-psd.dll"
  Write-Output "built: $dll"

  if ($Install) {
    $pluginDir = Join-Path $ObsDir 'obs-plugins\64bit'
    $dataDir = Join-Path $ObsDir 'data\obs-plugins\obs-psd\locale'
    New-Item -ItemType Directory -Force $pluginDir, $dataDir | Out-Null
    Copy-Item $dll (Join-Path $pluginDir 'obs-psd.dll') -Force
    Copy-Item (Join-Path $root 'data\locale\*.ini') $dataDir -Force
    Write-Output "installed into $ObsDir"
  }
} finally {
  Pop-Location
}
