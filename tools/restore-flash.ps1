# Restore the board to the exact state it was in before esp32-iphone-agent was flashed.
#
#   pwsh -File tools\restore-flash.ps1
#
# Writes the full 16 MB backup back, including the previously verified
# anan-iphone-use (Anan / MCP) firmware and its persisted pairing identity.
param(
    [string]$Port = "COM4",
    [string]$Backup = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $Backup) {
    $Backup = Join-Path $repo "backups\esp32s3-before-esp32agent.bin"
}
if (-not (Test-Path $Backup)) {
    throw "backup not found: $Backup"
}

$env:TEMP = Join-Path $repo ".tmp"
$env:TMP = $env:TEMP
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null

$py = Join-Path $repo ".pio-core\penv\Scripts\python.exe"
if (-not (Test-Path $py)) { throw "project python not found: $py" }
$esptoolDir = Join-Path $repo ".pio-core\packages\tool-esptoolpy"
if (-not (Test-Path $esptoolDir)) { throw "esptool not found: $esptoolDir" }

$size = (Get-Item $Backup).Length
$hash = (Get-FileHash $Backup -Algorithm SHA256).Hash
Write-Host "backup : $Backup"
Write-Host "size   : $size bytes"
Write-Host "sha256 : $hash"
Write-Host ""
Write-Host "Writing $([math]::Round($size/1MB,2)) MB to $Port ..."
Write-Host "Do not unplug the board."

Push-Location $esptoolDir
& $py esptool.py --chip esp32s3 --port $Port --baud 460800 `
    --before default_reset --after hard_reset `
    write_flash --flash_size 16MB 0x0 $Backup
$code = $LASTEXITCODE
Pop-Location

if ($code -ne 0) { throw "esptool write_flash failed with exit code $code" }

Write-Host ""
Write-Host "Restore complete. The board should now boot the previous anan-iphone-use firmware."
