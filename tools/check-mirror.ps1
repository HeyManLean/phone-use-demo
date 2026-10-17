# Diagnostic for the AirPlay -> BLE mirroring step.
#
#   pwsh -File tools\check-mirror.ps1
#
# Run this while you perform: Control Center -> Screen Mirroring -> "ESP32 Agent".
# It samples the board's state every 2 s and reports which stage was reached, with the
# meaning of each failure, so a silent "nothing happened" becomes a specific answer.
param(
    [int]$Seconds = 240
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
SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 240

# airplay_state() -> apStates[] in hid_bridge.ino
AP_MEANING = {
    "idle":      "no mirroring negotiated yet",
    "connected": "handshake started but no video frames arriving",
    "streaming": "video is flowing",
}

async def main():
    d = await BleakScanner.find_device_by_filter(
        lambda dev, adv: (dev.name or "").startswith("ESP32 Agent"), timeout=20.0)
    if not d:
        print("BOARD NOT FOUND -- is it still powered and in BLE range?")
        return
    async with BleakClient(d, timeout=25.0) as c:
        print("board: %s" % d.name)
        print("Now do: Control Center -> Screen Mirroring -> 'ESP32 Agent'")
        print("Sampling %ds, printing only changes.\n" % SECONDS)

        start = None
        last = None
        saw_idr = False
        end = time.time() + SECONDS
        while time.time() < end:
            try:
                s = json.loads(bytes(await c.read_gatt_char(STATUS)).decode("utf-8", "replace"))
            except Exception as e:
                print("  read error:", type(e).__name__, flush=True)
                await asyncio.sleep(2)
                continue
            b = s.get("ble", {})
            key = (s["ap"], s["usb"])
            if key != last:
                t = time.strftime("%H:%M:%S")
                print("[%s] ap=%s  usb=%s  (%s)" %
                      (t, s["ap"], s["usb"], AP_MEANING.get(s["ap"], "?")), flush=True)
                last = key
                if start is None:
                    start = time.time()
            if s["ap"] != "idle" or b.get("kbps", 0) > 0:
                if s.get("idr", 0) and not saw_idr:
                    saw_idr = True
                    print("    first IDR after %d ms -> decodable frame received" % s["idr"], flush=True)
                print("    %dx%d fps=%.1f kbps=%d idr=%dms gop=%dB mtu=%d drop=%d rdrop=%d" %
                      (s["w"], s["h"], s.get("fps", 0.0), b.get("kbps", 0),
                       s.get("idr", 0), s.get("gop", 0), b.get("mtu", 0),
                       b.get("drop", 0), b.get("rdrop", 0)), flush=True)
            await asyncio.sleep(2)

        # Final verdict.
        try:
            s = json.loads(bytes(await c.read_gatt_char(STATUS)).decode("utf-8", "replace"))
        except Exception:
            s = None
        print("\n" + "=" * 62)
        if s is None:
            print("VERDICT: could not read final state")
        elif s["ap"] == "streaming" and s.get("idr", 0) > 0:
            print("VERDICT: MIRRORING WORKS")
            print("  resolution %dx%d at %.1f fps, %d kbps, first IDR %d ms" %
                  (s["w"], s["h"], s.get("fps", 0.0), s.get("ble", {}).get("kbps", 0), s["idr"]))
        elif s["ap"] == "streaming":
            print("VERDICT: streaming but NO IDR yet")
            print("  AirPlay is connected and sending, but no keyframe has arrived.")
            print("  Try switching apps or scrolling on the phone to force an IDR.")
        elif s["ap"] == "connected":
            print("VERDICT: handshake reached 'connected' but no frames")
            print("  The RTSP/FairPlay handshake passed, so the board is reachable.")
            print("  Video did not start -- check the phone is unlocked and not showing")
            print("  protected content (passwords / Apple Pay / DRM).")
        else:
            print("VERDICT: still 'idle' -- the board was never selected")
            print("  Checks, in order:")
            print("   1. Phone joined the board hotspot (ESP32Agent-XXXX), not home Wi-Fi.")
            print("   2. Control Center -> Screen Mirroring lists 'ESP32 Agent'.")
            print("   3. Phone is unlocked; mirroring cannot start on a locked screen.")
            print("   4. Board uptime is sane (not rebooting from a brownout).")
        print("=" * 62)

asyncio.run(main())
'@

$tmp = Join-Path $env:TEMP "check-mirror.py"
Set-Content -LiteralPath $tmp -Value $script -Encoding utf8
Push-Location $repo
uv run --with bleak python $tmp $Seconds
Pop-Location
