// HID bridge: an ESP32-S3 (XIAO, 8 MB PSRAM) plugged into the iPhone. It is the phone's
// USB mouse + keyboard, an AirPlay screen-mirroring receiver, and a BLE peripheral for the
// iOS app on the same phone. It holds no logic; the app decides. Protocol: docs/ble-protocol.md.
//
// HID commands (unchanged from the old TCP protocol, the board answers "ok" or "err <why>"):
//   ping | home | move <dx> <dy> | press | release | click | moveabs <x> <y> | tap <x> <y>
//   drag <x1> <y1> <x2> <y2> ... [hold]  (one write for a whole stroke)
//   scroll <v> <h> | type <ascii text> | key <combo, e.g. cmd+h, enter>
//   homescreen (the consumer-control Home key, "AC Home": iOS goes to the Home Screen, like a keyboard's home key)

#include <esp_task_wdt.h>
#include <esp_mac.h>
#include "USB.h"
#include "USBHIDMouse.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "tusb.h"
#include "ble_link.h"
#include "netcfg.h"
#include "video_link.h"
#include "src/airplay/airplay.h"

#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 0  // 1: echo commands and the AirPlay handshake over USB serial
#endif

const int WATCHDOG_MS = 8000;  // a hung loop (e.g. blocked USB) reboots the board instead of staying offline

const int STEP = 10;      // max pointer delta per legacy `move` unit (kept for compatibility)
const int STEP_MS = 8;
const int HOME_REPORTS = 50;   // unused since the pointer became absolute; kept for reference
// Absolute pointer, 0..65535 per axis (same convention the ESP32 MCP firmware uses). Absolute
// positioning removes every source of drift the relative pointer had: iOS pointer acceleration,
// lost reports and the "assume it landed exactly where we aimed" bookkeeping in the app.
const long PTR_MAX = 65535;
// One legacy `move` unit = 1/1500 of the screen, so an app that still sends relative moves keeps
// working; `moveabs` is the precise path.
const long MOVE_UNITS_PER_SCREEN = 1500;
// Batched `drag`: ms rested between pointer reports of one stroke, so iOS sees separate moves, not a jump.
const int DRAG_STEP_MS = 8;
static long ptrx = 0, ptry = 0;  // current pointer position in 0..65535

USBHIDAbsoluteMouse mouse;
USBHIDKeyboard keyboard;
USBHIDConsumerControl consumer;
const uint16_t AC_HOME = 0x0223;  // HID consumer page: AC Home
static String receiverName = "ESP32 Agent";

// The absolute mouse report is 16-bit signed; scale 0..65535 down to 0..32767.
static inline int16_t absw(long v) { return (int16_t)(v >> 1); }

static void ptrReport() { mouse.move(absw(ptrx), absw(ptry)); }

static void ptrSet(long x, long y) {
  ptrx = constrain(x, 0L, PTR_MAX);
  ptry = constrain(y, 0L, PTR_MAX);
  ptrReport();
}

static void ptrMove(long dx, long dy) {
  long sx = dx * PTR_MAX / MOVE_UNITS_PER_SCREEN;
  long sy = dy * PTR_MAX / MOVE_UNITS_PER_SCREEN;
  ptrSet(ptrx + sx, ptry + sy);
}

uint8_t keyCode(const String &k) {
  if (k == "enter") return KEY_RETURN;
  if (k == "esc") return KEY_ESC;
  if (k == "tab") return KEY_TAB;
  if (k == "backspace") return KEY_BACKSPACE;
  if (k == "space") return ' ';
  if (k == "up") return KEY_UP_ARROW;
  if (k == "down") return KEY_DOWN_ARROW;
  if (k == "left") return KEY_LEFT_ARROW;
  if (k == "right") return KEY_RIGHT_ARROW;
  if (k == "cmd") return KEY_LEFT_GUI;
  if (k == "ctrl") return KEY_LEFT_CTRL;
  if (k == "alt") return KEY_LEFT_ALT;
  if (k == "shift") return KEY_LEFT_SHIFT;
  return k.length() == 1 ? k[0] : 0;
}

bool pressCombo(String combo) {
  combo += '+';
  for (int i; (i = combo.indexOf('+')) >= 0; combo = combo.substring(i + 1)) {
    uint8_t c = keyCode(combo.substring(0, i));
    if (!c) { keyboard.releaseAll(); return false; }
    keyboard.press(c);
  }
  delay(30);
  keyboard.releaseAll();
  return true;
}

// A locked or sleeping phone suspends USB; sending HID reports then can stall. Ask it to wake up instead,
// and report "asleep" right away if it does not, so the board never hangs.
bool usbReady() {
  if (tud_suspended()) {
    tud_remote_wakeup();
    for (int i = 0; i < 50 && tud_suspended(); i++) delay(10);
  }
  return tud_mounted() && !tud_suspended();
}

// Parses up to MAXP decimal numbers from `s` into `out`; returns how many. Used by the batched gestures,
// where every extra BLE round trip of a multi-tap or a drawing stroke costs a round-trip time on top of
// the video share, so the app sends them as one command.
const int MAXP = 128;  // 64 points; the app's default circle has 25 points
static int parsePoints(const String &s, long *out) {
  int n = 0;
  const char *p = s.c_str();
  for (;;) {
    while (*p == ' ') p++;
    if (!*p || !strcmp(p, "hold")) return n;
    if (n == MAXP || !(*p == '-' || (*p >= '0' && *p <= '9'))) return -1;
    char *end = nullptr;
    long value = strtol(p, &end, 10);
    if (end == p || (*end && *end != ' ') || value < 0 || value > PTR_MAX) return -1;
    out[n++] = value;
    p = end;
  }
}

// True when the rest of the arguments is the word "hold" (the app's marker for a gesture that rests at its
// last point: a drag ends there, like a finger that stops before lifting, so no momentum).
static bool hasHold(const String &s) { return s.indexOf("hold") >= 0; }

String run(const String &line) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  long a = 0, b = 0;
  sscanf(arg.c_str(), "%ld %ld", &a, &b);

  // Validate before touching USB: a malformed gesture must not move a pointer or press a button.
  static long pts[MAXP];
  int np = 0;
  if (cmd == "drag") {
    np = parsePoints(arg, pts);
    if (np < 4 || np % 2) return "err usage: drag x1 y1 x2 y2 ... [hold] (1-64 points)";
  }

  if (cmd == "ping") return "ok";
  if (!usbReady()) return "err phone usb asleep (unlock the phone)";
  if (cmd == "home") { ptrSet(0, 0); return "ok"; }              // absolute: exact top-left
  if (cmd == "moveabs") {                                        // absolute position, 0..65535
    ptrSet((long)constrain(a, 0L, 65535L), (long)constrain(b, 0L, 65535L));
    return "ok";
  }
  if (cmd == "move") { ptrMove(a, b); return "ok"; }
  if (cmd == "press") { mouse.press(MOUSE_LEFT); return "ok"; }
  if (cmd == "release") { mouse.release(MOUSE_LEFT); return "ok"; }
  if (cmd == "click") { mouse.press(MOUSE_LEFT); delay(60); mouse.release(MOUSE_LEFT); return "ok"; }
  if (cmd == "tap") {                                            // move + click in one write
    ptrSet((long)constrain(a, 0L, 65535L), (long)constrain(b, 0L, 65535L));
    mouse.press(MOUSE_LEFT);
    delay(60);
    mouse.release(MOUSE_LEFT);
    return "ok";
  }
  if (cmd == "drag") {                                           // press, stroke through x y pairs, release
    ptrSet(constrain(pts[0], 0L, PTR_MAX), constrain(pts[1], 0L, PTR_MAX));
    delay(20);  // let iOS observe the start position before the button transition
    mouse.press(MOUSE_LEFT);
    delay(40);  // establish a drag before moving
    for (int i = 2; i + 1 < np; i += 2) {
      const long x0 = ptrx, y0 = ptry;
      const long x1 = constrain(pts[i], 0L, PTR_MAX), y1 = constrain(pts[i + 1], 0L, PTR_MAX);
      // At most 2% of a screen per report. Even a short two-point swipe gets a
      // continuous 120 ms stroke; longer segments take proportionally longer.
      const long distance = max(labs(x1 - x0), labs(y1 - y0));
      const int steps = max(15L, (distance + 1310L - 1) / 1310L);
      for (int step = 1; step <= steps; step++) {
        ptrSet(x0 + (x1 - x0) * step / steps, y0 + (y1 - y0) * step / steps);
        delay(DRAG_STEP_MS);
      }
    }
    if (hasHold(arg)) delay(250);  // rest at the end: lists and pickers stop exactly here
    mouse.release(MOUSE_LEFT);
    return "ok";
  }
  if (cmd == "scroll") { mouse.move(absw(ptrx), absw(ptry), a, b); return "ok"; }
  if (cmd == "type") { keyboard.print(arg); return "ok"; }
  if (cmd == "key") return pressCombo(arg) ? "ok" : "err unknown key";
  if (cmd == "homescreen") { consumer.press(AC_HOME); delay(40); consumer.release(); return "ok"; }
  return "err unknown command";
}

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_TASK_WDT: case ESP_RST_INT_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_SW: return "software";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

const char *usbState() {
  if (!tud_mounted()) return "off";
  return tud_suspended() ? "asleep" : "ok";
}

// Board-level commands from the app; everything else is an HID command for run().
String board(const String &line, const String &id) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  long a = 0, b = 0;
  int n = sscanf(arg.c_str(), "%ld %ld", &a, &b);
  if (cmd == "stream") {
    if (arg != "on" && arg != "off") return "err usage: stream on|off";
    video_stream(arg == "on");
    return "ok";
  }
  if (cmd == "app" || cmd == "log") {  // debug relay: Mac -> app ("app run ...") and app -> Mac ("log ...")
    ble_reply(cmd + " " + arg);         // every peer sees it; each side ignores lines it doesn't handle
    return "ok";
  }
  if (cmd == "snap") { video_snap(id); return ""; }  // replies when the GOP is sent
  if (cmd == "bench") {
    if (n < 1 || a <= 0 || a > 4096) return "err usage: bench KB (1-4096)";
    video_bench(id, a);
    return "";
  }
  if (cmd == "res") {
    if (n < 2 || a < 160 || b < 160 || a > 1920 || b > 2560) return "err usage: res W H";
    cfg_set_res(a, b);
    airplay_set_display(a, b, cfg_fps());
    return "ok";
  }
  if (cmd == "fps") {
    if (n < 1 || a < 1 || a > 60) return "err usage: fps N (1-60)";
    cfg_set_fps(a);
    airplay_set_display(cfg_width(), cfg_height(), a);
    return "ok";
  }
  if (cmd == "coex") {  // A/B knob, not stored: every boot starts at balance
    if (arg.length() && !net_coex(arg)) return "err usage: coex bt|balance|wifi";
    return String("ok ") + net_coex_mode();
  }
  if (cmd == "unpair") {
    ble_reply(id + " ok");  // answer before the link goes down
    ble_unpair();
    return "";
  }
  return run(line);
}

String statusJson(bool full) {
  static const char *apStates[] = {"idle", "connected", "streaming"};
  VideoStats v = video_stats();
  char s[512];
  snprintf(s, sizeof s,
           "{\"up\":%lu,\"rst\":\"%s\",\"heap\":%u,\"psram\":%u,\"usb\":\"%s\",\"net\":\"%s\",\"ip\":\"%s\",\"sta\":%d,"
           "\"ap\":\"%s\",\"w\":%u,\"h\":%u,\"kbps\":%lu,\"fps\":%.1f,\"idr\":%lu,\"gop\":%lu,"
           "\"cfg\":%lu,\"aus\":%lu,\"idrs\":%lu,\"nal\":%lu,\"aulen\":%lu,\"px\":%ld,\"py\":%ld,"
           "\"ble\":{\"mtu\":%u,\"phy\":%u,\"kbps\":%lu,\"drop\":%lu,\"rdrop\":%lu,\"n\":%d}}",
           millis(), resetReason(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), usbState(), net_mode(), net_ip().c_str(), net_peers(),
           apStates[airplay_state()], cfg_width(), cfg_height(), (unsigned long)v.kbps, v.fps, (unsigned long)v.idr_ms,
           (unsigned long)v.gop_bytes, (unsigned long)v.cfg_len, (unsigned long)v.rx_aus,
           (unsigned long)v.idr_count, (unsigned long)v.last_nal, (unsigned long)v.last_au_len,
           ptrx, ptry,
           ble_mtu(), ble_phy(), (unsigned long)ble_kbps(), (unsigned long)ble_drops(),
           (unsigned long)ble_reply_drops(), ble_peers());
  if (full) return s;
  // MTU below ~250 (before the MTU exchange): keep only the fields the app needs to stay alive
  snprintf(s, sizeof s, "{\"up\":%lu,\"rst\":\"%s\",\"usb\":\"%s\",\"net\":\"%s\",\"ap\":\"%s\",\"ble\":{\"mtu\":%u}}",
           millis(), resetReason(), usbState(), net_mode(), apStates[airplay_state()], ble_mtu());
  return s;
}

void statusTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!ble_connected()) continue;
    String s = statusJson(true);
    if (s.length() > ble_mtu() - 3u) s = statusJson(false);
    ble_status(s);
  }
}

void logLine(const char *msg) {
#if DEBUG_SERIAL
  Serial.printf("[%lu] %s\n", millis(), msg);
#endif
}

void startAirplay() {
  static airplay_cfg cfg;
  cfg.name = receiverName.c_str();
  esp_read_mac(cfg.hwaddr, ESP_MAC_WIFI_STA);
  cfg_ed25519_seed(cfg.ed25519_seed);
  cfg.width = cfg_width();
  cfg.height = cfg_height();
  cfg.max_fps = cfg_fps();
  cfg.on_config = video_on_config;
  cfg.on_au = video_on_au;
  cfg.log = logLine;
  if (!airplay_start(&cfg)) Serial.println("airplay: start failed");
}

void setup() {
  Serial.begin(115200);
#if DEBUG_SERIAL
  for (int i = 0; i < 30 && !Serial; i++) delay(100);  // give a serial monitor 3 s to catch the boot lines
#endif
  mouse.begin();
  keyboard.begin();
  consumer.begin();
  USB.productName("HID Bridge");
  USB.begin();
  esp_task_wdt_config_t wdt = {.timeout_ms = WATCHDOG_MS, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(NULL);

  net_begin();
  if (!video_begin()) Serial.println("video: no PSRAM for the GOP buffer");
  ble_begin("ESP32 Agent " + board_suffix());
  startAirplay();
  xTaskCreatePinnedToCore(statusTask, "status", 4096, NULL, 1, NULL, 1);
  Serial.printf("boot %s: free internal heap %u (largest %u), PSRAM %u, reset %s\n", receiverName.c_str(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), resetReason());
}

void loop() {
  static bool linked = false;
  esp_task_wdt_reset();
  net_poll();
  if (linked && !ble_connected()) {
    mouse.release(MOUSE_LEFT);  // a gesture cut off mid-way must not leave the button held (long press)
    keyboard.releaseAll();
    video_stream(false);
  }
  linked = ble_connected();

  String line;
  if (Serial.available()) {  // USB serial takes the same `<id> <command>` lines, for debugging from a Mac
    line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("0 net ")) { net_request(line.substring(6)); Serial.println("0 ok"); return; }
    int sp = line.indexOf(' ');
    String id = sp < 0 ? line : line.substring(0, sp);
    Serial.println(id + " " + board(sp < 0 ? "" : line.substring(sp + 1), id));
    return;
  }
  if (!ble_pop_cmd(line, 50)) return;
  int sp = line.indexOf(' ');
  String id = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? "" : line.substring(sp + 1);
  rest.trim();
#if DEBUG_SERIAL
  unsigned long started = millis();
#endif
  String reply = id.toInt() || id == "0" ? board(rest, id) : "err missing request id";
  if (reply.length()) ble_reply(id + " " + reply);
#if DEBUG_SERIAL
  Serial.printf("%s -> %s (%lu ms)\n", line.c_str(), reply.c_str(), millis() - started);
#endif
}
