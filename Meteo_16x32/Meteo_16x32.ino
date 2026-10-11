// ============================================================
// Meteo_16x32 - ESP32 + 2 pannelli WS2812B 8x32 (tot 32x16)
// OpenWeather (lat/lon) + NTP + Web UI + OTA + NVS
//
// v2 (2026-10-07) - modifiche principali rispetto alla v1:
//  - FIX: riavvio ogni 3 minuti dopo aver chiuso la pagina web
//         (watchdog su /state rimosso, sostituito da controlli reali)
//  - FIX: fasi lunari mai visibili (maschera nera disegnata in modo
//         additivo = nessun effetto). Ora fase calcolata su ciclo
//         sinodico reale e disegnata pixel per pixel.
//  - FIX: icone disegnate in modo additivo (nuvole sature/bianche,
//         nuvola "trasparente" sopra il sole). Ora i layer coprono.
//  - FIX: possibile panic del watchdog durante OTA e durante il
//         fetch meteo (WiFiClient::setTimeout in secondi su core 2.x)
//         -> HTTPClient con timeout in ms + reset WDT.
//  - FIX: confronti millis() non sicuri all'overflow (49 giorni).
//  - Render solo al cambio frame (non piu' 30 fps continui).
//  - v2.1: icone pixel-art 16x16 ridisegnate, animazione a 8 passi.
//  - v2.2: icone sempre intere nel riquadro 16x16, posizione icona limitata (X 0..16, Y 0).
//  - Ultimi dati meteo mantenuti a display se il fetch fallisce
//    (diventano "--" solo dopo 3 intervalli o 30 min).
//  - Salvataggio NVS ritardato (meno scritture flash con slider).
//  - UI: slider non "saltano" durante il trascinamento, auto-reset
//    configurabile, pulsante "Aggiorna meteo", temp con decimale,
//    fase luna, uptime.
//  - mDNS: http://meteo16x32.local
//  - Credenziali opzionali in secrets.h (stessa cartella).
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <FastLED.h>
#include <time.h>
#include <math.h>
#include <pgmspace.h>
#include "esp_task_wdt.h"
#include <ArduinoOTA.h>

// Facoltativo: crea secrets.h nella cartella dello sketch con i #define
// WIFI_SSID, WIFI_PASS, OW_API_KEY_DEFAULT, OW_LAT_DEFAULT, OW_LON_DEFAULT
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "Your ssid"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "Your wifi password"
#endif
#ifndef OTA_HOSTNAME
#define OTA_HOSTNAME "Meteo16x32"
#endif
#ifndef OW_API_KEY_DEFAULT
#define OW_API_KEY_DEFAULT "Your OpenWeather API key"
#endif
#ifndef OW_LAT_DEFAULT
#define OW_LAT_DEFAULT "Your latitude"
#endif
#ifndef OW_LON_DEFAULT
#define OW_LON_DEFAULT "Your longitude"
#endif

// ---------------- Hardware ----------------
constexpr uint8_t PANEL_W = 32;
constexpr uint8_t PANEL_H = 8;
constexpr uint8_t MATRIX_W = 32;
constexpr uint8_t MATRIX_H = 16;
constexpr uint8_t ICON_SIZE = 16;
constexpr uint8_t ICON_MAX_X = MATRIX_W - ICON_SIZE;  // icona sempre intera
constexpr uint8_t ICON_MAX_Y = MATRIX_H - ICON_SIZE;
constexpr uint16_t PANEL_LEDS = PANEL_W * PANEL_H;
constexpr uint16_t NUM_LEDS = MATRIX_W * MATRIX_H;

constexpr uint8_t DATA_PIN = 5;
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
constexpr uint32_t PSU_MAX_MA = 2000;

CRGB leds[NUM_LEDS];
WebServer server(80);
Preferences prefs;

// ---------------- Impostazioni (NVS) ----------------
uint8_t g_brightness = 18;  // 1..255
CRGB g_colTemp = CRGB(255, 80, 0);
CRGB g_colHum = CRGB(0, 160, 255);
CRGB g_colBg = CRGB::Black;

uint8_t g_nightBrightnessMin = 5;
uint8_t g_nightBrightnessPct = 35;
uint8_t g_nightDimPct = 60;
uint8_t g_animSpeedPct = 45;

uint16_t g_dayStartMin = 7 * 60;
uint16_t g_nightStartMin = 21 * 60;
uint16_t g_owIntervalMin = 10;
uint16_t g_autoResetHours = 0;
uint8_t g_iconPosX = 0;
uint8_t g_iconPosY = 0;

// ---------------- Stato runtime ----------------
bool g_isNight = false;
bool g_forceNightPreview = false;
unsigned long g_forceNightStartMs = 0;
constexpr unsigned long FORCE_NIGHT_MS = 15000UL;

float g_tempC = NAN;
int g_humidity = -1;
char g_icon[4] = "03d";
bool g_meteoOk = false;      // esito ultimo fetch
bool g_haveMeteo = false;    // almeno un fetch riuscito
unsigned long g_meteoLastOkMs = 0;
unsigned long meteoLastTryMs = 0;
bool g_meteoForce = false;
uint8_t g_meteoFailStreak = 0;
int g_lastHttpCode = 0;

bool g_dirty = true;
bool g_prefsDirty = false;
unsigned long g_prefsDirtyMs = 0;
constexpr unsigned long PREFS_SAVE_DELAY_MS = 3000UL;

unsigned long bootMs = 0;
unsigned long lastWifiOkMs = 0;
unsigned long lastWifiTryMs = 0;
unsigned long lastHealthMs = 0;
bool g_otaStarted = false;
bool g_otaRunning = false;

constexpr unsigned long HEALTH_PERIOD_MS = 5000UL;
constexpr unsigned long WIFI_GRACE_MS = 30000UL;            // dopo 30 s offline: reconnect "duro"
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000UL;
constexpr unsigned long WIFI_OFFLINE_RESTART_MS = 15UL * 60UL * 1000UL; // 15 min offline -> reboot
constexpr uint8_t METEO_FAIL_RESTART = 30;                   // 30 fetch falliti con WiFi OK -> reboot
constexpr unsigned long METEO_RETRY_MS = 60000UL;

String OW_API_KEY = OW_API_KEY_DEFAULT;
String OW_LAT = OW_LAT_DEFAULT;
String OW_LON = OW_LON_DEFAULT;
const char* OW_LANG = "it";
const char* OW_UNITS = "metric";

enum WeatherIconKind : uint8_t {
  WICON_CLEAR,
  WICON_FEW_CLOUDS,
  WICON_CLOUDY,
  WICON_OVERCAST,
  WICON_FOG,
  WICON_RAIN,
  WICON_HEAVY_RAIN,
  WICON_THUNDER,
  WICON_SNOW
};

// ============================================================
// Utility
// ============================================================
static inline bool elapsed(const unsigned long since, const unsigned long ms) {
  return (millis() - since) >= ms;  // sicuro all'overflow
}

static inline uint8_t hex2b(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return 0;
}

CRGB parseHexColor(const String& s, const CRGB fallback) {
  if (s.length() != 7 || s[0] != '#') return fallback;
  const uint8_t r = (hex2b(s[1]) << 4) | hex2b(s[2]);
  const uint8_t g = (hex2b(s[3]) << 4) | hex2b(s[4]);
  const uint8_t b = (hex2b(s[5]) << 4) | hex2b(s[6]);
  return CRGB(r, g, b);
}

static inline void toHexColorBuf(const CRGB& c, char out[8]) {
  snprintf(out, 8, "#%02X%02X%02X", c.r, c.g, c.b);
}

static inline CRGB scaleColorPct(CRGB in, uint8_t pct) {
  if (pct > 100) pct = 100;
  in.nscale8_video(static_cast<uint8_t>((static_cast<uint16_t>(pct) * 255U) / 100U));
  return in;
}

static inline uint8_t brPctFrom255(const uint8_t b) {
  uint16_t p = (static_cast<uint16_t>(b) * 100U + 127U) / 255U;
  return static_cast<uint8_t>(constrain(p, 1, 100));
}

static inline uint8_t br255FromPct(uint8_t p) {
  p = constrain(p, 1, 100);
  uint16_t b = (static_cast<uint16_t>(p) * 255U + 50U) / 100U;
  return static_cast<uint8_t>(constrain(b, 1, 255));
}

bool timeIsValid() { return time(nullptr) > 1700000000; }

static inline void minToHHMMBuf(const uint16_t m, char out[6]) {
  snprintf(out, 6, "%02u:%02u", static_cast<unsigned>((m / 60) % 24), static_cast<unsigned>(m % 60));
}

static inline uint16_t hhmmToMinSafe(const String& hhmm, const uint16_t fallback) {
  if (hhmm.length() < 5 || hhmm[2] != ':') return fallback;
  const int hh = hhmm.substring(0, 2).toInt();
  const int mm = hhmm.substring(3, 5).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return fallback;
  return static_cast<uint16_t>(hh * 60 + mm);
}

// ============================================================
// Mapping matrice (serpentina verticale, pannello basso ruotato 180°)
// ============================================================
static inline uint16_t XY_panel(const uint8_t x, const uint8_t y) {
  const uint8_t yy = (x & 1) ? (PANEL_H - 1 - y) : y;
  return static_cast<uint16_t>(x) * PANEL_H + yy;
}

uint16_t XY(uint8_t x, uint8_t y) {
  const uint8_t panel = (y >= PANEL_H) ? 1 : 0;
  if (panel) {
    y -= PANEL_H;
    x = (PANEL_W - 1) - x;
    y = (PANEL_H - 1) - y;
  }
  return static_cast<uint16_t>(panel) * PANEL_LEDS + XY_panel(x, y);
}

// Scrive un pixel (sovrascrive) con clipping
static inline void plot(const int x, const int y, const CRGB& c) {
  if (x < 0 || x >= MATRIX_W || y < 0 || y >= MATRIX_H) return;
  leds[XY(static_cast<uint8_t>(x), static_cast<uint8_t>(y))] = c;
}

// ============================================================
// Giorno / notte e luminosita'
// ============================================================
bool isNightBySchedule(const uint16_t nowMin) {
  if (g_dayStartMin == g_nightStartMin) return false;
  if (g_dayStartMin < g_nightStartMin) return (nowMin >= g_nightStartMin) || (nowMin < g_dayStartMin);
  return (nowMin >= g_nightStartMin) && (nowMin < g_dayStartMin);
}

void updateNightFlag() {
  if (g_forceNightPreview) {
    if (elapsed(g_forceNightStartMs, FORCE_NIGHT_MS)) {
      g_forceNightPreview = false;
      g_dirty = true;
    } else {
      g_isNight = true;
      return;
    }
  }
  if (timeIsValid()) {
    struct tm t {};
    const time_t now = time(nullptr);
    localtime_r(&now, &t);
    g_isNight = isNightBySchedule(static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min));
  } else {
    g_isNight = (g_icon[2] == 'n');
  }
}

static inline uint8_t effectiveBrightness() {
  if (!g_isNight) return g_brightness;
  uint16_t b = (static_cast<uint16_t>(g_brightness) * g_nightBrightnessPct + 50U) / 100U;
  if (b < g_nightBrightnessMin) b = g_nightBrightnessMin;
  return static_cast<uint8_t>(constrain(b, 1, 255));
}

// ============================================================
// NVS
// ============================================================
void loadPrefs() {
  prefs.begin("meteo16x32", true);
  g_brightness = prefs.getUChar("br", g_brightness);
  g_colTemp = CRGB(prefs.getUChar("tr", g_colTemp.r), prefs.getUChar("tg", g_colTemp.g), prefs.getUChar("tb", g_colTemp.b));
  g_colHum = CRGB(prefs.getUChar("hr", g_colHum.r), prefs.getUChar("hg", g_colHum.g), prefs.getUChar("hb", g_colHum.b));
  g_colBg = CRGB(prefs.getUChar("br0", g_colBg.r), prefs.getUChar("bg0", g_colBg.g), prefs.getUChar("bb0", g_colBg.b));
  g_dayStartMin = prefs.getUShort("ds", g_dayStartMin);
  g_nightStartMin = prefs.getUShort("ns", g_nightStartMin);
  g_owIntervalMin = prefs.getUShort("owm", g_owIntervalMin);
  g_autoResetHours = prefs.getUShort("arh", g_autoResetHours);
  g_iconPosX = prefs.getUChar("ix", g_iconPosX);
  g_iconPosY = prefs.getUChar("iy", g_iconPosY);
  g_nightBrightnessPct = prefs.getUChar("nbp", g_nightBrightnessPct);
  g_nightBrightnessMin = prefs.getUChar("nbm", g_nightBrightnessMin);
  g_nightDimPct = prefs.getUChar("ndp", g_nightDimPct);
  g_animSpeedPct = prefs.getUChar("asp", g_animSpeedPct);
  prefs.end();

  g_dayStartMin = min<uint16_t>(g_dayStartMin, 1439);
  g_nightStartMin = min<uint16_t>(g_nightStartMin, 1439);
  g_owIntervalMin = constrain(g_owIntervalMin, 1, 120);
  g_autoResetHours = min<uint16_t>(g_autoResetHours, 720);
  g_nightBrightnessPct = constrain(g_nightBrightnessPct, 1, 100);
  g_nightBrightnessMin = constrain(g_nightBrightnessMin, 1, 80);
  g_nightDimPct = constrain(g_nightDimPct, 1, 100);
  g_animSpeedPct = constrain(g_animSpeedPct, 1, 100);
  g_iconPosX = min<uint8_t>(g_iconPosX, ICON_MAX_X);
  g_iconPosY = min<uint8_t>(g_iconPosY, ICON_MAX_Y);
  if (g_brightness < 1) g_brightness = 1;
}

void savePrefs() {
  prefs.begin("meteo16x32", false);
  prefs.putUChar("br", g_brightness);
  prefs.putUChar("tr", g_colTemp.r); prefs.putUChar("tg", g_colTemp.g); prefs.putUChar("tb", g_colTemp.b);
  prefs.putUChar("hr", g_colHum.r);  prefs.putUChar("hg", g_colHum.g);  prefs.putUChar("hb", g_colHum.b);
  prefs.putUChar("br0", g_colBg.r);  prefs.putUChar("bg0", g_colBg.g);  prefs.putUChar("bb0", g_colBg.b);
  prefs.putUShort("ds", g_dayStartMin); prefs.putUShort("ns", g_nightStartMin);
  prefs.putUShort("owm", g_owIntervalMin); prefs.putUShort("arh", g_autoResetHours);
  prefs.putUChar("ix", g_iconPosX); prefs.putUChar("iy", g_iconPosY);
  prefs.putUChar("nbp", g_nightBrightnessPct); prefs.putUChar("nbm", g_nightBrightnessMin);
  prefs.putUChar("ndp", g_nightDimPct); prefs.putUChar("asp", g_animSpeedPct);
  prefs.end();
  g_prefsDirty = false;
}

void markPrefsDirty() {
  g_prefsDirty = true;
  g_prefsDirtyMs = millis();
}

void handleDeferredSave() {
  if (g_prefsDirty && elapsed(g_prefsDirtyMs, PREFS_SAVE_DELAY_MS)) savePrefs();
}

// ============================================================
// OpenWeather
// ============================================================
static bool extractJsonNumberC(const char* s, const char* key, float& outVal) {
  const char* k = strstr(s, key);
  if (!k) return false;
  k = strchr(k, ':');
  if (!k) return false;
  k++;
  while (*k == ' ') k++;
  char* endptr = nullptr;
  outVal = strtof(k, &endptr);
  return endptr && endptr != k;
}

static bool extractIconC(const char* s, char iconOut[4]) {
  const char* k = strstr(s, "\"icon\"");
  if (!k) return false;
  k = strchr(k, ':');
  if (!k) return false;
  k = strchr(k, '"');
  if (!k) return false;
  k++;
  if (strlen(k) < 3) return false;
  memcpy(iconOut, k, 3);
  iconOut[3] = '\0';
  return true;
}

bool fetchMeteo() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (OW_API_KEY.length() < 10) return false;

  esp_task_wdt_reset();
  const String url = String("http://api.openweathermap.org/data/2.5/weather?lat=") + OW_LAT + "&lon=" + OW_LON +
                     "&appid=" + OW_API_KEY + "&units=" + OW_UNITS + "&lang=" + OW_LANG;

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(4000);  // ms
  http.setTimeout(4000);         // ms
  http.setReuse(false);
  if (!http.begin(client, url)) {
    g_lastHttpCode = -1;
    return false;
  }
  const int code = http.GET();
  g_lastHttpCode = code;
  esp_task_wdt_reset();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();
  esp_task_wdt_reset();

  float t, h;
  char ic[4];
  const char* b = body.c_str();
  if (!extractJsonNumberC(b, "\"temp\"", t)) return false;
  if (!extractJsonNumberC(b, "\"humidity\"", h)) return false;
  if (!extractIconC(b, ic)) return false;

  g_tempC = t;
  g_humidity = constrain(static_cast<int>(lroundf(h)), 0, 100);
  memcpy(g_icon, ic, 4);
  g_meteoLastOkMs = millis();
  g_haveMeteo = true;
  return true;
}

// I dati restano validi per 3 intervalli (minimo 30 minuti)
bool meteoFresh() {
  if (!g_haveMeteo) return false;
  unsigned long maxAge = static_cast<unsigned long>(g_owIntervalMin) * 3UL * 60000UL;
  if (maxAge < 30UL * 60000UL) maxAge = 30UL * 60000UL;
  return !elapsed(g_meteoLastOkMs, maxAge);
}

void handleMeteo() {
  const unsigned long intervalMs = g_meteoOk ? static_cast<unsigned long>(g_owIntervalMin) * 60000UL : METEO_RETRY_MS;
  if (!g_meteoForce && !elapsed(meteoLastTryMs, intervalMs)) return;
  if (WiFi.status() != WL_CONNECTED) return;  // riprova appena torna il WiFi

  g_meteoForce = false;
  meteoLastTryMs = millis();
  g_meteoOk = fetchMeteo();
  if (g_meteoOk) g_meteoFailStreak = 0;
  else if (g_meteoFailStreak < 250) g_meteoFailStreak++;
  Serial.printf("[Meteo] %s (HTTP %d) T=%.1f H=%d icona=%s\n", g_meteoOk ? "OK" : "ERRORE", g_lastHttpCode,
                g_tempC, g_humidity, g_icon);
  updateNightFlag();
  g_dirty = true;
}

// ============================================================
// Luna (ciclo sinodico reale)
// ============================================================
// Restituisce la fase 0..1 (0 = nuova, 0.5 = piena)
float moonPhase01() {
  if (!timeIsValid()) return 0.5f;
  constexpr double SYNODIC_DAYS = 29.530588853;
  constexpr double REF_NEW_MOON = 947182440.0;  // 2000-01-06 18:14 UTC
  const double days = (static_cast<double>(time(nullptr)) - REF_NEW_MOON) / 86400.0;
  double p = fmod(days / SYNODIC_DAYS, 1.0);
  if (p < 0) p += 1.0;
  return static_cast<float>(p);
}

uint8_t moonIllumPct() {
  return static_cast<uint8_t>(lroundf((1.0f - cosf(moonPhase01() * 2.0f * PI)) * 50.0f));
}

// ============================================================
// Icone 16x16 - pixel-art + animazione a 8 passi (step 0..7)
// ==== ICONS BEGIN
// ============================================================
static int g_clipX0 = 0, g_clipY0 = 0, g_clipX1 = 15, g_clipY1 = 15;
static uint8_t g_iconBrPct = 100;  // attenuazione icona (notte)

// Pixel dentro il riquadro icona (clipping) con attenuazione
#ifndef ICON_CLIP_HOOK
#define ICON_CLIP_HOOK()
#endif
static inline void iplot(const int x, const int y, CRGB c) {
  if (x < g_clipX0 || x > g_clipX1 || y < g_clipY0 || y > g_clipY1) { ICON_CLIP_HOOK(); return; }
  if (g_iconBrPct < 100) c = scaleColorPct(c, g_iconBrPct);
  plot(x, y, c);
}

// Toni nuvole: 0 bianca, 1 grigia chiara, 2 grigia scura (pioggia), 3 temporale
static const uint8_t CLOUD_PAL[4][3][3] = {
  {{255, 255, 255}, {190, 200, 220}, {110, 122, 150}},
  {{200, 208, 225}, {145, 155, 178}, {85, 95, 120}},
  {{150, 158, 180}, {105, 113, 135}, {60, 66, 88}},
  {{115, 120, 140}, {75, 80, 100}, {40, 44, 60}},
};

static CRGB palColor(const char ch, const uint8_t tone) {
  switch (ch) {
    case 'W': return CRGB(CLOUD_PAL[tone][0][0], CLOUD_PAL[tone][0][1], CLOUD_PAL[tone][0][2]);
    case 'G': return CRGB(CLOUD_PAL[tone][1][0], CLOUD_PAL[tone][1][1], CLOUD_PAL[tone][1][2]);
    case 'g': return CRGB(CLOUD_PAL[tone][2][0], CLOUD_PAL[tone][2][1], CLOUD_PAL[tone][2][2]);
    case 'O': return CRGB(255, 110, 0);    // bordo sole
    case 'Y': return CRGB(255, 190, 20);   // sole
    case 'y': return CRGB(255, 240, 130);  // riflesso sole
    case 'L': return CRGB(255, 220, 40);   // fulmine
    case 'l': return CRGB(255, 255, 210);  // fulmine flash
    default:  return CRGB::Black;
  }
}

// Disegna uno sprite di stringhe ('.' = trasparente)
static void drawSprite(const int x0, const int y0, const char* const rows[], const uint8_t h, const uint8_t tone) {
  for (uint8_t y = 0; y < h; y++) {
    const char* r = rows[y];
    for (uint8_t x = 0; r[x]; x++)
      if (r[x] != '.') iplot(x0 + x, y0 + y, palColor(r[x], tone));
  }
}

static const char* const SPR_SUN_DISC[8] = {
  "..OOOO..",
  ".OyyYYO.",
  "OyyYYYYO",
  "OyYYYYYO",
  "OYYYYYYO",
  "OYYYYYYO",
  ".OYYYYO.",
  "..OOOO..",
};

static const char* const SPR_CLOUD_BIG[8] = {
  "......WWWW....",
  "....WWGGGGW...",
  "..WWGGGGGGGW..",
  ".WGGGGGGGGGGW.",
  "WGGGGGGGGGGGGW",
  "GGGGGGGGGGGGGG",
  "gGGGGGGGGGGGGg",
  ".gggggggggggg.",
};

static const char* const SPR_SUN_SMALL[6] = {
  ".OOOO.",
  "OyyYYO",
  "OyYYYO",
  "OYYYYO",
  "OYYYYO",
  ".OOOO.",
};

static const char* const SPR_CLOUD_SMALL[7] = {
  ".....WWW....",
  "...WWGGGW...",
  ".WWGGGGGGWW.",
  "WGGGGGGGGGGW",
  "GGGGGGGGGGGG",
  "gGGGGGGGGGGg",
  ".gggggggggg.",
};

static const char* const SPR_BOLT[7] = {
  "..LLL",
  ".LLL.",
  "LLLLL",
  "..LL.",
  ".LL..",
  ".L...",
  "L....",
};

// Sole 16x16 con raggi che "pulsano" (cardinali / diagonali alternati)
static void drawSun(const int ox, const int oy, const uint8_t step) {
  const bool b = (step >> 2) & 1;
  const CRGB ray(255, 140, 0);
  const CRGB rayTip(255, 90, 0);
  for (int k = (b ? 1 : 0); k <= 2; k++) {
    const CRGB c = (k == (b ? 1 : 0)) ? rayTip : ray;
    iplot(ox + 7, oy + k, c);      iplot(ox + 8, oy + k, c);
    iplot(ox + 7, oy + 15 - k, c); iplot(ox + 8, oy + 15 - k, c);
    iplot(ox + k, oy + 7, c);      iplot(ox + k, oy + 8, c);
    iplot(ox + 15 - k, oy + 7, c); iplot(ox + 15 - k, oy + 8, c);
  }
  for (int k = (b ? 2 : 3); k <= 3; k++) {
    const CRGB c = (k == (b ? 2 : 3)) ? rayTip : ray;
    iplot(ox + k, oy + k, c);          iplot(ox + 15 - k, oy + k, c);
    iplot(ox + k, oy + 15 - k, c);     iplot(ox + 15 - k, oy + 15 - k, c);
  }
  drawSprite(ox + 4, oy + 4, SPR_SUN_DISC, 8, 0);
}

// Sole piccolo (per "poco nuvoloso"): disco 6x6 centrato in (ox+10.5, oy+4.5)
static void drawSunSmall(const int ox, const int oy, const uint8_t step) {
  const bool b = (step >> 2) & 1;
  const CRGB ray(255, 140, 0);
  if (!b) {  // raggi cardinali
    iplot(ox + 10, oy + 0, ray); iplot(ox + 11, oy + 0, ray);
    iplot(ox + 10, oy + 9, ray); iplot(ox + 11, oy + 9, ray);
    iplot(ox + 6, oy + 4, ray);  iplot(ox + 6, oy + 5, ray);
    iplot(ox + 15, oy + 4, ray); iplot(ox + 15, oy + 5, ray);
  } else {   // raggi diagonali
    iplot(ox + 7, oy + 1, ray);  iplot(ox + 14, oy + 1, ray);
    iplot(ox + 7, oy + 8, ray);  iplot(ox + 14, oy + 8, ray);
  }
  drawSprite(ox + 8, oy + 2, SPR_SUN_SMALL, 6, 0);
}

// Luna con fase reale, crateri, bordo; stelle che brillano
static void drawMoon(const int ox, const int oy, const float cx, const float cy, const float R, const uint8_t step, const bool stars) {
  if (stars) {
    static const int8_t sx[4] = {1, 14, 13, 2};
    static const int8_t sy[4] = {2, 1, 13, 14};
    for (uint8_t i = 0; i < 4; i++) {
      const uint8_t ph = (step + i * 3) & 7;
      if (ph < 2) {
        iplot(ox + sx[i], oy + sy[i], CRGB(255, 255, 220));
        if (ph == 0) {  // scintillio a croce
          const CRGB a(70, 70, 110);
          iplot(ox + sx[i] - 1, oy + sy[i], a); iplot(ox + sx[i] + 1, oy + sy[i], a);
          iplot(ox + sx[i], oy + sy[i] - 1, a); iplot(ox + sx[i], oy + sy[i] + 1, a);
        }
      } else if (ph < 5) {
        iplot(ox + sx[i], oy + sy[i], CRGB(90, 90, 130));
      }
    }
  }

  const float p = moonPhase01();
  const float k = cosf(p * 2.0f * PI);
  const bool waxing = (p < 0.5f);
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      const float dx = x - cx, dy = y - cy;
      const float d2 = dx * dx + dy * dy;
      if (d2 > R * R) continue;
      const float nx = dx / R, ny = dy / R;
      const float w = sqrtf(fmaxf(0.0f, 1.0f - ny * ny));
      const bool lit = waxing ? (nx >= k * w) : (nx <= -k * w);
      const bool edge = d2 > (R - 1.0f) * (R - 1.0f);
      CRGB c;
      if (lit) {
        c = edge ? CRGB(215, 210, 170) : CRGB(245, 242, 205);
        // crateri
        const int rx = static_cast<int>(lroundf(dx * 5.6f / R)), ry = static_cast<int>(lroundf(dy * 5.6f / R));
        if ((rx == -2 && ry == -2) || (rx == 1 && ry == 2) || (rx == 2 && ry == -1) || (rx == -2 && ry == 2))
          c = CRGB(185, 180, 145);
      } else {
        c = edge ? CRGB(40, 44, 70) : CRGB(18, 20, 36);
      }
      iplot(ox + x, oy + y, c);
    }
  }
}

// Gocce che cadono (1 px per step, ciclo di 8 righe sotto la nuvola)
static void drawRain(const int ox, const int oy, const uint8_t step, const bool heavy) {
  static const int8_t colsL[4] = {3, 6, 9, 12};
  static const int8_t offsL[4] = {0, 5, 2, 7};
  static const int8_t colsH[6] = {2, 4, 7, 9, 11, 13};
  static const int8_t offsH[6] = {0, 4, 6, 2, 5, 1};
  const uint8_t n = heavy ? 6 : 4;
  const CRGB head(90, 170, 255), tail(25, 70, 190);
  for (uint8_t i = 0; i < n; i++) {
    const int x = ox + (heavy ? colsH[i] : colsL[i]);
    const int y = 8 + ((step + (heavy ? offsH[i] : offsL[i])) & 7);  // righe 8..15
    iplot(x, oy + y, head);
    if (y - 1 >= 8) iplot(x, oy + y - 1, tail);
    if (heavy && y - 2 >= 8) iplot(x, oy + y - 2, CRGB(15, 40, 120));
  }
}

static void drawSnow(const int ox, const int oy, const uint8_t step) {
  static const int8_t cols[4] = {2, 6, 10, 14};
  static const int8_t offs[4] = {0, 4, 2, 6};
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t t = (step + offs[i]) & 7;
    const int y = oy + 9 + t * 6 / 8;            // scende piano (righe 9..14, bracci 8..15)
    const int x = ox + cols[i] + (((t >> 1) & 1) ? 1 : 0) - 1;  // ondeggia
    const CRGB arm(110, 160, 255);
    iplot(x, y, CRGB(255, 255, 255));
    iplot(x, y - 1, arm);
    iplot(x, y + 1, arm);
    iplot(x - 1, y, arm);
    iplot(x + 1, y, arm);
  }
}

static void drawFog(const int ox, const int oy, const uint8_t step, const uint8_t tone) {
  static const int8_t rowY[5] = {2, 5, 8, 11, 14};
  static const int8_t x0s[5] = {2, 0, 3, 1, 4};
  static const int8_t lens[5] = {11, 13, 12, 13, 9};
  const bool b = (step >> 2) & 1;
  for (uint8_t i = 0; i < 5; i++) {
    const int shift = (i & 1) ? (b ? 1 : 0) : (b ? 0 : 1);
    const CRGB c = (i & 1) ? palColor('G', tone) : palColor('W', tone);
    for (int j = 0; j < lens[i]; j++) {
      if (j == 3 + i) continue;  // piccolo stacco nella banda
      iplot(ox + x0s[i] + shift + j, oy + rowY[i], c);
    }
  }
}

static inline WeatherIconKind iconFromOpenWeather() {
  const int code = (g_icon[0] - '0') * 10 + (g_icon[1] - '0');
  switch (code) {
    case 1: return WICON_CLEAR;
    case 2: return WICON_FEW_CLOUDS;
    case 3: return WICON_CLOUDY;
    case 4: return WICON_OVERCAST;
    case 9: return WICON_HEAVY_RAIN;
    case 10: return WICON_RAIN;
    case 11: return WICON_THUNDER;
    case 13: return WICON_SNOW;
    case 50: return WICON_FOG;
    default: return WICON_CLOUDY;
  }
}

void drawWeatherIcon16x16(const int x0, const int y0, const WeatherIconKind kind, const bool night, const uint8_t step) {
  g_clipX0 = x0; g_clipY0 = y0; g_clipX1 = x0 + 15; g_clipY1 = y0 + 15;
  const bool b = (step >> 2) & 1;
  const uint8_t cloudBr = night ? 70 : 100;

  switch (kind) {
    case WICON_CLEAR:
      g_iconBrPct = 100;
      if (night) drawMoon(x0, y0, 7.5f, 7.5f, 5.6f, step, true);
      else drawSun(x0, y0, step);
      break;

    case WICON_FEW_CLOUDS:
      g_iconBrPct = 100;
      if (night) drawMoon(x0, y0, 10.5f, 4.5f, 3.9f, step, false);
      else drawSunSmall(x0, y0, step);
      g_iconBrPct = cloudBr;
      drawSprite(x0 + (b ? 1 : 0), y0 + 8, SPR_CLOUD_SMALL, 7, 0);
      break;

    case WICON_CLOUDY:
      g_iconBrPct = cloudBr;
      drawSprite(x0 + 3 + (b ? 0 : 1), y0 + 2, SPR_CLOUD_SMALL, 7, 1);
      drawSprite(x0 + (b ? 1 : 0), y0 + 7, SPR_CLOUD_BIG, 8, 0);
      break;

    case WICON_OVERCAST:
      g_iconBrPct = cloudBr;
      drawSprite(x0 + 3 + (b ? 1 : 0), y0 + 2, SPR_CLOUD_SMALL, 7, 2);
      drawSprite(x0 + (b ? 0 : 1), y0 + 7, SPR_CLOUD_BIG, 8, 1);
      break;

    case WICON_FOG:
      g_iconBrPct = cloudBr;
      drawFog(x0, y0, step, 1);
      break;

    case WICON_RAIN:
      g_iconBrPct = cloudBr;
      drawSprite(x0 + 1, y0, SPR_CLOUD_BIG, 8, 1);
      drawRain(x0, y0, step, false);
      break;

    case WICON_HEAVY_RAIN:
      g_iconBrPct = cloudBr;
      drawSprite(x0 + 1, y0, SPR_CLOUD_BIG, 8, 2);
      drawRain(x0, y0, step, true);
      break;

    case WICON_THUNDER: {
      g_iconBrPct = cloudBr;
      const bool flash = (step == 0);
      drawSprite(x0 + 1, y0, SPR_CLOUD_BIG, 8, flash ? 1 : 3);
      drawRain(x0, y0, step, false);
      if (step != 3 && step != 6 && step != 7) {
        g_iconBrPct = 100;
        drawSprite(x0 + 5, y0 + 8, SPR_BOLT, 7, 0);
        if (flash || step == 4) {  // flash: nucleo bianco
          iplot(x0 + 8, y0 + 8, palColor('l', 0));
          iplot(x0 + 7, y0 + 9, palColor('l', 0));
          iplot(x0 + 7, y0 + 10, palColor('l', 0));
          iplot(x0 + 8, y0 + 10, palColor('l', 0));
        }
      }
      break;
    }

    case WICON_SNOW:
      g_iconBrPct = cloudBr;
      drawSprite(x0 + 1, y0, SPR_CLOUD_BIG, 8, 0);
      g_iconBrPct = 100;
      drawSnow(x0, y0, step);
      break;
  }
  g_iconBrPct = 100;
}
// ==== ICONS END

// ============================================================
// Font 3x5
// ============================================================
const uint8_t font3x5_digits[10][5] = {
  {0b111, 0b101, 0b101, 0b101, 0b111}, {0b010, 0b110, 0b010, 0b010, 0b111}, {0b111, 0b001, 0b111, 0b100, 0b111},
  {0b111, 0b001, 0b111, 0b001, 0b111}, {0b101, 0b101, 0b111, 0b001, 0b001}, {0b111, 0b100, 0b111, 0b001, 0b111},
  {0b111, 0b100, 0b111, 0b101, 0b111}, {0b111, 0b001, 0b001, 0b001, 0b001}, {0b111, 0b101, 0b111, 0b101, 0b111},
  {0b111, 0b101, 0b111, 0b001, 0b111}
};
const uint8_t glyph_minus[5] = {0b000, 0b000, 0b111, 0b000, 0b000};
const uint8_t glyph_pct[5] = {0b101, 0b001, 0b010, 0b100, 0b101};

static inline void drawGlyph3x5(const int x, const int y, const uint8_t g[5], const CRGB& col) {
  for (int ry = 0; ry < 5; ry++)
    for (int rx = 0; rx < 3; rx++)
      if (g[ry] & (1 << (2 - rx))) plot(x + rx, y + ry, col);
}

static inline int drawChar3x5(const int x, const int y, const char ch, const CRGB& col) {
  if (ch >= '0' && ch <= '9') drawGlyph3x5(x, y, font3x5_digits[ch - '0'], col);
  else if (ch == '-') drawGlyph3x5(x, y, glyph_minus, col);
  else if (ch == '%') drawGlyph3x5(x, y, glyph_pct, col);
  return 4;
}

static inline int textWidth3x5(const char* s) {
  const int n = static_cast<int>(strlen(s));
  return n ? (n * 4 - 1) : 0;
}

static inline void drawText3x5(int x, const int y, const char* s, const CRGB& col) {
  for (int i = 0; s[i]; i++) x += drawChar3x5(x, y, s[i], col);
}

static inline void drawDegree2x2(const int x, const int y, const CRGB& col) {
  plot(x, y, col); plot(x + 1, y, col); plot(x, y + 1, col); plot(x + 1, y + 1, col);
}

// ============================================================
// Render
// ============================================================
constexpr int TEXT_MIN_X = 16;  // il testo non invade l'area icona standard

uint8_t lastFrame = 255;
bool lastNight = false;
int lastT = 99999;
int lastHum = 9999;
char lastIcon[4] = "xxx";
uint8_t lastEffBr = 0;

unsigned long animHalfPeriodMs() { return map(g_animSpeedPct, 1, 100, 2800, 350); }

void renderIfNeeded() {
  updateNightFlag();

  const unsigned long stepMs = max(60UL, animHalfPeriodMs() / 4UL);
  const uint8_t frame = static_cast<uint8_t>((millis() / stepMs) & 7UL);  // step animazione 0..7
  const uint8_t effBr = effectiveBrightness();
  const bool fresh = meteoFresh();
  const int t = (fresh && !isnan(g_tempC)) ? static_cast<int>(lroundf(g_tempC)) : 99999;
  const int hum = (fresh && g_humidity >= 0) ? g_humidity : 9999;

  const bool changed = g_dirty || frame != lastFrame || g_isNight != lastNight ||
                       strncmp(g_icon, lastIcon, 3) != 0 || t != lastT || hum != lastHum || effBr != lastEffBr;
  if (!changed) return;

  lastFrame = frame;
  lastNight = g_isNight;
  memcpy(lastIcon, g_icon, 4);
  lastT = t;
  lastHum = hum;
  lastEffBr = effBr;
  g_dirty = false;

  CRGB cTmp = g_colTemp;
  CRGB cHum = g_colHum;
  if (g_isNight) {
    cTmp = scaleColorPct(cTmp, g_nightDimPct);
    cHum = scaleColorPct(cHum, g_nightDimPct);
  }

  fill_solid(leds, NUM_LEDS, g_colBg);

  if (g_haveMeteo) drawWeatherIcon16x16(g_iconPosX, g_iconPosY, iconFromOpenWeather(), g_isNight, frame);

  constexpr int RIGHT_EDGE = MATRIX_W - 1;

  // Temperatura (in alto a destra) + simbolo gradi
  char ts[8];
  if (t == 99999) strcpy(ts, "--");
  else snprintf(ts, sizeof(ts), "%d", t);
  int tx = RIGHT_EDGE - 3 - textWidth3x5(ts);
  if (tx < TEXT_MIN_X) tx = TEXT_MIN_X;
  drawText3x5(tx, 1, ts, cTmp);
  drawDegree2x2(RIGHT_EDGE - 1, 1, cTmp);

  // Umidita' (in basso a destra)
  char hs[8];
  if (hum != 9999) snprintf(hs, sizeof(hs), "%d%%", hum);
  else strcpy(hs, "--%");
  int hx = RIGHT_EDGE + 1 - textWidth3x5(hs);
  if (hx < TEXT_MIN_X) hx = TEXT_MIN_X;
  drawText3x5(hx, 10, hs, cHum);

  // Indicatore WiFi assente: pixel rosso in basso a sinistra
  if (WiFi.status() != WL_CONNECTED) plot(0, MATRIX_H - 1, CRGB(120, 0, 0));

  FastLED.setBrightness(effBr);
  FastLED.show();
}

// ============================================================
// Web
// ============================================================
String stateJson() {
  const bool wifi_ok = (WiFi.status() == WL_CONNECTED);

  char ipbuf[20] = "";
  if (wifi_ok) {
    const IPAddress ip = WiFi.localIP();
    snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  }

  char dt[24] = "";
  if (timeIsValid()) {
    const time_t now = time(nullptr);
    struct tm t {};
    localtime_r(&now, &t);
    strftime(dt, sizeof(dt), "%Y-%m-%d %H:%M:%S", &t);
  }

  char ctHex[8], chHex[8], ds[6], ns[6];
  toHexColorBuf(g_colTemp, ctHex);
  toHexColorBuf(g_colHum, chHex);
  minToHHMMBuf(g_dayStartMin, ds);
  minToHHMMBuf(g_nightStartMin, ns);

  char rssiBuf[12] = "null";
  if (wifi_ok) snprintf(rssiBuf, sizeof(rssiBuf), "%d", WiFi.RSSI());

  char tempPart[16] = "null", humPart[8] = "null";
  if (g_haveMeteo && !isnan(g_tempC)) snprintf(tempPart, sizeof(tempPart), "%.1f", g_tempC);
  if (g_haveMeteo && g_humidity >= 0) snprintf(humPart, sizeof(humPart), "%d", g_humidity);

  static char json[900];
  snprintf(json, sizeof(json),
           "{"
           "\"wifi_ok\":%s,\"ip\":\"%s\",\"rssi_dbm\":%s,\"datetime\":\"%s\","
           "\"mode\":\"%s\",\"preview_night\":%s,\"br_pct\":%u,\"ct\":\"%s\",\"ch\":\"%s\","
           "\"ds\":\"%s\",\"ns\":\"%s\",\"owm\":%u,\"arh\":%u,\"nbp\":%u,\"nbm\":%u,\"ndp\":%u,\"asp\":%u,"
           "\"meteo_ok\":%s,\"meteo_fresh\":%s,\"http\":%d,\"icon\":\"%s\",\"temp_c\":%s,\"hum\":%s,"
           "\"meteo_age_s\":%ld,\"meteo_fail_streak\":%u,\"icon_x\":%u,\"icon_y\":%u,"
           "\"moon_pct\":%u,\"uptime_s\":%lu,\"heap\":%u,\"prefs_pending\":%s"
           "}",
           wifi_ok ? "true" : "false", ipbuf, rssiBuf, dt,
           g_isNight ? "NOTTE" : "GIORNO", g_forceNightPreview ? "true" : "false",
           brPctFrom255(g_brightness), ctHex, chHex, ds, ns,
           g_owIntervalMin, g_autoResetHours, g_nightBrightnessPct, g_nightBrightnessMin, g_nightDimPct, g_animSpeedPct,
           g_meteoOk ? "true" : "false", meteoFresh() ? "true" : "false", g_lastHttpCode, g_icon, tempPart, humPart,
           g_haveMeteo ? static_cast<long>((millis() - g_meteoLastOkMs) / 1000UL) : -1L,
           g_meteoFailStreak, g_iconPosX, g_iconPosY,
           moonIllumPct(), (millis() - bootMs) / 1000UL, static_cast<unsigned>(ESP.getFreeHeap()),
           g_prefsDirty ? "true" : "false");
  return String(json);
}

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Meteo 16x32</title>
<style>
:root{--bg:#0b1020;--line:#273150;--txt:#ecf2ff;--mut:#97a6c7;--ok:#34d399;--bad:#fb7185;--acc:#60a5fa}
*{box-sizing:border-box} body{margin:0;font-family:Inter,system-ui;background:radial-gradient(1000px 500px at 10% -10%,#1e2b52,transparent),var(--bg);color:var(--txt);padding:14px}
.wrap{max-width:980px;margin:auto}.top{display:flex;gap:10px;flex-wrap:wrap;align-items:center;justify-content:space-between}
.card{background:linear-gradient(180deg,rgba(255,255,255,.04),rgba(255,255,255,.02));border:1px solid var(--line);border-radius:16px;padding:14px;margin-top:10px;box-shadow:0 12px 30px rgba(0,0,0,.35)}
.badge{padding:6px 10px;border-radius:999px;border:1px solid var(--line);font-size:12px;color:var(--mut);display:inline-flex;gap:8px;align-items:center}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mut)}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}@media(max-width:860px){.grid{grid-template-columns:1fr}}
.row{background:rgba(255,255,255,.02);border:1px solid var(--line);border-radius:12px;padding:10px}.line{display:flex;justify-content:space-between;gap:8px;margin-bottom:6px}
.small{font-size:12px;color:var(--mut)} input[type=range]{width:100%;accent-color:var(--acc)} input[type=color],input[type=time],select{background:#0a1226;color:var(--txt);border:1px solid var(--line);border-radius:10px;padding:8px}
input[type=color]{padding:2px;width:60px;height:38px}
.actions{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.btn{border:1px solid var(--line);background:#1f2a46;color:var(--txt);padding:9px 12px;border-radius:10px;cursor:pointer}
.btn.acc{background:#1f3a69;border-color:#2f5ca6}.btn.danger{background:#4a1f2f;border-color:#7a2f48}.mono{font-family:ui-monospace,Consolas,monospace}
.big{font-size:28px;font-weight:600}
</style></head>
<body><div class="wrap">
  <div class="top"><h3 style="margin:0">Meteo 16x32</h3>
    <div class="badge"><span class="dot" id="wDot"></span><span id="wifi">WiFi…</span></div>
    <div class="badge"><span class="dot" id="mDot"></span><span id="meteoState">OpenWeather…</span></div>
    <div class="badge mono" id="meta">--</div>
  </div>
  <div class="card">
    <div class="actions" style="justify-content:space-between">
      <div><span class="big" id="tbig">--</span><span class="small"> °C</span> &nbsp; <span class="big" id="hbig">--</span><span class="small"> %</span></div>
      <div class="small mono" id="meteo">--</div>
    </div>
  </div>
  <div class="card">
    <div class="grid">
      <div class="row"><div class="line"><b>Luminosità giorno %</b><span class="mono" id="brv">--</span></div><input id="br" type="range" min="1" max="100"></div>
      <div class="row"><div class="line"><b>Luminosità notte (% del giorno)</b><span class="mono" id="nbpv">--</span></div><input id="nbp" type="range" min="1" max="100"></div>
      <div class="row"><div class="line"><b>Luminosità minima notte</b><span class="mono" id="nbmv">--</span></div><input id="nbm" type="range" min="1" max="80"></div>
      <div class="row"><div class="line"><b>Attenuazione testo notte %</b><span class="mono" id="ndpv">--</span></div><input id="ndp" type="range" min="1" max="100"></div>
      <div class="row"><div class="line"><b>Velocità animazione %</b><span class="mono" id="aspv">--</span></div><input id="asp" type="range" min="1" max="100"></div>
      <div class="row"><div class="line"><b>Intervallo OpenWeather (min)</b><span class="mono" id="owmv">--</span></div><select id="owm"><option>1</option><option>5</option><option>10</option><option>15</option><option>30</option><option>60</option><option>120</option></select></div>
      <div class="row"><div class="line"><b>Colori</b><span class="small">Temperatura / Umidità</span></div><input id="ct" type="color"> <input id="ch" type="color"></div>
      <div class="row"><div class="line"><b>Orari</b><span class="small">Inizio giorno / Inizio notte</span></div><input id="ds" type="time"> <input id="ns" type="time"></div>
      <div class="row"><div class="line"><b>Posizione icona X</b><span class="mono" id="ixv">--</span></div><input id="ix" type="range" min="0" max="16"></div>
      <div class="row"><div class="line"><b>Posizione icona Y</b><span class="mono" id="iyv">--</span></div><input id="iy" type="range" min="0" max="0"></div>
      <div class="row"><div class="line"><b>Riavvio automatico</b><span class="small">ore (0 = mai)</span></div><select id="arh"><option value="0">Mai</option><option value="6">6 h</option><option value="12">12 h</option><option value="24">24 h</option><option value="48">48 h</option><option value="168">1 settimana</option><option value="720">1 mese</option></select></div>
    </div>
    <div style="margin-top:12px" class="small">IP: <span class="mono" id="ip">--</span> · RSSI: <span class="mono" id="rssi">--</span> · Ora: <span class="mono" id="dt">--</span> · Uptime: <span class="mono" id="up">--</span> · Heap: <span class="mono" id="heap">--</span></div>
    <div class="actions" style="margin-top:12px">
      <button class="btn" id="save">Salva ora</button>
      <button class="btn acc" id="pn">Preview notte (15 s)</button>
      <button class="btn acc" id="rf">Aggiorna meteo</button>
      <button class="btn danger" id="rst">Riavvia ESP32</button>
      <span class="small" id="msg">—</span>
    </div>
  </div>
</div>
<script>
const $=id=>document.getElementById(id); let saveT=0, lastEdit=0;
function dot(el,on){el.style.background=on?'var(--ok)':'var(--bad)'}
function flash(t){$('msg').textContent=t;setTimeout(()=>$('msg').textContent='—',1500)}
function fmtUp(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'g ':'')+h+'h '+m+'m'}
function ui(s){
 dot($('wDot'),!!s.wifi_ok);$('wifi').textContent=s.wifi_ok?`WiFi OK (${s.ip})`:'WiFi non connesso';
 dot($('mDot'),!!s.meteo_ok);$('meteoState').textContent=s.meteo_ok?'OpenWeather OK':`OpenWeather errore (${s.http})`;
 $('meta').textContent=`${s.mode}${s.preview_night?' (preview)':''} · luna ${s.moon_pct}% · fail=${s.meteo_fail_streak} · age=${s.meteo_age_s<0?'--':s.meteo_age_s+'s'}`;
 $('ip').textContent=s.ip||'--';$('rssi').textContent=(s.rssi_dbm==null)?'--':`${s.rssi_dbm} dBm`;
 $('dt').textContent=s.datetime||'--';$('up').textContent=fmtUp(s.uptime_s);$('heap').textContent=s.heap;
 $('tbig').textContent=s.temp_c??'--';$('hbig').textContent=s.hum??'--';
 $('meteo').textContent=`icona ${s.icon}${s.meteo_fresh?'':' (dati scaduti)'}`;
 if(Date.now()-lastEdit<4000) return; // non sovrascrivere i controlli mentre li stai usando
 $('br').value=s.br_pct;$('nbp').value=s.nbp;$('nbm').value=s.nbm;$('ndp').value=s.ndp;$('asp').value=s.asp;
 $('brv').textContent=s.br_pct;$('nbpv').textContent=s.nbp;$('nbmv').textContent=s.nbm;$('ndpv').textContent=s.ndp;$('aspv').textContent=s.asp;
 $('owm').value=String(s.owm);$('owmv').textContent=s.owm;$('arh').value=String(s.arh);
 $('ct').value=s.ct;$('ch').value=s.ch;$('ds').value=s.ds;$('ns').value=s.ns;
 $('ix').value=s.icon_x;$('iy').value=s.icon_y;$('ixv').textContent=s.icon_x;$('iyv').textContent=s.icon_y;
}
async function load(){try{const r=await fetch('/state',{cache:'no-store'});ui(await r.json());}catch(e){dot($('wDot'),false);$('wifi').textContent='ESP32 non raggiungibile'}}
function payload(commit){return {br_pct:+$('br').value,nbp:+$('nbp').value,nbm:+$('nbm').value,ndp:+$('ndp').value,asp:+$('asp').value,owm:+$('owm').value,arh:+$('arh').value,ct:$('ct').value,ch:$('ch').value,ds:$('ds').value,ns:$('ns').value,ix:+$('ix').value,iy:+$('iy').value,commit:commit?1:0}}
async function send(commit){try{const r=await fetch('/set',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload(commit))});ui(await r.json());flash(commit?'Salvato in memoria':'Applicato')}catch(e){flash('Errore invio')}}
function debounceSend(){lastEdit=Date.now();clearTimeout(saveT);saveT=setTimeout(()=>send(false),200)}
['br','nbp','nbm','ndp','asp','ix','iy'].forEach(id=>$(id).addEventListener('input',()=>{$(id+'v').textContent=$(id).value;debounceSend();}));
['owm','arh','ct','ch','ds','ns'].forEach(id=>$(id).addEventListener('change',debounceSend));
['ct','ch'].forEach(id=>$(id).addEventListener('input',debounceSend));
$('save').onclick=()=>{lastEdit=0;send(true)};
$('pn').onclick=async()=>{const r=await fetch('/previewNight',{method:'POST'});ui(await r.json())};
$('rf').onclick=async()=>{await fetch('/refresh',{method:'POST'});flash('Aggiornamento richiesto');setTimeout(load,6000)};
$('rst').onclick=()=>{if(confirm('Riavviare ESP32?'))fetch('/restart',{method:'POST'}).then(()=>flash('Riavvio…'))};
load();setInterval(load,3000);
</script></body></html>
)rawliteral";

// Lettura parametro da query/form o da body JSON piatto
static bool jsonValueStart(const String& key, const String& body, int& i) {
  const String k = String("\"") + key + "\"";
  i = body.indexOf(k);
  if (i < 0) return false;
  i = body.indexOf(':', i + k.length());
  if (i < 0) return false;
  i++;
  while (i < static_cast<int>(body.length()) && body[i] == ' ') i++;
  return i < static_cast<int>(body.length());
}

static bool argOrJsonStr(const String& key, String& out) {
  if (server.hasArg(key)) { out = server.arg(key); return true; }
  const String body = server.arg("plain");
  int i;
  if (!jsonValueStart(key, body, i) || body[i] != '"') return false;
  const int j = body.indexOf('"', i + 1);
  if (j < 0) return false;
  out = body.substring(i + 1, j);
  return true;
}

static bool argOrJsonInt(const String& key, int& out) {
  if (server.hasArg(key)) { out = server.arg(key).toInt(); return true; }
  const String body = server.arg("plain");
  int i;
  if (!jsonValueStart(key, body, i)) return false;
  int j = i;
  while (j < static_cast<int>(body.length()) && (body[j] == '-' || isDigit(body[j]))) j++;
  if (j == i) return false;
  out = body.substring(i, j).toInt();
  return true;
}

void handleSet() {
  int v;
  String s;
  if (argOrJsonInt("br_pct", v)) g_brightness = br255FromPct(constrain(v, 1, 100));
  if (argOrJsonInt("nbp", v)) g_nightBrightnessPct = constrain(v, 1, 100);
  if (argOrJsonInt("nbm", v)) g_nightBrightnessMin = constrain(v, 1, 80);
  if (argOrJsonInt("ndp", v)) g_nightDimPct = constrain(v, 1, 100);
  if (argOrJsonInt("asp", v)) g_animSpeedPct = constrain(v, 1, 100);
  if (argOrJsonStr("ct", s)) g_colTemp = parseHexColor(s, g_colTemp);
  if (argOrJsonStr("ch", s)) g_colHum = parseHexColor(s, g_colHum);
  if (argOrJsonStr("ds", s)) g_dayStartMin = hhmmToMinSafe(s, g_dayStartMin);
  if (argOrJsonStr("ns", s)) g_nightStartMin = hhmmToMinSafe(s, g_nightStartMin);
  if (argOrJsonInt("owm", v)) g_owIntervalMin = constrain(v, 1, 120);
  if (argOrJsonInt("arh", v)) g_autoResetHours = constrain(v, 0, 720);
  if (argOrJsonInt("ix", v)) g_iconPosX = constrain(v, 0, static_cast<int>(ICON_MAX_X));
  if (argOrJsonInt("iy", v)) g_iconPosY = constrain(v, 0, static_cast<int>(ICON_MAX_Y));

  int commit = 0;
  argOrJsonInt("commit", commit);
  if (commit) savePrefs();
  else markPrefsDirty();

  updateNightFlag();
  g_dirty = true;
  server.send(200, "application/json", stateJson());
}

void handlePreviewNight() {
  g_forceNightPreview = !g_forceNightPreview;
  g_forceNightStartMs = millis();
  updateNightFlag();
  g_dirty = true;
  server.send(200, "application/json", stateJson());
}

void setupWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/state", HTTP_GET, []() { server.send(200, "application/json", stateJson()); });
  server.on("/set", HTTP_POST, handleSet);
  server.on("/previewNight", HTTP_POST, handlePreviewNight);
  server.on("/refresh", HTTP_POST, []() { g_meteoForce = true; server.send(200, "text/plain", "OK"); });
  server.on("/restart", HTTP_POST, []() {
    if (g_prefsDirty) savePrefs();
    server.send(200, "text/plain", "OK");
    delay(150);
    ESP.restart();
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ============================================================
// OTA / WiFi / salute
// ============================================================
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.onStart([]() {
    g_otaRunning = true;
    if (g_prefsDirty) savePrefs();
    FastLED.clear(true);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    esp_task_wdt_reset();  // l'upload OTA blocca il loop: evita il panic del WDT
    // barra di avanzamento sulla riga centrale
    const uint8_t n = total ? static_cast<uint8_t>((static_cast<uint64_t>(done) * MATRIX_W) / total) : 0;
    static uint8_t lastN = 255;
    if (n != lastN) {
      lastN = n;
      FastLED.clear();
      for (uint8_t x = 0; x < n && x < MATRIX_W; x++) plot(x, 7, CRGB(0, 60, 120));
      FastLED.setBrightness(30);
      FastLED.show();
    }
  });
  ArduinoOTA.onEnd([]() { g_otaRunning = false; });
  ArduinoOTA.onError([](ota_error_t) {
    g_otaRunning = false;
    g_dirty = true;
  });
  ArduinoOTA.begin();  // avvia anche mDNS con l'hostname
  MDNS.addService("http", "tcp", 80);
}

void wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOkMs = millis();
    if (!g_otaStarted) {
      setupOTA();
      g_otaStarted = true;
      Serial.printf("[WiFi] connesso, IP %s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }
  // l'autoreconnect del core prova da solo; dopo il periodo di grazia forziamo
  if (!elapsed(lastWifiOkMs, WIFI_GRACE_MS)) return;
  if (!elapsed(lastWifiTryMs, WIFI_RECONNECT_INTERVAL_MS)) return;
  lastWifiTryMs = millis();
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void healthCheck() {
  if (!elapsed(lastHealthMs, HEALTH_PERIOD_MS)) return;
  lastHealthMs = millis();
  wifiEnsureConnected();
  g_dirty = true;  // aggiorna indicatore WiFi

  // WiFi assente troppo a lungo -> riavvio pulito
  if (WiFi.status() != WL_CONNECTED && elapsed(lastWifiOkMs, WIFI_OFFLINE_RESTART_MS)) {
    if (g_prefsDirty) savePrefs();
    ESP.restart();
  }
  // WiFi OK ma OpenWeather fallisce da troppo tempo (stack di rete bloccato)
  if (WiFi.status() == WL_CONNECTED && g_meteoFailStreak >= METEO_FAIL_RESTART) {
    if (g_prefsDirty) savePrefs();
    ESP.restart();
  }
  // Riavvio programmato
  if (g_autoResetHours > 0 && elapsed(bootMs, static_cast<unsigned long>(g_autoResetHours) * 3600000UL)) {
    if (g_prefsDirty) savePrefs();
    delay(100);
    ESP.restart();
  }
}

// ============================================================
// Setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(120);
  bootMs = millis();
  Serial.println("\n[Meteo_16x32] avvio v2");

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms = 15000,
    .idle_core_mask = 0,  // solo il task loop
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&wdt_cfg);  // il core 3.x inizializza gia' il TWDT
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(15, true);
  esp_task_wdt_add(NULL);
#endif

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, PSU_MAX_MA);
  FastLED.setBrightness(8);
  FastLED.clear(true);

  loadPrefs();

  // Splash: barra blu mentre si connette
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);  // web UI e OTA piu' reattivi
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const unsigned long t0 = millis();
  uint8_t x = 0;
  while (WiFi.status() != WL_CONNECTED && !elapsed(t0, 15000UL)) {
    plot(x % MATRIX_W, 7, CRGB(0, 40, 120));
    plot((x + MATRIX_W - 4) % MATRIX_W, 7, CRGB::Black);
    FastLED.show();
    x++;
    delay(150);
    esp_task_wdt_reset();
  }
  FastLED.clear(true);
  lastWifiOkMs = millis();

  if (WiFi.status() == WL_CONNECTED) {
    setupOTA();
    g_otaStarted = true;
    Serial.printf("[WiFi] connesso a %s, IP %s, RSSI %d dBm\n", WIFI_SSID, WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("[WiFi] NON connesso a %s (status %d)\n", WIFI_SSID, static_cast<int>(WiFi.status()));
  }

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 30 && !timeIsValid(); i++) {
    delay(200);
    esp_task_wdt_reset();
  }

  setupWeb();

  g_meteoOk = fetchMeteo();
  meteoLastTryMs = millis();
  if (!g_meteoOk) g_meteoFailStreak = 1;
  Serial.printf("[NTP] ora %s\n", timeIsValid() ? "OK" : "non ancora valida");
  Serial.printf("[Meteo] %s (HTTP %d) T=%.1f H=%d icona=%s\n", g_meteoOk ? "OK" : "ERRORE", g_lastHttpCode,
                g_tempC, g_humidity, g_icon);
  Serial.printf("[Icona] posizione X=%u Y=%u\n", g_iconPosX, g_iconPosY);
  Serial.println("[Setup] completato - web UI su http://meteo16x32.local");
  updateNightFlag();
  g_dirty = true;
}

void loop() {
  esp_task_wdt_reset();
  if (g_otaStarted) ArduinoOTA.handle();
  if (g_otaRunning) return;
  server.handleClient();

  handleMeteo();
  healthCheck();
  handleDeferredSave();
  renderIfNeeded();
  delay(2);
}
