# esp32-iphone-agent on the user's ESP32-S3 N16R8 — build and verification record

This directory is an **independent** checkout of
[onehumanbeing/esp32-iphone-agent](https://github.com/onehumanbeing/esp32-iphone-agent).
It does not modify or depend on the already-verified `anan-iphone-use` (Anan / MCP) project
or the separate `circle` project.

- Upstream revision: `9b1a6674248b2479d2265e9cff0f5da9e8ad7330`
- Target board: third-party ESP32-S3 DevKitC-1 style carrier, **ESP32-S3-WROOM-1 N16R8**
  (16 MB quad flash, 8 MB octal PSRAM), CH343 UART bridge, native USB on GPIO19/20

## Why a separate build config

Upstream ships `hardware/flash.sh`, which needs `arduino-cli` and a bash environment, and
targets the Seeed XIAO ESP32S3 with `FlashSize=8M`. On this machine ESP-IDF and PlatformIO
were already installed, so `platformio.ini` was added to build the **unmodified upstream
sources** with an equivalent toolchain. Nothing in `hardware/` or `ios/` was changed.

Versions deliberately match upstream CI (`.github/workflows/ci.yml:28-31`):

| Component | Version |
|---|---|
| Arduino-ESP32 core | 3.3.12 |
| NimBLE-Arduino | 2.5.1 |
| Platform | pioarduino `platform-espressif32` 55.03.312-1 |
| Toolchain | xtensa-esp-elf 14.2.0 |

The platform is fetched into `.pio-core/` so the machine-wide PlatformIO install (and the
`circle` project's espressif32 6.13.0 / Arduino core 2.0.17) is left untouched.

## Behaviour-affecting build settings

| Setting | Value | Why |
|---|---|---|
| `board` | `esp32-s3-devkitc1-n16r8` | Matches the user's module: `qio_opi`, `psram_type=opi`, `default_16MB.csv`, `-DBOARD_HAS_PSRAM`. The generic `esp32-s3-devkitc-1` board is N8 **without PSRAM** and produces firmware that cannot buffer video. |
| `-include nimble_cfg.h` | required | Upstream `flash.sh:28` force-includes this. Without it `ble_link.cpp:16-20` fails `static_assert` and BLE buffers stay at the 12x256 default. |
| `-I .../libraries/FS/src` | added | The bundled USB library declares `depends=FS`, but PlatformIO does not resolve the built-in FS library from that name, so `USBHostMSC.cpp`/`USBMSCFS.cpp` failed on `#include "FS.h"`. |
| `ARDUINO_USB_MODE=0`, `ARDUINO_USB_CDC_ON_BOOT=1` | matches upstream | Native USB is the HID device. Consequence below. |

`DEBUG_SERIAL` is **not** enabled, matching a normal upstream flash.

## Local environment quirks (this machine only)

1. **Compiler cannot use the system TEMP.** `xtensa-esp-elf-gcc` failed with
   `Cannot create temporary file in C:\Users\...\Temp: Permission denied` while PowerShell
   itself could write there. Building with `TEMP`/`TMP` pointed at `./.tmp` works, so always
   build with those variables set (see below).
2. **esptool progress bar crashes on GBK consoles.** Set `PYTHONIOENCODING=utf-8` and
   `PYTHONUTF8=1`, otherwise esptool dies with `UnicodeEncodeError` while printing progress
   (the flash operation itself succeeds).
3. **Serial output is split across the two USB ports.** Because
   `ARDUINO_USB_CDC_ON_BOOT=1`, application `Serial` output goes to the **native USB** port.
   The UART port (COM4) shows only the ROM banner plus early Arduino init messages, then goes
   quiet. This is expected, not a hang. Add `-DDEBUG_SERIAL=1` or read the native port to see
   application logs.

## Build

```powershell
$env:TEMP = "D:\workspaces\iphone-use\esp32-iphone-agent\.tmp"
$env:TMP  = $env:TEMP
Set-Location D:\workspaces\iphone-use\esp32-iphone-agent
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e devkitc1_n16r8
```

Result:

```
RAM:   [===       ]  28.8% (used  94508 bytes from 327680 bytes)
Flash: [==        ]  22.4% (used 1466107 bytes from 6553600 bytes)
===================== [SUCCESS] =====================
```

## Verification performed (Windows side)

Backup taken **before** flashing anything (`tools/backup-flash.ps1`, full 16 MB read):

| | |
|---|---|
| file | `backups/esp32s3-before-esp32agent.bin` |
| size | 16777216 bytes |
| sha256 | `D8057B46885473C1C8849AFDEF79A7CA17C838D01616E9EEDDD651451FDDB3F6` |

The backup was confirmed to contain the previous firmware (strings `mcp`, `device_status`,
`stopwatch`, `anan`) and esptool parses its image header (`ESP32-S3 Image Header`,
`Flash size: 16MB`). Restore with `tools/restore-flash.ps1`.

New firmware flashed to COM4; esptool reported **`Hash of data verified`**.

Live status read from the board over BLE (`tools/watch-board.ps1`):

```json
{"up":453219,"rst":"power-on","heap":69016,"psram":3374020,
 "usb":"off","net":"ap","ip":"192.168.4.1","sta":0,"ap":"idle",
 "w":480,"h":1040,"kbps":0,"fps":0.0,
 "ble":{"mtu":517,"phy":1,"n":1,"drop":0,"rdrop":0}}
```

| Observation | Meaning |
|---|---|
| `psram: 3374020` | ~3.2 MB PSRAM usable — **confirms the n16r8 board choice was required and correct**; the generic board would report 0 and `video_begin()` would fail. |
| `mtu: 517` | `nimble_cfg.h` really applied (framework default is 256). |
| `net: ap`, `ip: 192.168.4.1` | Board opened its own hotspot. |
| `rst: power-on`, `drop/rdrop: 0` | Clean boot, no brownout, no BLE loss. |
| BLE advertises `ESP32 Agent D29D` | Device suffix derived from MAC (`netcfg.cpp:27-33`), so init completed. |
| `usb: off`, `ap: idle` | Expected: at that point the native USB was not attached to a host and no mirroring was running. |

### USB HID enumeration confirmed on the iPhone

With the board's **native USB** port attached to the iPhone, the board reported the
transition, proving iOS enumerated it as a HID mouse + keyboard:

```
1226     ok    idle   68768   3374020  *** USB off -> ok ***
```

The field then oscillated between `ok` and `asleep` while the phone screen was idle. That is
iOS suspending the USB peripheral on screen lock, not a firmware defect — `hid_bridge.ino`
handles it explicitly (`usbReady()` calls `tud_remote_wakeup()` and reports `asleep` rather
than stalling). After setting the phone's auto-lock to **Never**, the state held steady:

```
samples by state: {'ok': 30}
transitions: ok
STABLE 'ok' THROUGHOUT: YES
```

**Operational requirement: the phone must stay unlocked and awake while the agent runs.**
Upstream documents the mirroring half of this ("Mirroring stops when the phone locks"), but
the USB link drops out first, so the phone's auto-lock must be disabled before any run.

Throughout the suspend/resume cycles the board stayed healthy: `heap` steady at ~68760,
`psram` steady at 3374020, no panic, no reboot, `up` climbing past 1500 s.

## Partition layouts differ between the two firmwares

The backup and the new firmware use entirely different partition tables, which matters for
rollback. Decoded from `backups/esp32s3-before-esp32agent.bin` at 0x8000 versus the newly
built `partitions.bin`:

| | previous `anan-iphone-use` (ESP-IDF) | current `esp32-iphone-agent` (Arduino `default_16MB`) |
|---|---|---|
| app | `factory` @ 0x10000, 3 MB | `app0` @ 0x10000 and `app1` @ 0x650000, 6.4 MB each |
| nvs | 24 KB @ 0x9000 | 20 KB @ 0x9000 |
| extra | `phy_init`, **`mcp_kv` @ 0xF00000 (64 KB)** | `otadata`, `spiffs`, `coredump` |
| OTA | no | yes (two app slots) |

Consequences:

- **Rollback must write the whole 16 MB image**, not just an app slot: the partition table
  itself differs, so `tools/restore-flash.ps1` writes from 0x0. Writing only a region would
  leave the board with a partition table that does not match its contents.
- **The Anan pairing identity lived in `mcp_kv` @ 0xF00000**, an `nvs`-subtype partition
  outside the app image — that is why it previously survived reflashing. The new firmware's
  table has no `mcp_kv`, so that partition is currently unused. The full backup does contain
  its contents, so a whole-flash restore should bring it back, but **whether the identity is
  actually recovered has not been tested** and depends on that region not being disturbed.
  If it matters, restore first and then check `identity` in the Anan firmware before assuming.

## Open issue: mirroring connects but no video reaches the app

After the app was built and installed, onboarding completed and the board reported AirPlay
**connected and streaming**, but any task failed immediately with the firmware's own error:

```
cannot start: err no video (start screen mirroring)
```

That string comes from `video_link.cpp:77`, returned when `snapshot()` is false, which
(`video_link.cpp:67`) requires `n > 0 && cfg_len > 0` — i.e. the GOP buffer holds no access
unit. Observed over ~20 minutes of continuous streaming:

| field | observed | expected when healthy |
|---|---|---|
| `ap` | `streaming` | `streaming` |
| `fps` | 3.7 – 6.0, fluctuating | ~5 |
| `idr` | **0** (never non-zero) | > 0 once a keyframe arrives |
| `gop` | **0** (never non-zero) | > 0 |
| `kbps` | **0** | rising |
| `usb` | `ok` | `ok` |

So the AirPlay handshake and the frame clock are working, but **no decodable video ever
lands in the GOP buffer**. Because `gop_push()` discards everything until an IDR arrives
(`gop.c:31-33`) and `wait_idr` is only cleared by one (`gop.c:28-30`), a stream that never
yields a detected IDR stays permanently empty regardless of how many frames arrive.

Two candidate explanations remain, and they are not yet separated:

1. **No IDR is being produced or detected.** `annexb_has_idr()` (`gop.c:46-50`) scans for
   NAL type 5. If iOS 27's AirPlay stream starts without an IDR, or the keyframe shape
   differs, the buffer never opens. Motion on the phone screen (swiping pages, opening apps)
   did **not** change `idr` from 0 in repeated 120–150 s windows.
2. **The BLE video path is far slower than required.** A `bench 512` run against this
   Windows host completed 512 KB in 355.6 s (`bench` returns elapsed ms,
   `video_link.cpp:105`), i.e. **~11.5 kbps** — roughly 30x below the 300-500 kbps the
   upstream docs describe. 480x1040 at 5 fps needs on the order of 100-300 kbps, so this
   path could not carry the stream even with an IDR present.

Caveat on (2): the upstream tooling notes explicitly say a Mac host sees only ~100-200 kbps
and to use the iPhone for real throughput, and `ble_link.cpp:29-30` throttles video to
`VIDEO_CTRL_PKTS = 8` ACL packets for `CMD_ACTIVE_MS = 1000` after any command write. A host
that polls status every few seconds may keep that throttle engaged. **The 11.5 kbps figure
therefore measures a throttled, non-representative client and must not be quoted as the
board's real capability.** Measuring properly requires the iPhone app alone, with no Windows
BLE client attached.

Not yet tried, and the natural next steps:

- Watch `idr` with **no** Windows BLE client attached at all (the App alone), to remove the
  throttle from the measurement.
- Try `coex bt|balance|wifi` at runtime (accepted by `board()` in `hid_bridge.ino`) to give
  BLE more airtime.
- Lower the load: `res 240 520` and `fps 2`, then retry.
- Build with `-DDEBUG_SERIAL=1` and read the **native USB** port to see the AirPlay layer's
  own logs (`mirror: SPS ... PPS`, `mirror: undecodable payload`), which would distinguish
  "no IDR" from "undecodable stream" directly.

This is the point at which the Windows-side investigation stopped; resolving it needs the
iPhone app as the only BLE peer plus the serial log above.

## RESOLVED (diagnosis): the AirPlay mirror stream stops after SPS/PPS

A `DEBUG_SERIAL=1` build (`platformio.ini` env `devkitc1_n16r8_debug`, section
"Diagnostic build" below) finally exposed the AirPlay layer. Mirroring **a Mac** to the board
produced a fully successful handshake and then silence:

```
[353093] rtsp: connection from 192.168.4.2
[353095] rtsp: GET /info (full txtAirPlay)
[353207] pair-setup ok
[353396] pair-verify 1 ok
[353418] pair-verify 2 ok
[353429] fp-setup 1 ok (mode 1)
[353452] fp-setup 2 ok
[353483] setup: session from <Mac>, timing port 62469
[353491] rtsp: GET /info (full)
[353503] record
[353752] setup: mirror stream on port 7100
[353767] mirror: data connection accepted
[360306] mirror: SPS 30 + PPS 4 bytes
   ... nothing further ...
(and later) log board status: ap idle      <- back to idle
```

Interpretation, using `mirror.c`:

- Every handshake stage succeeds, so pairing, FairPlay and RTSP are all fine.
- `mirror: SPS 30 + PPS 4 bytes` is printed by `handle_codec()` (`mirror.c:71`), which is only
  reached for **unencrypted** type-`0x01` packets, and it then calls `on_config`, so
  **`cfg_len > 0` is satisfied**. That was one of the two conditions in `snapshot()`
  (`video_link.cpp:67`).
- Video is carried in **encrypted** type-`0x00` packets (`mirror.c:78-82`). If such a packet
  arrived and failed to parse, `mirror: undecodable payload (N bytes)` (`mirror.c:81`) would
  be logged. **That line never appears.**
- So the board receives no usable type-`0x00` video payloads at all, `gop_push()` is never
  reached, `n` stays 0, and `snapshot()` keeps failing — which is why the app reports
  `err no video (start screen mirroring)` while everything else looks healthy.
- `ap` returning to `idle` afterwards means the board itself considers the session over.

**Conclusion: this is an upstream compatibility gap between the AirPlay mirror sender
(macOS 26 here, and iOS 27 from the app) and the firmware's mirror receiver — not a
configuration, build or board problem on this side.** Everything this project controls was
verified working: handshake, pairing, FairPlay, RTSP, SPS/PPS delivery, BLE at MTU 517 with
zero drops, and USB HID enumerated and stable.

Nothing about this is fixable by changing `platformio.ini`, the board definition, the Wi-Fi
setup or the phone settings; it requires changes in `hardware/hid_bridge/src/airplay/`.

### How the logs were obtained

With `ARDUINO_USB_CDC_ON_BOOT=1`, `Serial` — and therefore `logLine()` behind
`ap_cfg.log` (`hid_bridge.ino:203-219`) — goes to the **native USB** port, *not* UART.
Mirroring from a Mac while that port is attached to the Mac is therefore the only way to see
these logs: the board serves as a USB HID device **and** streams its log over the same
native USB connection. Trying to mirror from the iPhone instead leaves the native USB port
occupied by the phone, so the logs become unreadable — confirmed experimentally (UART shows
only the ROM banner; `DEBUG_SERIAL` does take effect, visible as the boot timestamp jumping
from `[17]` to `[3021]`, the 3 s serial-monitor wait at `hid_bridge.ino:226`).

## Absolute pointer (the fix for unreliable tapping)

The relative pointer could not aim reliably on this setup. Reconstruction from the app's own
logs and from the reference implementation in this repo:

- `Pointer.swift` keeps a model of where the pointer is: after `home` it *assumes* the pointer is
  exactly at (0,0), then every `move dx dy` is applied to that assumption.
- iOS accelerates relative pointer movement, so the distance actually travelled depends on the
  speed and size of each step. `movePaced()` (10 units per 8 ms) is a mitigation, not a fix.
- Errors therefore accumulate. The app only re-homes every `rehomeEvery = 10` gestures
  (`Pointer.swift:11`), so up to nine gestures run on a drifting estimate.
- A tap that lands tens of pixels off misses an app icon, the screen does not change, the actor
  prompt tells the model "the action missed", and the loop repeats.

Measured on the device: identical `move` sequences produced no screen change after 4 out of 4
actions (`frame: none after the action; screen unchanged`, AirPlay input staying at 4-12 kbps).

The ESP32 MCP firmware in this workspace solves the same problem with an **absolute** pointer
(`tud_hid_n_abs_mouse_report`), verified on the same iPhone: a sweep of normalised coordinates
moved the cursor across the screen exactly as instructed.

### Firmware change (`hardware/hid_bridge/hid_bridge.ino`)

- `USBHIDMouse` (relative) -> `USBHIDAbsoluteMouse`, reported over 0..32767 (`pos >> 1` from a
  0..65535 internal range, the same convention the MCP firmware uses).
- New `moveabs <x> <y>` command: absolute position in 0..65535, no accumulation.
- `home` is now `moveabs 0 0`, so it is exactly the top-left corner instead of a "slam into the
  corner and hope".
- `move <dx> <dy>` is kept for compatibility and converted to absolute internally
  (`MOVE_UNITS_PER_SCREEN = 1500` units per screen), so the unmodified app still runs.
- `scroll` re-reports the current absolute position (the absolute mouse needs a position).
- Status JSON now carries `px`/`py`, the position the firmware believes it is at.

### App change (`ios/IPhoneAgent/Agent/Pointer.swift`)

- `span` is fixed at the report's full scale (`absMax = 65535`), so the coordinate space needs no
  calibration and `startCalibration()` returns success immediately (the old relative-mouse
  routine is kept as `legacyCalibration` for reference).
- `moveTo()` sends `moveabs tx ty` directly: no `home`, no subtraction from a remembered
  position, no drift.
- `tap()` ignores its `aim` flag: locating the pointer on screen is pointless when the target
  cannot be missed.

### Verified on hardware (firmware side)

```
baseline idle            : 5 kbps
tap moveabs 32767 32767  : 84, 93, 52 kbps   (10-19x baseline; 5/5 taps changed the screen)
tap moveabs 22937 26214  : 76 kbps
tap moveabs 55705 26214  : 63 kbps
```

Identical coordinates produced 84 and 93 kbps - the same target hit twice - which is what the
relative pointer could not do (it produced 0-8 kbps, i.e. nothing at all). **The app-side change
is not yet verified on a device: it has to be built on a Mac.**

## What could NOT be verified from Windows

1. **Hotspot credentials.** The NET characteristic returns
   `Insufficient Authentication` on Windows. The password is random per device and
   deliberately behind authenticated read (`netcfg.cpp:35-40`), so it must be obtained by the
   iOS app, which then either auto-joins (paid account) or displays it for manual joining.
   Upstream's `board_test.py` relies on macOS/iOS pairing for this, so it cannot do it here.
2. **AirPlay mirroring** (`ap: idle` → `streaming`) — needs an iPhone or Mac as mirror source.
3. **The closed loop itself** — needs the iOS app, which must be built with Xcode on a Mac.

## Remaining work (requires the user's Mac and iPhone)

1. On the Mac: build and install the app.

   A corrected template is provided at `ios/Config/Local.xcconfig.free-team`. Either copy it:
   ```bash
   cp ios/Config/Local.xcconfig.free-team ios/Config/Local.xcconfig
   ```
   or copy the upstream example and **blank out the two entitlement lines**.

   **Trap:** `ios/Config/Local.xcconfig.example:9-10` sets `IA_ENTITLEMENTS` and
   `IA_WIDGET_ENTITLEMENTS` to non-empty values, contradicting its own comment on line 8
   ("Leave empty to build without them"). `Shared.xcconfig` defaults both to empty
   (lines 11-12) but includes `Local.xcconfig` **last** (line 25), so the example's values win
   and silently enable App Groups, Hotspot Configuration, Wi-Fi information and time-sensitive
   notifications — capabilities a free Personal Team cannot sign.

   Verified wiring: `ios/project.yml:68` and `:87` bind `CODE_SIGN_ENTITLEMENTS` to those
   variables, so an empty value means no entitlements file at all. That is the supported
   free-account path; do **not** disable code signing instead. Without Hotspot Configuration
   the app displays the hotspot SSID/password for manual joining
   (`HotspotJoin.swift:30-56`), so functionality is preserved.

   Real-device builds still require an Apple development signature; a free Apple ID must
   re-sign every 7 days (`README.md:78`). `CODE_SIGNING_ALLOWED=NO` in CI is simulator-only
   (`ci.yml:16-21`). `ios/project.yml:12` uses `CODE_SIGN_STYLE: Automatic`.

   Required fields: `DEVELOPMENT_TEAM` (10-character team ID) and a unique `IA_BUNDLE_ID`
   (the widget becomes `<id>.activity`). The committed `IPhoneAgent.xcodeproj` opens directly;
   XcodeGen is only needed if `project.yml` changes. Both the app and the embedded
   `AgentActivity` extension must be signed with the same team.
2. On the iPhone: follow the app's four onboarding steps (BLE pair, join hotspot + Screen
   Mirroring, AssistiveTouch + pointer calibration, model endpoint/key).
3. Verify the closed loop with a simple observable task and confirm the agent re-observes
   after acting.

## Windows tooling notes

This machine has **Windows PowerShell 5.1** (`powershell.exe`), not PowerShell 7 — there is no
`pwsh` on PATH. Run the helper scripts with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\watch-board.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-mirror.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\backup-flash.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\restore-flash.ps1
```

`uv` is available and is used to supply `bleak` on demand, so `board_test.py` and these
scripts need no manual Python environment.

## Rollback

Restores the 16 MB backup, returning the board to the previously verified
`anan-iphone-use` firmware and its persisted pairing identity.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\restore-flash.ps1
```

