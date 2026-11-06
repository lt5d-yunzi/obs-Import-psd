# Generates the MSVC import libraries (obs.lib, obs-frontend-api.lib) from the
# DLLs of an installed OBS, so the plugin can link against libobs without a full
# OBS source build.
#
#   powershell -ExecutionPolicy Bypass -File cmake/make-import-libs.ps1
#   powershell -ExecutionPolicy Bypass -File cmake/make-import-libs.ps1 `
#       -ObsBin 'C:\Program Files\obs-studio\bin\64bit'
param(
  [string]$ObsBin = 'D:\64\obs-studio\bin\64bit',
  [string]$OutDir = (Join-Path $PSScriptRoot '..\third_party\obs-libs\x64')
)

$ErrorActionPreference = 'Stop'

# Locate dumpbin.exe / lib.exe from the newest VS 2022+ installation.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio C++ tools" }
$vsPath = & $vswhere -products * -latest -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation found" }

$dumpbin = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
  Where-Object FullName -match 'Hostx64\\x64' | Select-Object -First 1 -ExpandProperty FullName
$libexe = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Recurse -Filter lib.exe -ErrorAction SilentlyContinue |
  Where-Object FullName -match 'Hostx64\\x64' | Select-Object -First 1 -ExpandProperty FullName
if (-not $dumpbin -or -not $libexe) { throw "dumpbin.exe/lib.exe not found under $vsPath" }

New-Item -ItemType Directory -Force $OutDir | Out-Null

foreach ($base in @('obs', 'obs-frontend-api')) {
  $dll = Join-Path $ObsBin "$base.dll"
  if (-not (Test-Path $dll)) { throw "missing $dll" }
  $lines = & $dumpbin /exports $dll 2>&1
  $names = foreach ($line in $lines) {
    if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') {
      $n = $Matches[1]
      # Skip the C++-mangled frontend internals: only the C API is linkable from
      # the plugin's extern "C" declarations.
      if ($n -match '^[A-Za-z_][A-Za-z0-9_]*$') { $n }
    }
  }
  $names = $names | Sort-Object -Unique
  $def = Join-Path $OutDir "$base.def"
  Set-Content -Path $def -Value (@("LIBRARY $base", 'EXPORTS') + $names) -Encoding ascii
  & $libexe "/def:$def" /machine:x64 "/out:$(Join-Path $OutDir "$base.lib")" | Out-Null
  Write-Output "$base : $($names.Count) exports -> $(Join-Path $OutDir "$base.lib")"
}

Write-Output "done"
