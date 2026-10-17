// Mirrored H.264 from AirPlay to BLE, see video_link.h.
#include "video_link.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "ble_link.h"
#include "src/video/gop.h"

// GOP since the latest IDR; overflow drops it until the next IDR. A still screen gets an IDR only every
// few minutes, so room for ~27 min of AUs at 5 fps (P-frames of a still screen are tiny).
const size_t GOP_CAP = 4 * 1024 * 1024;
const int MAX_AUS = 8192;
const size_t SCRATCH_CAP = 512 * 1024;   // largest access unit we forward
const size_t MAX_CHUNK = 512;

struct Req {
  char id[16];
  uint32_t kb;
  bool bench;
};

static gop_buf gop;
static SemaphoreHandle_t lock;
static uint8_t *scratch;
static QueueHandle_t reqs;
static TaskHandle_t task;
static volatile bool streaming;
static uint16_t seq;
static uint8_t chunk[VCHUNK_HDR + MAX_CHUNK];
static uint32_t winStart, winBytes, winFrames, lastIdr;
static volatile uint32_t statKbps, statIdrMs, statAt;
static volatile float statFps;

// One access unit as notifies. A failed chunk still consumes its seq, so the app sees a gap.
static bool sendAu(const uint8_t *p, size_t n, uint8_t flags, uint32_t ts) {
  size_t room = min(ble_video_room(), MAX_CHUNK);
  if (room < 5) return false;
  vchunk c;
  vchunk_begin(&c, p, n, flags, ts);
  for (size_t len; (len = vchunk_next(&c, chunk, room, seq)) > 0;) {
    bool ok = ble_video_send(chunk, len);
    seq++;
    if (!ok) return false;
  }
  return true;
}

// Copies AU i of generation gen into scratch; false if the GOP moved on.
static bool copyAu(uint32_t gen, int i, gop_au *au) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = gop.gen == gen && i < gop.n && gop.aus[i].len <= SCRATCH_CAP;
  if (ok) {
    *au = gop.aus[i];
    memcpy(scratch, gop.arena + au->off, au->len);
  }
  xSemaphoreGive(lock);
  return ok;
}

// Snapshot of the GOP state: generation, AU count, SPS/PPS.
static bool snapshot(uint32_t *gen, int *n, uint8_t *cfg, size_t *cfg_len, uint32_t *cfg_ts) {
  xSemaphoreTake(lock, portMAX_DELAY);
  *gen = gop.gen;
  *n = gop.wait_idr ? 0 : gop.n;
  *cfg_len = gop.cfg_len;
  *cfg_ts = gop.cfg_ts;
  memcpy(cfg, gop.cfg, gop.cfg_len);
  xSemaphoreGive(lock);
  return *n > 0 && *cfg_len > 0;
}

static String doSnap() {
  static uint8_t cfg[GOP_CFG_MAX];
  if (!ble_video_room()) return "err video not subscribed";
  // A snapshot must start at an IDR. After a GOP overflow, or right after mirroring starts, the
  // buffer sets wait_idr and drops everything until the next keyframe arrives - on a mostly
  // static screen that can be seconds away. Returning immediately made the app's very first
  // request fail with "err no video" even though the mirror stream was perfectly healthy, so
  // wait for a usable GOP instead of giving up on the spot.
  const uint32_t WAIT_MS = 15000;
  uint32_t start = millis();
  bool waited = false;
  for (int attempt = 0; attempt < 3;) {
    uint32_t gen, cfg_ts;
    size_t cfg_len;
    int n;
    if (!snapshot(&gen, &n, cfg, &cfg_len, &cfg_ts)) {
      if ((uint32_t)(millis() - start) >= WAIT_MS) return "err no video (start screen mirroring)";
      if (!waited) {
        waited = true;
        ESP_LOGI("video", "snap: no GOP yet, waiting up to %u ms for an IDR", (unsigned)WAIT_MS);
      }
      vTaskDelay(pdMS_TO_TICKS(100));  // do not consume an attempt: keep waiting for the keyframe
      continue;
    }
    attempt++;
    if (!sendAu(cfg, cfg_len, VF_CONFIG | VF_SNAP, cfg_ts)) return "err ble send failed";
    bool complete = true;
    for (int i = 0; i < n && complete; i++) {
      gop_au au;
      complete = copyAu(gen, i, &au);
      if (complete && !sendAu(scratch, au.len, VF_SNAP | (au.idr ? VF_IDR : 0), au.ts)) return "err ble send failed";
    }
    if (complete) return "ok";  // else a new IDR replaced the GOP mid-send: start over with it
  }
  return "err video changing too fast";
}

static String doBench(uint32_t kb) {
  size_t room = min(ble_video_room(), MAX_CHUNK);
  if (!room) return "err video not subscribed";
  uint32_t start = millis();
  memset(chunk + VCHUNK_HDR, 0xA5, room);
  for (size_t left = kb * 1024; left;) {
    size_t n = min(left, room);
    chunk[0] = (uint8_t)seq;
    chunk[1] = (uint8_t)(seq >> 8);
    chunk[2] = VF_BENCH;
    bool ok = ble_video_send(chunk, VCHUNK_HDR + n);
    seq++;
    if (!ok) return "err ble send failed";
    left -= n;
  }
  return "ok " + String(millis() - start);
}

// Continuous mode: send every AU once, catching up from the GOP's IDR when streaming starts (P-frames
// can't be skipped: each needs the one before, and the next IDR may be minutes away). After a failed
// send, start over from the GOP's IDR.
static void streamStep() {
  static uint32_t txGen = UINT32_MAX;
  static int next;
  static uint8_t cfg[GOP_CFG_MAX];
  if (!streaming) {
    txGen = UINT32_MAX;  // resend config + whole GOP when streaming resumes
    return;
  }
  // Temporarily out of room (BLE is the bottleneck) must NOT rewind the send position. Resetting
  // txGen here made every congested tick restart from the GOP's IDR, so the board spent the whole
  // run re-sending the same megabytes while the app decoded stale frames faster than real time.
  if (!ble_video_room()) return;
  uint32_t gen, cfg_ts;
  size_t cfg_len;
  int n;
  if (!snapshot(&gen, &n, cfg, &cfg_len, &cfg_ts)) return;
  if (gen != txGen) {
    next = 0;
    if (!sendAu(cfg, cfg_len, VF_CONFIG, cfg_ts)) return;  // retry the config next tick
    txGen = gen;                                          // only now is this GOP ours to drain
  }
  for (gop_au au; next < n && copyAu(gen, next, &au); next++)
    if (!sendAu(scratch, au.len, au.idr ? VF_IDR : 0, au.ts)) return;  // keep next: resume, never rewind
}

static void videoTask(void *) {
  for (;;) {
    Req r;
    if (xQueueReceive(reqs, &r, 0) == pdTRUE) {
      String reply = String(r.id) + " " + (r.bench ? doBench(r.kb) : doSnap());
#if DEBUG_SERIAL
      Serial.println("video: " + reply);  // the reply only goes over BLE; mirror it for serial-driven tests
#endif
      ble_reply(reply);
    }
    streamStep();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  }
}

bool video_begin() {
  uint8_t *arena = (uint8_t *)heap_caps_malloc(GOP_CAP, MALLOC_CAP_SPIRAM);
  gop_au *aus = (gop_au *)heap_caps_malloc(MAX_AUS * sizeof(gop_au), MALLOC_CAP_SPIRAM);
  scratch = (uint8_t *)heap_caps_malloc(SCRATCH_CAP, MALLOC_CAP_SPIRAM);
  if (!arena || !aus || !scratch) return false;
  gop_init(&gop, arena, GOP_CAP, aus, MAX_AUS);
  lock = xSemaphoreCreateMutex();
  reqs = xQueueCreate(4, sizeof(Req));
  return xTaskCreatePinnedToCore(videoTask, "video", 6 * 1024, NULL, 2, &task, 1) == pdPASS;
}

void video_on_config(const uint8_t *sps_pps, size_t len) {
  xSemaphoreTake(lock, portMAX_DELAY);
  gop_set_config(&gop, sps_pps, len, millis());
  xSemaphoreGive(lock);
}

static volatile uint32_t diagRxAus, diagIdrCount, diagLastNal, diagLastAuLen;

// NAL identification from an Annex-B access unit: returns (h264_type << 8) | h265_type.
// H.264 IDR slices are type 5; HEVC IDR_W_RADL/IDR_N_LP are types 19/20, which is why a stream
// that is really HEVC never matches annexb_has_idr() and the GOP stays empty.
static uint32_t nalInfo(const uint8_t *p, size_t n) {
  for (size_t i = 0; i + 4 < n; i++)
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      uint8_t b = p[i + 3];
      return ((uint32_t)(b & 0x1f) << 8) | (uint32_t)((b >> 1) & 0x3f);
    }
  return 0xFFFF;
}

void video_on_au(const uint8_t *au, size_t len, bool idr) {
  uint32_t now = millis();
  diagRxAus++;
  if (idr) diagIdrCount++;
  diagLastNal = nalInfo(au, len);
  diagLastAuLen = len;
  winBytes += len;
  winFrames++;
  if (idr) {
    if (lastIdr) statIdrMs = now - lastIdr;
    lastIdr = now;
  }
  if (now - winStart >= 1000) {
    statKbps = winBytes * 8 / (now - winStart);
    statFps = winFrames * 1000.0f / (now - winStart);
    statAt = now;
    winBytes = winFrames = 0;
    winStart = now;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  gop_push(&gop, au, len, idr, now);
  xSemaphoreGive(lock);
  xTaskNotifyGive(task);
}

void video_stream(bool on) { streaming = on; }

static void request(const String &id, bool bench, uint32_t kb) {
  Req r = {{0}, kb, bench};
  strncpy(r.id, id.c_str(), sizeof r.id - 1);
  if (xQueueSend(reqs, &r, 0) != pdTRUE) ble_reply(id + " err busy");
}

void video_snap(const String &id) { request(id, false, 0); }
void video_bench(const String &id, uint32_t kb) { request(id, true, kb); }

VideoStats video_stats() {
  bool fresh = millis() - statAt < 3000;  // mirroring stopped: report zeros, not stale numbers
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t gopBytes = gop.used;
  uint32_t cfgLen = (uint32_t)gop.cfg_len;
  xSemaphoreGive(lock);
  VideoStats s;
  s.kbps = fresh ? statKbps : 0;
  s.idr_ms = statIdrMs;
  s.gop_bytes = gopBytes;
  s.fps = fresh ? statFps : 0;
  s.cfg_len = cfgLen;
  s.rx_aus = diagRxAus;
  s.idr_count = diagIdrCount;
  s.last_nal = diagLastNal;
  s.last_au_len = diagLastAuLen;
  return s;
}
