# Changelog

## Unreleased (main)

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
