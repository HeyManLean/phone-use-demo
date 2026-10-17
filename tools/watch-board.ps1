# Live monitor for the ESP32 Agent board over BLE.
# Prints the fields that matter while you plug the board into the iPhone and mirror.
#
#   pwsh -File tools\watch-board.ps1
#
# Expect:
#   usb : off -> ok          (board enumerated by the iPhone as a HID mouse/keyboard)
#   ap  : idle -> connected -> streaming   (AirPlay mirroring established)
#   kbps/fps rise, drops stay 0
param(
    [int]$Seconds = 180
)

$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$env:TEMP = Join-Path $repo ".tmp"
$env:TMP = $env:TEMP
$env:UV_CACHE_DIR = Join-Path $repo ".uv-cache"
$env:PYTHONIOENCODING = "utf-8"
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null

$script = @'
import asyncio, json, sys, time
from bleak import BleakClient, BleakScanner

BASE = "6e400001-a9e8-4b1b-9c39-6d6f8a3c"
STATUS = BASE + "0003"
SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 180

async def main():
    d = await BleakScanner.find_device_by_filter(
        lambda dev, adv: (dev.name or "").startswith("ESP32 Agent"), timeout=15.0)
    if not d:
        print("board not found (is it powered?)", flush=True)
        return
    print("connected to %s\n" % d.name, flush=True)
    print("%-7s %-5s %-10s %-9s %-7s %-6s %-6s %-6s %s" %
          ("up(s)", "usb", "ap", "ip", "kbps", "fps", "drop", "heap", "note"), flush=True)
    print("-" * 78, flush=True)
    prev = None
    async with BleakClient(d, timeout=25.0) as c:
        end = time.time() + SECONDS
        while time.time() < end:
            try:
                v = await c.read_gatt_char(STATUS)
                s = json.loads(bytes(v).decode("utf-8", "replace"))
            except Exception as e:
                print("read failed: %s" % type(e).__name__, flush=True)
                await asyncio.sleep(2)
                continue
            b = s.get("ble", {})
            note = ""
            if s["usb"] == "ok":
                note += "USB-HID-OK "
            if s["ap"] == "streaming":
                note += "MIRRORING "
            if b.get("drop", 0) or b.get("rdrop", 0):
                note += "DROPS! "
            if s.get("psram", 0) == 0:
                note += "NO-PSRAM! "
            print("%-7d %-5s %-10s %-9s %-7d %-6.1f %-6d %-6d %s" %
                  (s["up"] // 1000, s["usb"], s["ap"], s["ip"],
                   b.get("kbps", 0), s.get("fps", 0.0), b.get("drop", 0),
                   s.get("heap", 0), note), flush=True)
            await asyncio.sleep(2)

asyncio.run(main())
'@

$tmp = Join-Path $env:TEMP "watch-board.py"
Set-Content -LiteralPath $tmp -Value $script -Encoding utf8
Push-Location $repo
uv run --with bleak python $tmp $Seconds
Pop-Location
