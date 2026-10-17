// Mirrored H.264 from AirPlay to BLE: keeps the current GOP in PSRAM and sends it on `snap`,
// or every access unit while `stream on`. Also runs `bench` and computes the video stats.
#pragma once
#include <Arduino.h>

bool video_begin();
void video_on_config(const uint8_t *sps_pps, size_t len);  // AirPlay callbacks (mirror task)
void video_on_au(const uint8_t *au, size_t len, bool idr);
void video_stream(bool on);
void video_snap(const String &id);               // replies "<id> ok" when the GOP is sent
void video_bench(const String &id, uint32_t kb);  // replies "<id> ok <ms>"

struct VideoStats {
  uint32_t kbps, idr_ms, gop_bytes;
  float fps;
  // Diagnostics: expose what the decoder side actually received, readable over BLE so it works
  // even when the native USB port is occupied by the phone and serial logs are unavailable.
  uint32_t cfg_len;    // SPS+PPS bytes accepted (0 => codec config never parsed)
  uint32_t rx_aus;     // access units delivered by the AirPlay layer
  uint32_t idr_count;  // AUs classified as IDR
  uint32_t last_nal;   // (hdr[4] << 8) | first NAL type seen, for format identification
  uint32_t last_au_len;
};
VideoStats video_stats();
