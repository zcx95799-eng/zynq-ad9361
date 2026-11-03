param([string]$QtBin = 'E:\QTapp\6.10.0\mingw_64\bin', [string]$MinGWBin = 'E:\QTapp\Tools\mingw1310_64\bin')
$ErrorActionPreference = 'Stop'
$boardDir = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $boardDir 'artifacts\qt\tcp.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'Qt executable not found. Run build_qt.ps1 and use build\qt\release\tcp.exe.' }
$env:PATH = "$QtBin;$MinGWBin;$env:PATH"
Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe)
