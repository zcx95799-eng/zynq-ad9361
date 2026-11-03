param(
    [string]$Xsct = 'F:\vivado2\SDK\2019.1\bin\xsct.bat',
    [string]$Server = 'tcp:127.0.0.1:3121'
)
$ErrorActionPreference = 'Stop'
$boardDir = Split-Path -Parent $PSScriptRoot
if ($boardDir -match '[^\x00-\x7F]') {
    throw 'Xilinx 2019.1 has path encoding limitations. Extract board to an ASCII-only path such as D:\board, then run again.'
}
if (!(Test-Path -LiteralPath $Xsct)) { throw 'XSCT not found. Set -Xsct to your SDK 2019.1 xsct.bat.' }
$runDir = Join-Path $boardDir ('run_outputs\' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '_download')
New-Item -ItemType Directory -Path $runDir | Out-Null
Push-Location $boardDir
try {
    & $Xsct (Join-Path $PSScriptRoot 'download_run.tcl') $Server 2>&1 | Tee-Object -FilePath (Join-Path $runDir 'xsct.log')
    if ($LASTEXITCODE -ne 0) { throw 'Download/run failed. See the XSCT log.' }
} finally {
    Pop-Location
}
