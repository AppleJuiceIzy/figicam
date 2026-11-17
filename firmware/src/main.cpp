// ============================================================
//  Figicam v1 firmware
//  Freenove ESP32-S3 WROOM CAM + OV5640 · 27 exposures
//  Wind the thumbwheel -> press shutter -> photo to SD.
//  After the last frame the roll uploads to the Lovable darkroom.
// ============================================================
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_wifi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <RTClib.h>
#include <U8g2lib.h>
#include "esp_camera.h"
#include "config.h"

// secrets.h may #define CAMERA_ID. Otherwise the id is filled in setup()
// from the last 6 hex digits of the Wi-Fi MAC.
#ifndef CAMERA_ID
#include <esp_mac.h>
static char cameraIdRuntime[16] = "figicam-000000";
#define CAMERA_ID cameraIdRuntime
#define CAMERA_ID_FROM_MAC 1
#else
#define CAMERA_ID_FROM_MAC 0
#endif

// ---------- Freenove ESP32-S3 WROOM CAM camera pins ----------
#define CAM_PWDN   -1
#define CAM_RESET  -1
#define CAM_XCLK   15
#define CAM_SIOD    4
#define CAM_SIOC    5
#define CAM_Y9     16
#define CAM_Y8     17
#define CAM_Y7     18
#define CAM_Y6     12
#define CAM_Y5     10
#define CAM_Y4      8
#define CAM_Y3      9
#define CAM_Y2     11
#define CAM_VSYNC   6
#define CAM_HREF    7
#define CAM_PCLK   13

// ---------- onboard microSD (1-bit SDMMC) ----------
#define SD_CLK 39
#define SD_CMD 38
#define SD_D0  40

// 0.96" SSD1306 OLED, 128x64, on the shared I2C bus
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
RTC_DS3231 rtc;
Preferences prefs;

bool hasCam = false, hasSd = false, hasRtc = false, hasLcd = false;
bool unpaired = false;                 // upload-frame returned 403
bool resumeAfterMessage = false;      // start the upload once "paired" has shown
char setupSsid[16] = "Figicam-0000";

// ---------- roll state (persisted in flash) ----------
uint32_t rollId = 0;
int framesShot = 0;                    // photos on this roll, 0..27
int uploaded = 0;
bool wound = false;                    // dial has advanced; shutter is unlocked
bool closed = false;                   // no more shooting; upload or already sent
int clicks = 0;                        // wind detents, 0..7

enum Mode { SHOOTING, DEVELOPING, WAITING_WIFI, SENT };
Mode mode = SHOOTING;

// ---------- input / UI state ----------
volatile uint8_t encPrev = 0;
int encSub = 0;
volatile int encSteps = 0;             // quadrature steps accumulated in the ISR
bool shutterDown = false;
volatile bool shutterFell = false;
volatile uint32_t shutterFellAt = 0;
uint32_t shutterChangedAt = 0;
uint32_t shutterPressedAt = 0;
enum Gesture { GESTURE_NONE, GESTURE_HOLD, GESTURE_CONSUMED };
Gesture shutterGesture = GESTURE_NONE;
enum HoldHint { HINT_NONE, HINT_REWIND, HINT_NEW };
HoldHint holdHint = HINT_NONE;
static const uint32_t HOLD_HINT_MS = 3000;
static const uint32_t HOLD_WIFI_MS = 8000;
static const uint32_t REWIND_CONFIRM_MS = 4000;
static const uint32_t WIFI_ATTEMPT_MS = 15000;
static const int WIFI_SLOTS = 5;
static const int WIFI_SSID_LEN = 32;
static const int WIFI_PASS_LEN = 64;
bool rewindAsk = false;
uint32_t rewindAskAt = 0;
bool retryShown = false;
bool drumAnimating = false;
uint32_t drumStart = 0;
int drumFrom = 0;
uint32_t lastRetryAt = 0;
bool uiDirty = true;
uint32_t messageUntil = 0;
bool overlayShown = false;
bool clockSynced = false;

int dialNumber() { return wound ? framesShot + 1 : framesShot; }

// ============================================================
//  Helpers
// ============================================================
void saveState() {
  prefs.putUInt("roll", rollId);
  prefs.putInt("shot", framesShot);
  prefs.putInt("sent", uploaded);
  prefs.putBool("wound", wound);
  prefs.putInt("dots", clicks);
  prefs.putBool("closed", closed);
}

String rollDir()  { return "/roll_" + String((unsigned long)rollId); }
String metaPath() { return rollDir() + "/meta.csv"; }
String framePath(int n) {
  char b[40];
  snprintf(b, sizeof b, "/roll_%lu/f%02d.jpg", (unsigned long)rollId, n);
  return String(b);
}
#if CAMERA_ID_FROM_MAC
void initCameraId() {
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return;
  snprintf(cameraIdRuntime, sizeof cameraIdRuntime, "figicam-%02X%02X%02X",
           mac[3], mac[4], mac[5]);
}
#endif

String rollKey() { return String(CAMERA_ID) + "-" + String((unsigned long)rollId); }

uint32_t nowUnix() { return hasRtc ? rtc.now().unixtime() : 0; }

// Drum is centered in the space left of the battery so the two never meet.
static const int BATT_X = 118;
static const int BATT_Y = 19;
static const int DIAL_CX = 56;

float batteryVolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BATT_SENSE);
  return (sum / 16.0f) * BATT_DIVIDER / 1000.0f;
}

int battStep = -1;                     // 0/20/40/60/80/100, -1 until the first sample
bool battUsb = false;
bool battLow = false;
uint32_t battSampledAt = 0;
bool battBlinkOn = true;

int lipoPercent(float v) {
  static const float vs[] = {4.20f, 4.10f, 4.00f, 3.90f, 3.80f, 3.70f, 3.60f, 3.50f, 3.30f};
  static const int ps[]   = {100,   90,    80,    65,    50,    30,    15,    5,     0};
  if (v >= vs[0]) return 100;
  if (v <= vs[8]) return 0;
  for (int i = 0; i < 8; i++) {
    if (v <= vs[i] && v >= vs[i + 1]) {
      float u = (v - vs[i + 1]) / (vs[i] - vs[i + 1]);
      return ps[i + 1] + (int)((ps[i] - ps[i + 1]) * u + 0.5f);
    }
  }
  return 0;
}

int quantizeBatt(int pct) {
  if (pct < 10) return 0;
  if (pct < 30) return 20;
  if (pct < 50) return 40;
  if (pct < 70) return 60;
  if (pct < 90) return 80;
  return 100;
}

void sampleBattery() {
  if (mode == DEVELOPING) return;       // not while a frame is uploading
  uint32_t now = millis();
  if (battSampledAt != 0 && now - battSampledAt < 20000) {
    if (battLow) {
      bool on = ((now / 1000) % 2) == 0;
      if (on != battBlinkOn) { battBlinkOn = on; uiDirty = true; }
    }
    return;
  }
  battSampledAt = now;
  float v = batteryVolts();
  int step = battStep;
  bool usb = false;
  bool low = false;
  if (v < 0.5f) {
    usb = true;
    step = 0;
  } else {
    int pct = lipoPercent(v);
    low = pct < 15;
    step = low ? 0 : quantizeBatt(pct);
  }
  if (step != battStep || usb != battUsb || low != battLow) uiDirty = true;
  battStep = step;
  battUsb = usb;
  battLow = low;
  if (!battLow) battBlinkOn = true;
}

void drawBattery() {
  if (battStep < 0) return;
  if (battLow && !battBlinkOn) return;  // empty outline, 1 s on / 1 s off
  const int w = 10, h = 28;
  oled.drawBox(BATT_X + 3, BATT_Y - 2, 4, 2);          // nub
  oled.drawFrame(BATT_X, BATT_Y, w, h);
  if (battUsb) {
    // Small bolt, centered in the 8×26 interior.
    static const int8_t bolt[][2] = {
      {4,10},{5,10}, {3,11},{4,11},{5,11}, {4,12},
      {3,13},{4,13},{5,13}, {3,14}, {2,15},{3,15}
    };
    for (int i = 0; i < 12; i++) oled.drawPixel(BATT_X + bolt[i][0], BATT_Y + bolt[i][1]);
    return;
  }
  int fillH = (26 * battStep + 50) / 100;
  if (battStep >= 100) fillH = 26;
  if (fillH > 0) oled.drawBox(BATT_X + 1, BATT_Y + 1 + (26 - fillH), 8, fillH);
}

void flashMessage(const char* a, const char* b, uint32_t ms) {
  if (hasLcd) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_helvB10_tr);
    bool two = b && b[0];
    int16_t y = two ? 28 : (64 + oled.getAscent() + oled.getDescent()) / 2;
    int16_t w = oled.getStrWidth(a);
    oled.drawStr(DIAL_CX - w / 2, y, a);
    if (two) {
      w = oled.getStrWidth(b);
      oled.drawStr(DIAL_CX - w / 2, 48, b);
    }
    drawBattery();
    oled.sendBuffer();
  }
  if (a[0]) Serial.printf("[LCD] %s%s%s\n", a, (b && b[0]) ? " | " : "", (b && b[0]) ? b : "");
  messageUntil = millis() + ms;
  overlayShown = true;
  uiDirty = true;
}

// Film-counter drum. t=0 rests on `from`; t=1 has rolled up to from+1.
// Ease-out may overshoot past 1 so the digit clicks back into the window.
void drawDial(int from, float t, const char* footer) {
  if (!hasLcd) return;
  auto lerp = [](int a, int b, float u) { return a + (int)((b - a) * u); };
  auto glyph = [](const uint8_t* font, int value, int baseline) {
    if (value < 0 || value > FRAMES_PER_ROLL) return;
    char buf[4];
    snprintf(buf, sizeof buf, "%d", value);
    oled.setFont(font);
    int16_t x = DIAL_CX - (int16_t)oled.getStrWidth(buf) / 2;
    if (x < 0) x = 0;
    oled.drawStr(x, baseline, buf);
  };

  oled.clearBuffer();
  bool arrived = t >= 0.5f;
  // Previous sliver at the top, the live number in the window, the next sliver just above the wind track.
  glyph(u8g2_font_logisoso24_tn, from - 1, lerp(6, -22, t));
  if (!footer) glyph(arrived ? u8g2_font_logisoso24_tn : u8g2_font_logisoso42_tn, from, lerp(50, 6, t));
  else         glyph(u8g2_font_logisoso42_tn, from, 50);
  if (!footer) glyph(arrived ? u8g2_font_logisoso42_tn : u8g2_font_logisoso24_tn, from + 1, lerp(78, 50, t));
  if (t > 0.05f) glyph(u8g2_font_logisoso24_tn, from + 2, lerp(106, 78, t));

  if (footer) {
    oled.setFont(u8g2_font_5x8_tf);
    int16_t w = oled.getStrWidth(footer);
    oled.drawStr(DIAL_CX - w / 2, 63, footer);
  } else if (t == 0.f) {
    for (int i = 0; i < CLICKS_TO_WIND; i++) {
      int x = 8 + i * 13;
      if (i < clicks) oled.drawBox(x - 3, 61, 7, 2);
      else            oled.drawBox(x, 61, 2, 2);
    }
  }
  drawBattery();
  oled.sendBuffer();
}

float easeOutBack(float x) {
  const float c1 = 1.2f;
  const float c3 = c1 + 1.f;
  float p = x - 1.f;
  return 1.f + c3 * p * p * p + c1 * p * p;
}

void drawFitted(const char* s, int16_t y) {
  const int limit = BATT_X - 2;
  const uint8_t* fonts[] = {
    u8g2_font_helvB10_tr,
    u8g2_font_helvB08_tr,
    u8g2_font_5x8_tf
  };
  const uint8_t* font = fonts[2];
  for (int i = 0; i < 3; i++) {
    oled.setFont(fonts[i]);
    if ((int)oled.getStrWidth(s) <= limit) { font = fonts[i]; break; }
  }
  oled.setFont(font);
  int16_t w = oled.getStrWidth(s);
  int16_t x = DIAL_CX - w / 2;
  if (x < 0) x = 0;
  oled.drawStr(x, y, s);
}

void drawStatus(const char* a, const char* b) {
  if (!hasLcd) return;
  oled.clearBuffer();
  int16_t y = b ? 28 : 0;
  if (!b) {
    oled.setFont(u8g2_font_helvB10_tr);
    y = (64 + oled.getAscent() + oled.getDescent()) / 2;
  }
  drawFitted(a, y);
  if (b) drawFitted(b, 48);
  drawBattery();
  oled.sendBuffer();
}

// 6 digits of the counter's 42 px font are wider than the 128 px OLED,
// so use that font when the code fits and the next logisoso size when it doesn't.
void drawPairCode(const char* code) {
  if (!hasLcd) return;
  const int limit = BATT_X - 2;
  const uint8_t* fonts[] = {
    u8g2_font_logisoso42_tn,
    u8g2_font_logisoso34_tn,
    u8g2_font_logisoso24_tn
  };
  const uint8_t* font = fonts[2];
  oled.clearBuffer();
  for (int i = 0; i < 3; i++) {
    oled.setFont(fonts[i]);
    if ((int)oled.getStrWidth(code) <= limit) { font = fonts[i]; break; }
  }
  oled.setFont(font);
  int16_t w = oled.getStrWidth(code);
  int16_t x = (limit - w) / 2;
  if (x < 0) x = 0;
  oled.drawStr(x, 50, code);
  oled.setFont(u8g2_font_5x8_tf);
  const char* footer = "enter on site";
  w = oled.getStrWidth(footer);
  x = (limit - w) / 2;
  if (x < 0) x = 0;
  oled.drawStr(x, 63, footer);
  drawBattery();
  oled.sendBuffer();
}

void drawUi() {
  if (millis() < messageUntil) return;
  if (holdHint == HINT_REWIND) { drawStatus("release: rewind", nullptr); return; }
  if (holdHint == HINT_NEW)    { drawStatus("release: new roll", nullptr); return; }
  if (rewindAsk) { drawStatus("rewind?", "tap to confirm"); return; }
  switch (mode) {
    case SHOOTING:
      if (drumAnimating) {
        float x = (millis() - drumStart) / 250.f;
        if (x < 0.f) x = 0.f;
        if (x > 1.f) x = 1.f;
        drawDial(drumFrom, easeOutBack(x), nullptr);
      } else {
        drawDial(dialNumber(), 0.f, retryShown ? "retry" : nullptr);
      }
      break;
    case DEVELOPING: {
      int n = uploaded + 1;
      if (n > framesShot) n = framesShot;
      char bot[12];
      snprintf(bot, sizeof bot, "%d/%d", n, framesShot);
      drawStatus("developing", bot);
      break;
    }
    case WAITING_WIFI:
      if (unpaired) drawStatus("not paired", "hold shutter 8s");
      else          drawStatus("no wifi", nullptr);
      break;
    case SENT:
      drawStatus("roll sent", "hold shutter: new roll");
      break;
  }
}

// ============================================================
//  Hardware init
// ============================================================
bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = CAM_Y2; c.pin_d1 = CAM_Y3; c.pin_d2 = CAM_Y4; c.pin_d3 = CAM_Y5;
  c.pin_d4 = CAM_Y6; c.pin_d5 = CAM_Y7; c.pin_d6 = CAM_Y8; c.pin_d7 = CAM_Y9;
  c.pin_xclk = CAM_XCLK;
  c.pin_pclk = CAM_PCLK;
  c.pin_vsync = CAM_VSYNC;
  c.pin_href = CAM_HREF;
  c.pin_sccb_sda = CAM_SIOD;
  c.pin_sccb_scl = CAM_SIOC;
  c.pin_pwdn = CAM_PWDN;
  c.pin_reset = CAM_RESET;
  c.xclk_freq_hz = CAM_XCLK_HZ;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_QSXGA;        // 2560x1920; driver clamps to the sensor's max (stock OV2640 -> UXGA)
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  Serial.printf("Camera sensor PID: 0x%x %s\n", s->id.PID,
                s->id.PID == OV5640_PID ? "(OV5640)" : "(not OV5640 - stock sensor?)");
  // Disposable-style auto exposure and white balance. Neutral grade for the darkroom.
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_gainceiling(s, GAINCEILING_32X);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_lenc(s, 1);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);
  // If photos come out upside down or mirrored, uncomment:
  // s->set_vflip(s, 1);
  // s->set_hmirror(s, 1);
  return true;
}

bool initSd() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) return false;   // true = 1-bit mode
  return SD_MMC.cardType() != CARD_NONE;
}

// ============================================================
//  Roll state
// ============================================================
void startNewRoll() {
  rollId = hasRtc ? nowUnix() : esp_random();
  framesShot = 0;
  uploaded = 0;
  wound = false;
  closed = false;
  clicks = 0;
  retryShown = false;
  rewindAsk = false;
  drumAnimating = false;
  prefs.putBool("done", false);
  if (hasSd) SD_MMC.mkdir(rollDir());
  saveState();
  mode = SHOOTING;
  uiDirty = true;
  Serial.printf("New roll: %s\n", rollKey().c_str());
}

void loadState() {
  prefs.begin("latent", false);
  if (!prefs.isKey("roll")) { startNewRoll(); return; }

  rollId     = prefs.getUInt("roll", 0);
  framesShot = prefs.getInt("shot", 0);
  uploaded   = prefs.getInt("sent", 0);
  wound      = prefs.getBool("wound", false);
  clicks     = prefs.getInt("dots", 0);
  closed     = prefs.getBool("closed", false);
  if (hasSd && !SD_MMC.exists(rollDir())) SD_MMC.mkdir(rollDir());

  if (prefs.getBool("done", false)) mode = SENT;
  else if (closed)                  mode = DEVELOPING;
  else                              mode = SHOOTING;

  Serial.printf("Roll %s: %d shot, %d uploaded, wound=%d\n",
                rollKey().c_str(), framesShot, uploaded, wound);
}

// ============================================================
//  Capture
// ============================================================
bool takePhoto() {
  camera_fb_t* fb = esp_camera_fb_get();   // CAMERA_GRAB_LATEST is already the newest frame
  if (!fb) { Serial.println("Capture failed"); return false; }

  int n = framesShot + 1;
  size_t len = fb->len;
  File f = SD_MMC.open(framePath(n), FILE_WRITE);
  bool ok = f && f.write(fb->buf, len) == len;
  if (f) f.close();
  esp_camera_fb_return(fb);
  if (!ok) { Serial.println("SD write failed"); return false; }

  File m = SD_MMC.open(metaPath(), FILE_APPEND);
  if (m) { m.printf("%d,%lu\n", n, (unsigned long)nowUnix()); m.close(); }

  framesShot = n;
  wound = false;
  clicks = 0;
  retryShown = false;
  if (framesShot >= FRAMES_PER_ROLL) {
    closed = true;
    mode = DEVELOPING;
    lastRetryAt = 0;
  }
  saveState();
  Serial.printf("Frame %d saved (%u bytes)\n", n, (unsigned)len);
  uiDirty = true;
  return true;
}

// ============================================================
//  Upload to the Lovable darkroom
// ============================================================
uint32_t frameTime(int n) {
  File m = SD_MMC.open(metaPath());
  if (!m) return 0;
  while (m.available()) {
    String line = m.readStringUntil('\n');
    int c = line.indexOf(',');
    if (c > 0 && line.substring(0, c).toInt() == n) {
      m.close();
      return (uint32_t)line.substring(c + 1).toInt();
    }
  }
  m.close();
  return 0;
}

void addAuthHeaders(HTTPClient& http) {
  http.addHeader("x-api-key", API_KEY);
  http.addHeader("x-camera-id", CAMERA_ID);
}

void addRollHeaders(HTTPClient& http) {
  addAuthHeaders(http);
  http.addHeader("x-roll-id", rollKey());
  http.addHeader("x-frames-total", String(framesShot));
}

bool redirectCode(int code) {
  return code == 301 || code == 302 || code == 307 || code == 308;
}

void logExchange(const char* method, const char* url, HTTPClient& http, int code, const String& body) {
  Serial.printf("%s %s -> %d\n", method, url, code);
  if (body.length()) Serial.println(body);
  else Serial.println("(empty body)");
  if (redirectCode(code)) Serial.printf("Location: %s\n", http.header("Location").c_str());
}

bool beginRequest(WiFiClientSecure& client, HTTPClient& http, const char* url, uint16_t timeoutMs) {
  client.setInsecure();                  // v1 shortcut; pin the root CA later
  if (!http.begin(client, url)) {
    Serial.printf("%s -> begin failed\n", url);
    return false;
  }
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);
  static const char* keys[] = {"Location"};
  http.collectHeaders(keys, 1);
  return true;
}

bool jsonSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Pulls a JSON string or literal after "key":
bool extractJsonValue(const String& body, const char* key, String& out) {
  String pat = String("\"") + key + "\"";
  int i = 0;
  while ((i = body.indexOf(pat, i)) >= 0) {
    int c = body.indexOf(':', i + pat.length());
    if (c < 0) return false;
    int j = c + 1;
    while (j < (int)body.length() && jsonSpace(body[j])) j++;
    if (j >= (int)body.length()) return false;
    if (body[j] == '"') {
      int e = body.indexOf('"', j + 1);
      if (e < 0) return false;
      out = body.substring(j + 1, e);
      return true;
    }
    int e = j;
    while (e < (int)body.length() && body[e] != ',' && body[e] != '}' && !jsonSpace(body[e])) e++;
    out = body.substring(j, e);
    return out.length() > 0;
  }
  return false;
}

bool extractPairCode(const String& body, String& code) {
  if (!extractJsonValue(body, "code", code)) return false;
  code.trim();
  if (!code.length()) return false;
  bool digits = true;
  for (unsigned i = 0; i < code.length(); i++) {
    if (code[i] < '0' || code[i] > '9') { digits = false; break; }
  }
  if (digits) while (code.length() < 6) code = "0" + code;
  return true;
}

struct WifiSlot {
  char ssid[WIFI_SSID_LEN + 1];
  char pass[WIFI_PASS_LEN + 1];
};

struct WifiList {
  uint8_t count;
  WifiSlot slot[WIFI_SLOTS];
};

WifiList wifiNets;
bool wifiNetsLoaded = false;

void copyWifiField(char* dst, int len, const char* src) {
  if (!src) src = "";
  strncpy(dst, src, len);
  dst[len] = 0;
}

void loadWifiNets() {
  if (wifiNetsLoaded) return;
  wifiNetsLoaded = true;
  memset(&wifiNets, 0, sizeof wifiNets);
  if (prefs.getBytesLength("wifinets") != sizeof wifiNets) return;
  if (prefs.getBytes("wifinets", &wifiNets, sizeof wifiNets) != sizeof wifiNets) {
    memset(&wifiNets, 0, sizeof wifiNets);
    return;
  }
  if (wifiNets.count > WIFI_SLOTS) {
    memset(&wifiNets, 0, sizeof wifiNets);
    return;
  }
  WifiList clean = {};
  for (uint8_t i = 0; i < wifiNets.count; i++) {
    wifiNets.slot[i].ssid[WIFI_SSID_LEN] = 0;
    wifiNets.slot[i].pass[WIFI_PASS_LEN] = 0;
    if (!wifiNets.slot[i].ssid[0]) continue;
    clean.slot[clean.count++] = wifiNets.slot[i];
  }
  wifiNets = clean;
}

// Newest first. An SSID already in the list is updated and moved to the front.
// A full list drops the oldest.
void rememberNetwork(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return;
  loadWifiNets();
  WifiList updated = {};
  copyWifiField(updated.slot[0].ssid, WIFI_SSID_LEN, ssid);
  copyWifiField(updated.slot[0].pass, WIFI_PASS_LEN, pass);
  updated.count = 1;
  for (uint8_t i = 0; i < wifiNets.count && updated.count < WIFI_SLOTS; i++) {
    if (strcmp(wifiNets.slot[i].ssid, updated.slot[0].ssid) == 0) continue;
    updated.slot[updated.count++] = wifiNets.slot[i];
  }
  wifiNets = updated;
  prefs.putBytes("wifinets", &wifiNets, sizeof wifiNets);
  Serial.printf("WiFi saved: %s (%d/%d)\n", wifiNets.slot[0].ssid, wifiNets.count, WIFI_SLOTS);
}

// The previous firmware kept a single network in the WiFi stack. Keep it as slot 0.
void importStaIfEmpty() {
  loadWifiNets();
  if (wifiNets.count > 0) return;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return;
  char ssid[WIFI_SSID_LEN + 1];
  char pass[WIFI_PASS_LEN + 1];
  memcpy(ssid, conf.sta.ssid, WIFI_SSID_LEN);
  ssid[WIFI_SSID_LEN] = 0;
  memcpy(pass, conf.sta.password, WIFI_PASS_LEN);
  pass[WIFI_PASS_LEN] = 0;
  if (!ssid[0]) return;
  Serial.printf("WiFi imported: %s\n", ssid);
  rememberNetwork(ssid, pass);
  WiFi.disconnect(false, false);
}

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool connectSaved(const char* ssid, const char* pass, int rssi) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(ssid, pass);
  uint32_t start = millis();
  bool ok = false;
  while (millis() - start < WIFI_ATTEMPT_MS) {
    if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
    delay(100);
  }
  Serial.printf("WiFi attempt: %s rssi %d %s\n", ssid, rssi, ok ? "connected" : "failed");
  if (!ok) WiFi.disconnect(false, false);
  else Serial.printf("WiFi: %s\n", WiFi.SSID().c_str());
  return ok;
}

bool wifiUp() {
  if (WiFi.status() == WL_CONNECTED) return true;
  importStaIfEmpty();
  if (wifiNets.count == 0) {
    Serial.println("WiFi: no saved networks");
    return false;
  }

  for (uint8_t i = 0; i < wifiNets.count; i++)
    Serial.printf("WiFi list: %s\n", wifiNets.slot[i].ssid);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(200);
  int found = WiFi.scanNetworks();
  if (found == 0) {
    delay(400);
    found = WiFi.scanNetworks();
  }
  if (found < 0) {
    Serial.printf("WiFi scan failed: %d\n", found);
    return false;
  }
  Serial.printf("WiFi scan: %d\n", found);

  int slotAt[WIFI_SLOTS];
  int rssiAt[WIFI_SLOTS];
  int hits = 0;
  for (uint8_t i = 0; i < wifiNets.count; i++) {
    bool seen = false;
    int best = 0;
    for (int j = 0; j < found; j++) {
      if (strcmp(WiFi.SSID(j).c_str(), wifiNets.slot[i].ssid) != 0) continue;
      int rssi = WiFi.RSSI(j);
      if (!seen || rssi > best) best = rssi;
      seen = true;
    }
    if (!seen) {
      Serial.printf("WiFi: %s not visible\n", wifiNets.slot[i].ssid);
      continue;
    }
    slotAt[hits] = i;
    rssiAt[hits] = best;
    hits++;
  }
  WiFi.scanDelete();

  for (int a = 1; a < hits; a++) {
    int slot = slotAt[a];
    int rssi = rssiAt[a];
    int b = a;
    while (b > 0 && rssiAt[b - 1] < rssi) {
      slotAt[b] = slotAt[b - 1];
      rssiAt[b] = rssiAt[b - 1];
      b--;
    }
    slotAt[b] = slot;
    rssiAt[b] = rssi;
  }

  if (!hits) {
    Serial.println("WiFi: no saved network visible");
    return false;
  }
  for (int i = 0; i < hits; i++) {
    WifiSlot net = wifiNets.slot[slotAt[i]];
    if (connectSaved(net.ssid, net.pass, rssiAt[i])) return true;
  }
  return false;
}

void syncClock() {
  if (!hasRtc || clockSynced) return;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm tmv;
  if (getLocalTime(&tmv, 5000)) {
    rtc.adjust(DateTime((uint32_t)time(nullptr)));
    clockSynced = true;
    Serial.println("RTC synced to NTP (UTC)");
  }
}

// returns HTTP code, or -404 if the frame isn't on the card
int postFrame(int n) {
  File f = SD_MMC.open(framePath(n));
  if (!f) return -404;
  uint32_t ts = frameTime(n);

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(client, http, URL_UPLOAD, 30000)) { f.close(); return -1; }
  http.addHeader("Content-Type", "image/jpeg");
  addRollHeaders(http);
  http.addHeader("x-frame", String(n));
  http.addHeader("x-captured-at", String((unsigned long)ts));

  int code = http.sendRequest("POST", &f, f.size());
  String body = http.getString();
  logExchange("POST", URL_UPLOAD, http, code, body);
  http.end();
  f.close();
  return code;
}

int postComplete() {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(client, http, URL_UPLOAD, 15000)) return -1;
  addRollHeaders(http);
  http.addHeader("x-roll-complete", "1");
  int code = http.POST("");
  String body = http.getString();
  logExchange("POST", URL_UPLOAD, http, code, body);
  http.end();
  return code;
}

void failToWaiting() {
  mode = WAITING_WIFI;
  lastRetryAt = millis();
  uiDirty = true;
}

void failNotPaired() {
  unpaired = true;
  Serial.println("[LCD] not paired | hold shutter 8s");
  failToWaiting();
}

void develop() {
  uiDirty = true; drawUi();
  if (!wifiUp()) {
    Serial.println("No WiFi");
    unpaired = false;
    failToWaiting();
    return;
  }
  syncClock();

  for (int n = uploaded + 1; n <= framesShot; n++) {
    uiDirty = true; drawUi();
    int code = postFrame(n);
    Serial.printf("Frame %d -> %d\n", n, code);
    if (code == -404) {
      Serial.printf("Frame %d missing on SD, skipping\n", n);
    } else if (code == 403) {
      failNotPaired();
      return;
    } else if (code < 200 || code >= 300) {
      unpaired = false;
      failToWaiting();
      return;
    }
    uploaded = n;
    saveState();
  }

  int code = postComplete();
  Serial.printf("Roll complete -> %d\n", code);
  if (code == 403) {
    failNotPaired();
  } else if (code >= 200 && code < 300) {
    prefs.putBool("done", true);
    unpaired = false;
    mode = SENT;
    wifiOff();
    uiDirty = true;
  } else {
    unpaired = false;
    failToWaiting();
  }
}

void settleAfterSetup() {
  noInterrupts();
  encSteps = 0;
  shutterFell = false;
  interrupts();
  encSub = 0;
  shutterDown = digitalRead(PIN_SHUTTER) == LOW;
  shutterChangedAt = millis();
  shutterPressedAt = millis();
  holdHint = HINT_NONE;
  // The 8 s hold already opened setup. The rest of that press is not a new gesture.
  shutterGesture = shutterDown ? GESTURE_CONSUMED : GESTURE_NONE;
}

bool requestPairCode(String& code) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(client, http, URL_PAIR_START, 15000)) return false;
  addAuthHeaders(http);
  int status = http.POST("");
  String body = http.getString();
  logExchange("POST", URL_PAIR_START, http, status, body);
  http.end();
  if (status < 200 || status >= 300) return false;
  return extractPairCode(body, code);
}

bool pollPaired(bool& paired) {
  paired = false;
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(client, http, URL_STATUS, 15000)) return false;
  addAuthHeaders(http);
  int status = http.GET();
  String body = http.getString();
  logExchange("GET", URL_STATUS, http, status, body);
  http.end();
  if (status < 200 || status >= 300) return false;
  String value;
  if (!extractJsonValue(body, "paired", value)) return false;
  paired = value == "true";
  return true;
}

bool waitForPaired() {
  uint32_t start = millis();
  const uint32_t limit = 10UL * 60UL * 1000UL;
  while (millis() - start < limit) {
    bool paired = false;
    if (pollPaired(paired) && paired) return true;
    uint32_t waitStart = millis();
    while (millis() - waitStart < 3000 && millis() - start < limit) delay(50);
  }
  return false;
}

void showWifiSetupScreen() {
  if (hasLcd) drawStatus("wifi setup", setupSsid);
  Serial.printf("[LCD] wifi setup | %s\n", setupSsid);
}

void finishSetup(const char* message, bool resumeUpload) {
  if (resumeUpload && (mode == WAITING_WIFI || mode == DEVELOPING)) resumeAfterMessage = true;
  flashMessage(message, "", 1500);
  wifiOff();
  settleAfterSetup();
  uiDirty = true;
}

void failSetup() {
  wifiOff();
  flashMessage("pair failed", "", 1500);
  settleAfterSetup();
  uiDirty = true;
}

// The press that opened setup is still down. After it has been released,
// the next press leaves the portal.
bool setupTapCancel(bool& armed, uint32_t& changedAt, bool& stableDown) {
  bool down = digitalRead(PIN_SHUTTER) == LOW;
  uint32_t now = millis();
  if (down != stableDown) {
    stableDown = down;
    changedAt = now;
    return false;
  }
  if (!armed) {
    if (!stableDown && now - changedAt >= 150) armed = true;
    return false;
  }
  return stableDown && now - changedAt >= 30;
}

void runWifiSetup() {
  importStaIfEmpty();
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(setupSsid, sizeof setupSsid, "Figicam-%02X%02X", mac[4], mac[5]);
  showWifiSetupScreen();
  Serial.println("WiFi setup: tap to cancel");

  char portalSsid[WIFI_SSID_LEN + 1] = {0};
  char portalPass[WIFI_PASS_LEN + 1] = {0};

  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  wm.setConfigPortalBlocking(false);
  wm.setAPCallback([](WiFiManager*) { showWifiSetupScreen(); });
  wm.setSaveConfigCallback([&]() {
    String ssid = wm.getWiFiSSID(true);
    String pass = wm.getWiFiPass(true);
    if (!ssid.length()) ssid = WiFi.SSID();
    copyWifiField(portalSsid, WIFI_SSID_LEN, ssid.c_str());
    copyWifiField(portalPass, WIFI_PASS_LEN, pass.c_str());
  });

  wm.startConfigPortal(setupSsid);
  bool armed = false;
  bool stableDown = digitalRead(PIN_SHUTTER) == LOW;
  uint32_t changedAt = millis();
  bool cancelled = false;
  while (wm.getConfigPortalActive()) {
    if (wm.process()) break;
    if (setupTapCancel(armed, changedAt, stableDown)) {
      cancelled = true;
      Serial.println("WiFi setup cancelled");
      wm.stopConfigPortal();
      break;
    }
    delay(10);
  }

  if (cancelled) {
    wifiOff();
    settleAfterSetup();
    uiDirty = true;
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi setup ended without a connection");
    failSetup();
    return;
  }
  Serial.printf("WiFi: %s\n", WiFi.SSID().c_str());
  if (!portalSsid[0]) copyWifiField(portalSsid, WIFI_SSID_LEN, WiFi.SSID().c_str());
  if (!portalPass[0]) copyWifiField(portalPass, WIFI_PASS_LEN, wm.getWiFiPass(true).c_str());
  rememberNetwork(portalSsid, portalPass);

  bool paired = false;
  if (pollPaired(paired) && paired) {
    prefs.putBool("paired", true);
    unpaired = false;
    finishSetup("wifi saved", true);
    return;
  }

  String code;
  if (!requestPairCode(code)) {
    failSetup();
    return;
  }
  drawPairCode(code.c_str());
  Serial.printf("[LCD] %s | enter on site\n", code.c_str());

  if (!waitForPaired()) {
    failSetup();
    return;
  }

  prefs.putBool("paired", true);
  unpaired = false;
  finishSetup("paired", true);
}

// ============================================================
//  Inputs
// ============================================================
void onClick() {
  if (rewindAsk) { rewindAsk = false; uiDirty = true; return; }
  if (mode != SHOOTING || wound || drumAnimating || closed || framesShot >= FRAMES_PER_ROLL) return;
  clicks++;
  uiDirty = true;
  if (clicks >= CLICKS_TO_WIND) {
    clicks = 0;
    wound = true;
    drumFrom = framesShot;
    drumStart = millis();
    drumAnimating = true;
    retryShown = false;
    saveState();
    Serial.println("Wound - ready");
  }
}

static const DRAM_ATTR int8_t ENC_STEP[16] = {
  0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0
};

void IRAM_ATTR onEncoder() {
  uint8_t cur = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  int8_t d = ENC_STEP[(encPrev << 2) | cur];
  encPrev = cur;
  if (ENC_REVERSE) d = -d;
  encSteps += d;
}

void pollEncoder() {
  noInterrupts();
  int n = encSteps;
  encSteps = 0;
  interrupts();
  if (!n) return;
  encSub += n;
  while (encSub >= ENC_STEPS_PER_CLICK) {  // whole clicks only; keep the remainder
    encSub -= ENC_STEPS_PER_CLICK;
    onClick();
  }
  if (encSub <= -ENC_STEPS_PER_CLICK) encSub = 0;  // backwards: the ratchet ignores it
}

void IRAM_ATTR onShutterFall() {
  shutterFellAt = millis();
  shutterFell = true;
}

void shootFrame() {
  drumAnimating = false;
  if (!hasCam || !hasSd) {
    flashMessage("CAN'T SHOOT", !hasCam ? "NO CAMERA" : "NO SD CARD", 1500);
    return;
  }
  if (hasLcd) { oled.clearBuffer(); oled.sendBuffer(); }
  delay(120);                            // number blinks off before the capture
  retryShown = false;
  drawDial(dialNumber(), 0.f, "processing...");
  if (!takePhoto()) {
    retryShown = true;
    uiDirty = true;
  }
}

void confirmRewind() {
  rewindAsk = false;
  if (framesShot <= 0) { uiDirty = true; return; }
  closed = true;
  wound = false;
  clicks = 0;
  drumAnimating = false;
  mode = DEVELOPING;
  lastRetryAt = 0;
  saveState();
  uiDirty = true;
  Serial.printf("Rewind - uploading %d\n", framesShot);
}

void deleteRollFolder(uint32_t id) {
  char dir[32];
  snprintf(dir, sizeof dir, "/roll_%lu", (unsigned long)id);
  int guard = 0;
  while (guard++ < 64) {
    File root = SD_MMC.open(dir);
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      return;
    }
    File entry = root.openNextFile();
    if (!entry) {
      root.close();
      break;
    }
    String path = entry.path();
    bool isDir = entry.isDirectory();
    entry.close();
    root.close();
    if (!path.length()) break;
    bool ok = isDir ? SD_MMC.rmdir(path) : SD_MMC.remove(path);
    Serial.printf("Removed %s (%s)\n", path.c_str(), ok ? "ok" : "failed");
    if (!ok) return;
    yield();
  }
  if (SD_MMC.rmdir(dir)) Serial.printf("Removed %s\n", dir);
  else Serial.printf("Could not remove %s\n", dir);
}

void loadNewRoll() {
  bool done = prefs.getBool("done", false);
  uint32_t oldRoll = rollId;
  if (done && hasSd && SD_MMC.exists(rollDir())) {
    if (hasLcd) drawStatus("clearing...", nullptr);
    Serial.println("[LCD] clearing...");
    deleteRollFolder(oldRoll);
  }
  startNewRoll();
}

void showHoldHint(HoldHint hint) {
  holdHint = hint;
  messageUntil = 0;
  overlayShown = false;
  uiDirty = true;
  Serial.printf("[LCD] %s\n", hint == HINT_REWIND ? "release: rewind" : "release: new roll");
}

void onShutterPress(uint32_t edgeAt) {
  uint32_t now = millis();
  shutterPressedAt = (uint32_t)(now - edgeAt) < 500 ? edgeAt : now;
  holdHint = HINT_NONE;

  if (rewindAsk) {
    shutterGesture = GESTURE_CONSUMED;
    if ((uint32_t)(now - rewindAskAt) <= REWIND_CONFIRM_MS) confirmRewind();
    else { rewindAsk = false; uiDirty = true; }
    return;
  }
  // Already wound: the photo happens on the press. The rest of the hold is ignored.
  if (mode == SHOOTING && wound) {
    shutterGesture = GESTURE_CONSUMED;
    shootFrame();
    return;
  }
  shutterGesture = GESTURE_HOLD;
}

void onShutterRelease(uint32_t now) {
  Gesture g = shutterGesture;
  uint32_t held = now - shutterPressedAt;
  shutterGesture = GESTURE_NONE;
  if (holdHint != HINT_NONE) uiDirty = true;
  holdHint = HINT_NONE;
  if (g != GESTURE_HOLD) return;

  if (held >= HOLD_WIFI_MS) return;        // setup already started while it was held
  if (held >= HOLD_HINT_MS) {
    if (mode == SHOOTING && framesShot >= 1) {
      rewindAsk = true;
      rewindAskAt = now;
      uiDirty = true;
      Serial.println("[LCD] rewind? | tap to confirm");
    } else if (mode == SENT) {
      loadNewRoll();
    }
    return;
  }
  if (mode == SHOOTING) flashMessage("WIND", "", 600);
  else if (mode == WAITING_WIFI) lastRetryAt = 0;
}

void serviceHold(uint32_t now) {
  if (mode == SHOOTING && wound) {         // wound while the button was down: not a hold gesture
    shutterGesture = GESTURE_CONSUMED;
    if (holdHint != HINT_NONE) uiDirty = true;
    holdHint = HINT_NONE;
    return;
  }
  uint32_t held = now - shutterPressedAt;
  if (held >= HOLD_WIFI_MS) {
    shutterGesture = GESTURE_CONSUMED;
    holdHint = HINT_NONE;
    runWifiSetup();
    return;
  }
  if (held < HOLD_HINT_MS || holdHint != HINT_NONE) return;
  if (mode == SHOOTING && framesShot >= 1) showHoldHint(HINT_REWIND);
  else if (mode == SENT) showHoldHint(HINT_NEW);
}

void pollShutter() {
  uint32_t now = millis();
  bool edge = false;
  uint32_t edgeAt = 0;
  if (shutterFell) {
    noInterrupts();
    edge = shutterFell;
    edgeAt = shutterFellAt;
    shutterFell = false;
    interrupts();
  }

  bool down = digitalRead(PIN_SHUTTER) == LOW;
  if (shutterDown && !down && (uint32_t)(now - shutterChangedAt) > 30) {
    shutterDown = false;
    if (!edge) shutterChangedAt = now;
    onShutterRelease(now);
  }

  // 30 ms debounce. A press that landed while we were busy is still in the flag.
  if (edge && !shutterDown && (uint32_t)(edgeAt - shutterChangedAt) > 30) {
    shutterDown = true;
    shutterChangedAt = edgeAt;
    onShutterPress(edgeAt);
    bool still = digitalRead(PIN_SHUTTER) == LOW;
    noInterrupts();
    shutterFell = false;                     // bounce during the shot is not another press
    interrupts();
    if (!still) {                            // released while the shot was saving, or a finished tap
      shutterDown = false;
      shutterChangedAt = millis();
      onShutterRelease(millis());
    }
  }

  if (shutterDown && shutterGesture == GESTURE_HOLD) serviceHold(millis());
}

// ============================================================
//  Arduino entry points
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Figicam v1 ===");
#if CAMERA_ID_FROM_MAC
  initCameraId();
#endif
  Serial.printf("camera id: %s\n", CAMERA_ID);

  pinMode(PIN_SHUTTER, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  encPrev = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(PIN_ENC_A, onEncoder, CHANGE);
  attachInterrupt(PIN_ENC_B, onEncoder, CHANGE);
  attachInterrupt(PIN_SHUTTER, onShutterFall, FALLING);
  analogSetPinAttenuation(PIN_BATT_SENSE, ADC_11db);

  hasCam = initCamera();                 // camera first: it claims its own I2C port for SCCB
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  uint8_t oledAddr = 0;
  Serial.print("I2C found:");
  for (uint8_t addr = 1; addr <= 126; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", addr);
      if (addr == 0x3C || (addr == 0x3D && oledAddr == 0)) oledAddr = addr;
    }
  }
  Serial.println();
  Wire.end();   // u8g2 begin() pinModes these pins; let it attach the driver itself

  hasLcd = oledAddr != 0;
  if (hasLcd) {
    oled.setI2CAddress(oledAddr * 2);    // u8g2 wants the 8-bit address
    oled.setBusClock(400000);
    oled.begin();
  }

  hasRtc = rtc.begin(&Wire);
  if (hasRtc && rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  hasSd = initSd();

  Serial.printf("camera:%d  sd:%d  rtc:%d  lcd:%d  psram:%u bytes\n",
                hasCam, hasSd, hasRtc, hasLcd, ESP.getPsramSize());

#if RESET_ROLL_ON_BOOT
  prefs.begin("latent", false); prefs.clear(); prefs.end();
#endif
  loadState();

  if (!hasCam)     flashMessage("NO CAMERA", "CHECK RIBBON", 3000);
  else if (!hasSd) flashMessage("NO SD CARD", "INSERT + REBOOT", 3000);
  sampleBattery();
  if (!prefs.getBool("paired", false)) {
    if (messageUntil > millis()) delay(messageUntil - millis());
    runWifiSetup();
  }
  uiDirty = true;
}

void loop() {
  pollEncoder();
  pollShutter();

  if (drumAnimating && millis() - drumStart >= 250) {
    drumAnimating = false;
    uiDirty = true;
  }
  if (rewindAsk && millis() - rewindAskAt >= REWIND_CONFIRM_MS) {
    rewindAsk = false;
    uiDirty = true;
  }
  if (overlayShown && millis() >= messageUntil) {
    overlayShown = false;
    uiDirty = true;
    if (resumeAfterMessage) {
      resumeAfterMessage = false;
      unpaired = false;
      lastRetryAt = 0;
      mode = DEVELOPING;
    }
  }

  // Hold the "paired" / "pair failed" screen; develop() draws over it.
  if (millis() >= messageUntil) {
    if (mode == DEVELOPING) {
      develop();
    } else if (mode == WAITING_WIFI &&
               (lastRetryAt == 0 || millis() - lastRetryAt > RETRY_MS)) {
      mode = DEVELOPING;
    } else {
      sampleBattery();                   // skip while a frame is saving or uploading
    }
  }

  static uint32_t lastAnimDraw = 0;
  bool animFrame = drumAnimating && millis() - lastAnimDraw >= 33;
  if (millis() < messageUntil) return;
  if (!uiDirty && !animFrame) return;
  if (animFrame) lastAnimDraw = millis();
  uiDirty = false;
  drawUi();
}
