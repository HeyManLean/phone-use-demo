# Changelog

## Unreleased (main)

- Absolute-pointer fixes: ignore legacy relative calibration files; map normalized model targets
  directly to 0..65535 and log the target/report coordinates. Calibration checks actual UIKit
  touch landings without changing the fixed report range.
- Batched drags now have a button-down dwell and intermediate held reports instead of a single
  endpoint jump. Support up to 64 points (including the default 25-point circle); reject overflow
  or malformed trailing arguments before moving.
- The actor prompt now states that the pointer is absolute (a tap lands where the coordinates say),
  instead of the upstream relative-mouse wording; `path` accepts up to 64 points like the firmware.
- Firmware build, host drag/parser regression and Swift syntax checks pass; iOS type checking,
  XCTest and actual target-hit/drag behavior still need Mac/device validation.

- No computer: the ESP32 is the AirPlay receiver (ported from UxPlay) with its own hotspot, and talks to a new
  iPhone app over BLE. The app is the agent: planner/actor loop, pointer calibration, run replay, Live Activity,
  widgets, Siri and Shortcuts, self-healing when the board, hotspot or mirroring drops.
- Prompts moved to `prompts/`.
- Product name: ESP32 Agent.
- `server/` is legacy and no longer needed.
- Faster rounds: a batched round of actions waits for the screen once (at its end), not after every action;
  taps and drags are one BLE round trip (`tap`, `drag` commands — older firmware falls back to the
  per-command form), and Diagnostics shows each round's total time, action count and screenshot size.

## v0.1.0

- Computer-side version: UxPlay + Python server + web UI; the ESP32 is a USB HID bridge over Wi-Fi.
