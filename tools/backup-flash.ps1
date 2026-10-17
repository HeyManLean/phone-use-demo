# Back up the whole 16 MB flash of the board on COM4 before flashing anything new.
# Usage:  pwsh -File tools\backup-flash.ps1 [-Port COM4] [-OutPath <file>]
param(
    [string]$Port = "COM4",
    [string]$OutPath = ""
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $here

if (-not $OutPath) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $dir = Join-Path $repo "backups"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $OutPath = Join-Path $dir "esp32s3-full-$stamp.bin"
}

$python = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
if (-not (Test-Path $python)) { $python = "python" }

Write-Host "Reading 16 MB from $Port -> $OutPath"
Write-Host "Do not unplug the board."

# 16 MB = 0x1000000
& $python -m esptool --chip esp32s3 --port $Port --baud 460800 `
    --before default_reset --after hard_reset `
    read_flash 0 0x1000000 $OutPath

if ($LASTEXITCODE -ne 0) {
    Write-Error "esptool read_flash failed with exit code $LASTEXITCODE"
}

$f = Get-Item $OutPath
$hash = (Get-FileHash $OutPath -Algorithm SHA256).Hash
Write-Host ""
Write-Host "Backup complete."
Write-Host "  file   : $($f.FullName)"
Write-Host "  size   : $($f.Length) bytes (expected 16777216)"
Write-Host "  sha256 : $hash"
