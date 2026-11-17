#pragma once
// ============================================================
//  Figicam v1 — board settings live here. Secrets live in secrets.h.
// ============================================================

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h. Copy include/secrets.example.h to include/secrets.h and set API_KEY."
#endif

#ifndef API_KEY
#error "API_KEY is not defined. Set it in include/secrets.h — see include/secrets.example.h."
#endif

// ---- WiFi ----
// Saved from the phone portal: hold the shutter 8 s, or the first boot
// before this camera has paired. Up to 5 networks (newest first). 2.4 GHz only.
// A tap on the shutter leaves setup and returns to the counter.

// ---- Lovable darkroom ----
// API_KEY comes from secrets.h. After the first successful pairing the backend
// locks this camera to that key — do not change it.
// CAMERA_ID is optional in secrets.h. If it is not defined, the firmware uses
// "figicam-" plus the last 6 hex digits of the Wi-Fi MAC, set at boot.
#define API_BASE       "https://project--72c6fdf9-c335-4574-89fe-bfa19ac9a329.lovable.app/api/public"
#define URL_PAIR_START API_BASE "/camera-pair-start"
#define URL_STATUS     API_BASE "/camera-status"
#define URL_UPLOAD     API_BASE "/upload-frame"

// ---- Roll ----
#define FRAMES_PER_ROLL      27     // set to 3 while testing
#define CLICKS_TO_WIND        8     // thumbwheel detents per advance
#define RESET_ROLL_ON_BOOT    0     // 1 = wipe roll state every boot (testing only)
#define RETRY_MS          60000     // how often to retry uploading without WiFi

// ---- Encoder tuning ----
#define ENC_STEPS_PER_CLICK   4     // if each detent counts as 2 clicks, set to 4; if it takes 2 detents per click, set to 2
#define ENC_REVERSE           0     // 1 if winding the "wrong" way is the one that counts

// ---- Camera ----
#define CAM_XCLK_HZ    20000000     // drop to 16000000 if photos come out corrupt / "NO-EOI"
#define JPEG_QUALITY          8     // 0-63, lower = better + bigger files

// ---- Pins (Freenove ESP32-S3 CAM) ----
#define PIN_BATT_SENSE   1   // ADC1, 100k/100k divider from switched battery +
#define PIN_SHUTTER      3   // Kailh Box Jade -> GND. Don't hold it while switching power on.
#define PIN_ENC_A       14   // EC11 A
#define PIN_ENC_B       21   // EC11 B   (EC11 middle pin C -> GND)
#define PIN_I2C_SDA     41   // DS3231 + OLED
#define PIN_I2C_SCL     42
#define BATT_DIVIDER  2.0f
