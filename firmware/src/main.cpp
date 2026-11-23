/**
 * Panda State: open firmware for BIQU's ESP32-C3 light boards. Written for the
 * Panda Status P2, the light bar for Bambu Lab P2S and X2D printers, and built
 * also for the original Panda Status, the Panda Lux RGB and the Panda RGB
 * Controller (see BOARD_ below).
 *
 * The stock firmware talks to the printer itself and can show three things:
 * idle, printing, error. It has no idea the plate is still hot after a print,
 * and it draws a finished print in its own green whatever colours it is given
 * (measured on a P2 running stock V1.0.0).
 *
 * This firmware can be told what to show by something that knows more than
 * the bar does (a home-automation system, say, through POST /api/state),
 * and it can also be paired with the printer and work entirely by itself.
 * A controller that is speaking wins; when nothing is, the bar's own printer
 * link decides; with neither it says so.
 *
 * Hardware facts are in ../../HARDWARE.md, each with how it was measured.
 *
 * Getting home again is the first design constraint, ahead of any feature:
 *  - The image is installed over the air into the slot that is not running,
 *    so stock stays intact in the other one.
 *  - A build that has never proved it can get on Wi-Fi is "on trial". If it
 *    is not on Wi-Fi within two minutes it hands the bar back to the other
 *    slot by itself. Only a build that has been reachable is kept.
 *  - Stock's own settings (namespace app_nvs) are read, never written.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <nvs.h>
#include <esp_ota_ops.h>
#include <esp_image_format.h>
#include <HTTPClient.h>
#include <memory>
#include <mbedtls/sha256.h>
#include <StreamString.h>
#include <esp_timer.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <PubSubClient.h>
#include "page.h"
#include "roots.h"

/* What the firmware calls itself on its page and in /api/status. Plain and generic, so it says what it is on anyone's network. */
#define FW_NAME "panda"
#define FW_VERSION "0.7.20"
/* Where new releases and the add-on store live. A release must carry a
   panda-<board>-custom-<version>.bin for each board and a SHA256SUMS.txt that lists them. */
#define UPDATE_REPO "ImSammyTTV/Panda-State"

/* One source, three BIQU boards, all ESP32-C3. Each gets its own image
   (pio run -e panda | status | rgb) because the wrong pins on the wrong board
   is not something to decide at run time: on the RGB Controller, the P2's
   microphone clock pins are its PWM outputs. Pins for the original Panda
   Status and the RGB Controller were read out of BTT's own V1.0.3 and V1.0.2
   images (the RMT channel table and the LEDC channel set-up), not guessed. */
#if defined(BOARD_RGB)
#define BOARD_ID "rgb"
#define BOARD_NAME "Panda RGB Controller"
#elif defined(BOARD_LUX)
/* The Panda Lux RGB (PX and PX2): BTT's own image drives a WS2812 strip on GPIO 5 exactly as the original Panda Status does, so it is that board under its own name. */
#define BOARD_STATUS 1
#define BOARD_ID "lux"
#define BOARD_NAME "Panda Lux RGB"
#elif defined(BOARD_STATUS)
#define BOARD_ID "status"
#define BOARD_NAME "Panda Status"
#else
#define BOARD_P2 1
#define BOARD_ID "p2"
#define BOARD_NAME "Panda Status P2"
#endif
/* The P2 keeps the hotspot name it has always had, so its owners find it where the README says. */
#if defined(BOARD_P2)
#define AP_DEFAULT_SSID "PandaP2-Setup"
#else
#define AP_DEFAULT_SSID "PandaState-Setup"
#endif
/* The .local name each board's stock firmware answers to, read out of BTT's
   own images (the string beside their _http._tcp record), so an owner who
   bookmarked it, or was told it by BTT, still lands on the bar. "pandastatus"
   is answered too: every Panda State before 0.7.6 used it. */
#if defined(BOARD_RGB)
#define STOCK_HOST "PandaRGBController"
#elif defined(BOARD_LUX)
#define STOCK_HOST "PandaLuxRGB"
#elif defined(BOARD_STATUS)
#define STOCK_HOST "PandaStatusV1"
#else
#define STOCK_HOST "PandaStatusP2"
#endif
static const char *BUILD_ID = FW_VERSION " " __DATE__ " " __TIME__;
/* Which board an image is for, where an upload can read it before anything is written: the section right after the app description, a few hundred bytes into every image. The description itself names only the library the firmware was built with, the same for every board, so without this a Lux image installed on a P2 without a word (0.7.13). */
static const size_t BOARD_TAG_PREFIX = 18;   // "PANDA-STATE-BOARD:"
extern "C" const char PANDA_BOARD_TAG[] __attribute__((section(".rodata_custom_desc"), used, aligned(4))) =
  "PANDA-STATE-BOARD:" BOARD_ID ";" FW_VERSION " " __DATE__ " " __TIME__;

static char hostName[33] = STOCK_HOST;  // what the bar answers to on the network, without ".local"
/* Whether a name ends in ".local", in any case: mDNS names are not case-sensitive, and "printer.LOCAL" was sent to ordinary DNS, which cannot answer it. */
static bool endsWithLocal(const String &name) {
  return name.length() > 6 && name.substring(name.length() - 6).equalsIgnoreCase(".local");
}

static bool hostNameValid(const char *name) {
  const size_t len = strlen(name);
  if (len < 1 || len > 32 || name[0] == '-' || name[len - 1] == '-') return false;
  for (const char *c = name; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-') return false;
  return true;
}
static void handleHostName();

/* Measured, not assumed: see HARDWARE.md. GPIO 5 is the strip on both the P2 and the original Panda Status. */
static const uint8_t LED_PIN = 5;
/* The bar everything is drawn on: 25 LEDs, the P2's own. Other boards show the same 25-LED picture, stretched over their strip or averaged onto their PWM channels. */
static const uint8_t LED_COUNT = 25;
#if defined(BOARD_STATUS)
/* The original Panda Status drives whatever WS2812 strip is plugged in, so its length is a setting. */
static const uint16_t STRIP_MAX = 300;
static uint16_t stripLength = 25;
#elif defined(BOARD_RGB)
/* Two 12 V RGB channels on PWM, R, G, B each. Stock also drives GPIO 7 and 8 high at start-up and leaves them there; done the same here. */
static const uint8_t RGB_PINS[2][3] = {{10, 1, 4}, {6, 0, 5}};
static const uint8_t RGB_ENABLE_PINS[2] = {7, 8};
static bool rgbSplit = false;   // false: both channels show the whole bar; true: channel 1 the first half, channel 2 the second
#endif
/* The ES8311 audio chip (the microphone) and the button, as the stock firmware's board file sets them. */
static const uint8_t PIN_SDA = 2, PIN_SCL = 3;
static const uint8_t PIN_MCLK = 0, PIN_BCK = 1, PIN_WS = 4, PIN_DIN = 10;   // stock also names DOUT 7; nothing audible is on it
static const uint8_t PIN_BUTTON = 9;   // to ground when pressed; stock reads it with the pull-up on

/* Sent as raw bytes in wire order; the colour order is a setting because it
   has not been confirmed on the real LEDs yet (GRB is only the usual one). */
#if !defined(BOARD_RGB)
static Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);
#endif
/* The library hands any POST that is not a form upload to the same upload function, with no upload behind it; this is how that function can tell (0.7.11 fell over on one). */
struct PandaServer : WebServer {
  using WebServer::WebServer;
  bool uploading() const { return (bool)_currentUpload; }
};
static PandaServer server(80);

/* ---------------------------------------------------------------- log -- */

/**
 * What the owner did and what went wrong, for a bug report (GET /api/log, "Save a report" on the page).
 *
 * Kept in memory a restart does not clear, so after a crash the lines that led up to it are still there;
 * a power cut does clear it. Only the loop task writes it: other tasks' errors are picked up by
 * logWatch() once a second, from the same fields the page shows. Nothing secret goes in: no Wi-Fi name
 * or password, no access code, no serial number, no Home Assistant user or password.
 */
static const uint32_t LOG_MAGIC = 0x50534C31;
static const size_t LOG_SIZE = 3072;
struct LogStore { uint32_t magic, head, used, starts; char text[LOG_SIZE]; };
RTC_NOINIT_ATTR static LogStore logStore;

static void logBegin() {
  /* After a power cut this memory holds noise: anything that does not add up is started afresh. */
  if (logStore.magic != LOG_MAGIC || logStore.head >= LOG_SIZE || logStore.used > LOG_SIZE) {
    memset(&logStore, 0, sizeof logStore);
    logStore.magic = LOG_MAGIC;
  }
  logStore.starts++;
}

static void logLine(const char *format, ...) {
  char line[176];
  const uint32_t up = millis() / 1000;
  int at = snprintf(line, sizeof line, "%lu:%02lu:%02lu ", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  va_list args;
  va_start(args, format);
  vsnprintf(line + at, sizeof line - at - 1, format, args);
  va_end(args);
  size_t length = strlen(line);
  line[length++] = '\n';
  for (size_t i = 0; i < length; i++) {
    logStore.text[logStore.head] = line[i];
    logStore.head = (logStore.head + 1) % LOG_SIZE;
    if (logStore.used < LOG_SIZE) logStore.used++;
  }
}

/* The log, oldest line first. Once it has gone round, the first line is a partial one and is left out. */
static void logAppendTo(String &out) {
  const size_t start = logStore.used < LOG_SIZE ? 0 : logStore.head;
  size_t i = 0;
  if (logStore.used == LOG_SIZE) while (i < LOG_SIZE && logStore.text[(start + i) % LOG_SIZE] != '\n') i++;
  if (logStore.used == LOG_SIZE) i++;
  for (; i < logStore.used; i++) out += logStore.text[(start + i) % LOG_SIZE];
}

static void handleLog();   // with loop(), below: it reads most of the firmware's state

static const char *resetText(esp_reset_reason_t why) {
  switch (why) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restarted by the firmware";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "crash (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "crash (task watchdog)";
    case ESP_RST_WDT: return "crash (watchdog)";
    case ESP_RST_BROWNOUT: return "power dipped (brownout)";
    case ESP_RST_DEEPSLEEP: return "woke from sleep";
    case ESP_RST_EXT: return "reset pin";
    default: return "unknown";
  }
}
static Preferences prefs;

/* ------------------------------------------------------------- looks -- */

/* The first six are the bar's own. The rest are lamp effects in the spirit of WLED's, redrawn for 25 LEDs. They take two colours: the state's own and a second one. */
enum Effect : uint8_t {
  FX_SOLID, FX_BREATHE, FX_FILL, FX_FLASH, FX_CHASE, FX_RAINBOW,
  FX_GRADIENT, FX_WAVES, FX_SCROLL, FX_THEATRE, FX_SCANNER, FX_METEOR, FX_FIRE, FX_TWINKLE, FX_SPARKLE, FX_RAIN, FX_AURORA,
  FX_HEARTBEAT, FX_STROBE, FX_CANDLE, FX_RIPPLE, FX_STACK,   // 0.7.15; added at the end, as settings keep effects by name
  FX_COUNT
};
static const char *FX_NAMES[FX_COUNT] = {
  "solid", "breathe", "fill", "flash", "chase", "rainbow",
  "gradient", "waves", "scroll", "theatre", "scanner", "meteor", "fire", "twinkle", "sparkle", "rain", "aurora",
  "heartbeat", "strobe", "candle", "ripple", "stack",
};

/* Preparing (0.7.19) goes last among the looks: settings keep states by name, and "off" has to stay after every look. */
enum State : uint8_t { ST_IDLE, ST_PRINTING, ST_PAUSED, ST_FINISHED, ST_COOLING, ST_READY, ST_ERROR, ST_OFFLINE, ST_PREPARING, ST_OFF, ST_COUNT };
static const char *ST_NAMES[ST_COUNT] = {"idle", "printing", "paused", "finished", "cooling", "ready", "error", "offline", "preparing", "off"};
/* What a state that is switched off gives way to: the next one along that is on. Idle and offline go dark. */
static const uint8_t ST_INSTEAD[ST_COUNT] = {ST_OFF, ST_IDLE, ST_PRINTING, ST_READY, ST_FINISHED, ST_IDLE, ST_IDLE, ST_OFF, ST_PRINTING, ST_OFF};

/* colour2 is the second colour the two-colour effects use. AUTO means "work one out from the first", so a look set before 0.5.0 (or with only one colour given) still has a sensible partner. */
static const uint32_t AUTO = 0xFFFFFFFF;
/* A constructor rather than a default on the member: this toolchain's C++11 will not brace-initialise a struct that has one. */
struct Look {
  uint32_t colour; uint8_t effect; uint8_t speed; uint32_t colour2;
  uint8_t level;    // this state's own brightness, 1-100; 0 uses the bar's
  uint16_t limit;   // minutes this state is shown before the bar moves on (idle and offline go dark, the rest go to idle); 0 for as long as it lasts
  bool on;          // false: the state is skipped, and the bar shows the one it gives way to (ST_INSTEAD)
  Look(uint32_t c = 0, uint8_t e = 0, uint8_t s = 50, uint32_t c2 = AUTO) : colour(c), effect(e), speed(s), colour2(c2), level(0), limit(0), on(true) {}
};

/* Idle, printing and error start close to the stock bar's own colours. */
static Look looks[ST_COUNT] = {
  /* idle     */ {0x009DFF, FX_BREATHE, 30},
  /* printing */ {0x1BFF00, FX_FILL, 50},
  /* paused   */ {0xFFB000, FX_BREATHE, 50},
  /* finished */ {0x1BFF00, FX_SOLID, 50},
  /* cooling  */ {0xFF6A00, FX_FILL, 50},
  /* ready    */ {0x1BFF00, FX_BREATHE, 40},
  /* error    */ {0xFF0000, FX_FLASH, 60},
  /* offline  */ {0x404040, FX_BREATHE, 15},
  /* preparing*/ {0x00E5FF, FX_SCANNER, 50, 0x1BFF00},
  /* off      */ {0x000000, FX_SOLID, 50},
};

static uint8_t brightness = 100;     // 0-100, the same scale the stock bar uses
#if defined(BOARD_RGB)
static char order[4] = "RGB";   // the PWM channels are wired R, G, B (RGB_PINS); GRB is the WS2812 strips' order, and as the default here it swapped red and green
#else
static char order[4] = "GRB";
#endif
static bool reversed = false;
/* Whether the LEDs an effect leaves unlit (past a fill's edge, a breath's low point, a flash's off half) keep a faint glow, or go fully dark. */
static bool glow = true;
/* The cool-down the bar works out for itself when paired: "ready" only once this many minutes have passed AND the bed is at or below the target. A controller that pushes states makes its own decision and ignores these. */
static uint8_t coolMinutes = 10;
static uint8_t coolTarget = 35;
/* The two modes the stock firmware had: follow the printer, or dance to the room. */
enum Mode : uint8_t { MODE_STATUS, MODE_MUSIC, MODE_COUNT };
static const char *MODE_NAMES[MODE_COUNT] = {"status", "music"};
static uint8_t mode = MODE_STATUS;
/* How loud the room has to be to fill the bar. "auto" follows the room: quietest recent sound is empty, loudest is full. */
enum Sensitivity : uint8_t { SENS_LOW, SENS_MEDIUM, SENS_HIGH, SENS_AUTO, SENS_COUNT };
static const char *SENS_NAMES[SENS_COUNT] = {"low", "medium", "high", "auto"};
static uint8_t sensitivity = SENS_MEDIUM;

/* What a controller last said, and when. */
static State current = ST_OFFLINE;
static float progress = 0;           // 0-100; what "fill" draws
static uint32_t lastPushAt = 0;
static bool everPushed = false;
/* A controller is expected to push every few seconds. Past this the bar says so rather
   than showing a state that may no longer be true. */
static const uint32_t LINK_TIMEOUT_MS = 45000;

/* A look being tried from the page, which must not be mistaken for the printer's state. */
static State previewState = ST_IDLE;
static float previewProgress = 0;
static uint32_t previewUntil = 0;

static uint8_t frame[LED_COUNT][3];  // what is on the LEDs now, as R,G,B, for the page's preview

static int findName(const char *const *names, int count, const char *name) {
  if (!name) return -1;
  for (int i = 0; i < count; i++) if (strcasecmp(names[i], name) == 0) return i;
  return -1;
}

static bool parseColour(const char *text, uint32_t &out) {
  if (!text) return false;
  if (*text == '#') text++;
  /* Six hex digits and nothing else in them: strtoul also took a sign, spaces and "0x", and "-00001" came out as the "auto" marker. What follows the six is left alone, since frames are runs of them. */
  uint32_t value = 0;
  for (int i = 0; i < 6; i++) {
    const unsigned char c = text[i];   // the terminator is not a hex digit, so a short string stops here
    if (!isxdigit(c)) return false;
    value = value << 4 | (uint32_t)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
  }
  out = value;
  return true;
}

/* A partner for a colour: the same brightness and strength, a sixth of the way round the colour wheel. Close enough to sit well beside it, far enough to be seen. White and greys get a soft blue, since they have no hue to turn. */
static uint32_t partnerOf(uint32_t colour) {
  float r = ((colour >> 16) & 255) / 255.0f, g = ((colour >> 8) & 255) / 255.0f, b = (colour & 255) / 255.0f;
  const float hi = max(r, max(g, b)), lo = min(r, min(g, b)), span = hi - lo;
  if (span < 0.08f) return 0x4060FF;
  float h = hi == r ? fmodf((g - b) / span, 6) : hi == g ? (b - r) / span + 2 : (r - g) / span + 4;
  h = fmodf(h / 6 + 1 + 1.0f / 6, 1) * 6;
  const float s = span / hi, v = hi, f = h - (int)h;
  const float p = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
  float rr, gg, bb;
  switch ((int)h % 6) {
    case 0: rr = v; gg = u; bb = p; break;
    case 1: rr = q; gg = v; bb = p; break;
    case 2: rr = p; gg = v; bb = u; break;
    case 3: rr = p; gg = q; bb = v; break;
    case 4: rr = u; gg = p; bb = v; break;
    default: rr = v; gg = p; bb = q; break;
  }
  return ((uint32_t)(rr * 255) << 16) | ((uint32_t)(gg * 255) << 8) | (uint32_t)(bb * 255);
}

static uint32_t secondColour(const Look &look) { return look.colour2 == AUTO ? partnerOf(look.colour) : look.colour2; }

static String colourText(uint32_t colour) {
  char text[8];
  snprintf(text, sizeof text, "#%06X", (unsigned)(colour & 0xFFFFFF));
  return String(text);
}

/* -------------------------------------------------------- settings -- */

/* The schedule's and the add-ons' share of the settings; they are defined further down, beside the things they describe. */
static void extraConfigToJson(JsonDocument &doc);
/* A whole number from a request, within lo..hi: a number, or a string holding one. Anything else is refused rather than read as 0, which is what ArduinoJson makes of a number too big for an int, and 0 passed the brightness check as "off". */
static bool intIn(JsonVariantConst v, int lo, int hi, int &out) {
  double d;
  if (v.is<const char *>()) {
    const char *text = v.as<const char *>();
    char *end = nullptr;
    d = strtod(text, &end);
    if (end == text || *end) return false;
  } else if (v.is<double>()) {
    d = v.as<double>();
  } else {
    return false;
  }
  if (!(d >= lo && d <= hi)) return false;
  out = (int)d;
  return true;
}
static void faZoneChanged();   // with the file add-ons, below
static const char *applyExtraConfig(JsonVariantConst doc);

static void configToJson(JsonDocument &doc) {
  extraConfigToJson(doc);
  doc["brightness"] = brightness;
  doc["order"] = order;
  doc["reversed"] = reversed;
  doc["glow"] = glow;
#if defined(BOARD_STATUS)
  doc["stripLength"] = stripLength;
#elif defined(BOARD_RGB)
  doc["split"] = rgbSplit;
#endif
  doc["cool"]["minutes"] = coolMinutes;
  doc["cool"]["target"] = coolTarget;
  doc["mode"] = MODE_NAMES[mode];
  doc["music"]["sensitivity"] = SENS_NAMES[sensitivity];
  JsonObject states = doc["states"].to<JsonObject>();
  for (int i = 0; i < ST_OFF; i++) {
    JsonObject one = states[ST_NAMES[i]].to<JsonObject>();
    one["colour"] = colourText(looks[i].colour);
    /* Always the colour actually used, so the page's picker shows it; "colour2Auto" says it was worked out, not chosen. */
    one["colour2"] = colourText(secondColour(looks[i]));
    one["colour2Auto"] = looks[i].colour2 == AUTO;
    one["effect"] = FX_NAMES[looks[i].effect];
    one["speed"] = looks[i].speed;
    one["level"] = looks[i].level;
    one["limit"] = looks[i].limit;
    one["on"] = looks[i].on;
  }
}

/* Applies whatever is present and valid, leaves the rest alone. Returns an error message or nullptr. */
static const char *applyConfig(JsonVariantConst doc) {
  if (const char *error = applyExtraConfig(doc)) return error;
  if (!doc["brightness"].isNull()) {
    int value;
    if (!intIn(doc["brightness"], 0, 100, value)) return "Brightness is 0-100";
    brightness = value;
  }
  if (!doc["order"].isNull()) {
    const char *value = doc["order"].as<const char *>();
    static const char *ORDERS[] = {"RGB", "RBG", "GRB", "GBR", "BRG", "BGR"};
    int at = findName(ORDERS, 6, value);
    if (at < 0) return "Unknown colour order";
    strcpy(order, ORDERS[at]);
  }
  if (!doc["reversed"].isNull()) reversed = doc["reversed"].as<bool>();
  if (!doc["glow"].isNull()) glow = doc["glow"].as<bool>();
#if defined(BOARD_STATUS)
  if (!doc["stripLength"].isNull()) {
    int value;
    if (!intIn(doc["stripLength"], 1, STRIP_MAX, value)) return "The strip is 1-300 LEDs";
    stripLength = value;
  }
#elif defined(BOARD_RGB)
  if (!doc["split"].isNull()) rgbSplit = doc["split"].as<bool>();
#endif
  if (!doc["cool"]["minutes"].isNull()) {
    int value;
    if (!intIn(doc["cool"]["minutes"], 0, 120, value)) return "Cool-down is 0-120 minutes";
    coolMinutes = value;
  }
  /* 0.7.14 had this for errors alone; it is now the error look's own limit, and still taken in this form. */
  if (!doc["error"]["minutes"].isNull()) {
    int value;
    if (!intIn(doc["error"]["minutes"], 0, 1440, value)) return "An error is shown for 0-1440 minutes (0: until it clears)";
    looks[ST_ERROR].limit = value;
  }
  if (!doc["cool"]["target"].isNull()) {
    int value;
    if (!intIn(doc["cool"]["target"], 20, 60, value)) return "Bed target is 20-60";
    coolTarget = value;
  }
  if (!doc["mode"].isNull()) {
    int at = findName(MODE_NAMES, MODE_COUNT, doc["mode"].as<const char *>());
    if (at < 0) return "Mode is status or music";
#if !defined(BOARD_P2)
    if (at == MODE_MUSIC) return "Music mode needs the P2's microphone, which this board does not have";
#endif
    mode = at;
  }
  if (!doc["music"]["sensitivity"].isNull()) {
    int at = findName(SENS_NAMES, SENS_COUNT, doc["music"]["sensitivity"].as<const char *>());
    if (at < 0) return "Sensitivity is low, medium, high or auto";
    sensitivity = at;
  }
  JsonObjectConst states = doc["states"].as<JsonObjectConst>();
  for (JsonPairConst pair : states) {
    int at = findName(ST_NAMES, ST_OFF, pair.key().c_str());
    if (at < 0) return "Unknown state";
    JsonVariantConst one = pair.value();
    if (!one["colour"].isNull()) {
      uint32_t colour;
      if (!parseColour(one["colour"].as<const char *>(), colour)) return "Colour must be #RRGGBB";
      looks[at].colour = colour;
    }
    /* An automatic second colour is stored as the colour it worked out, with colour2Auto beside it. The flag is what keeps it automatic after a restart; a colour2 that is not the first colour's partner is one somebody chose, and wins. */
    uint32_t partner;
    if (one["colour2Auto"].as<bool>() && (one["colour2"].isNull() || (parseColour(one["colour2"].as<const char *>(), partner) && partner == partnerOf(looks[at].colour)))) {
      looks[at].colour2 = AUTO;
    } else if (!one["colour2"].isNull()) {
      const char *text = one["colour2"].as<const char *>();
      uint32_t colour;
      if (text && strcasecmp(text, "auto") == 0) looks[at].colour2 = AUTO;
      else if (parseColour(text, colour)) looks[at].colour2 = colour;
      else return "Second colour must be #RRGGBB or auto";
    }
    if (!one["effect"].isNull()) {
      int fx = findName(FX_NAMES, FX_COUNT, one["effect"].as<const char *>());
      if (fx < 0) return "Unknown effect";
      looks[at].effect = fx;
    }
    if (!one["speed"].isNull()) {
      int speed;
      if (!intIn(one["speed"], 1, 100, speed)) return "Speed is 1-100";
      looks[at].speed = speed;
    }
    if (!one["level"].isNull()) {
      int level;
      if (!intIn(one["level"], 0, 100, level)) return "A state's brightness is 1-100, or 0 for the bar's";
      looks[at].level = level;
    }
    if (!one["limit"].isNull()) {
      int limit;
      if (!intIn(one["limit"], 0, 1440, limit)) return "A state is shown for 0-1440 minutes (0: as long as it lasts)";
      looks[at].limit = limit;
    }
    if (!one["on"].isNull()) looks[at].on = one["on"].as<bool>();
  }
  return nullptr;
}

/* Settings are kept as a blob ("cfgb"), not a string. The settings store is
   12 KB shared with stock in the other slot, and an NVS string has to fit
   inside one 4 KB page: once no page had 1.1 KB spare, every save failed
   without a sound and each change lasted only until the next restart (an
   add-on pick kept vanishing). A blob is stored in chunks across pages, so it
   fits wherever there is room. The old "cfg" string is read once and removed
   after the first blob save, which also gives its space back. */
static bool cfgSaveOk = true;
static String cfgSaved;  // what the store holds, so an unchanged save writes nothing
static SemaphoreHandle_t cfgLock;   // the page, Home Assistant and the add-on task all save: one at a time
static void saveConfigNow() {
  JsonDocument doc;
  configToJson(doc);
  String text;
  serializeJson(doc, text);
  if (text == cfgSaved && cfgSaveOk) return;
  bool ok = prefs.putBytes("cfgb", text.c_str(), text.length()) == text.length();
  if (!ok && prefs.isKey("cfg")) {
    // Not enough room while the old string still sits there: drop it and try again, and put it back if that fails too.
    prefs.remove("cfg");
    ok = prefs.putBytes("cfgb", text.c_str(), text.length()) == text.length();
    if (!ok) prefs.putString("cfg", text);
  }
  if (ok) {
    if (prefs.isKey("cfg")) prefs.remove("cfg");
    cfgSaved = text;
  }
  cfgSaveOk = ok;
}
static void saveConfig() {
  if (cfgLock) xSemaphoreTake(cfgLock, portMAX_DELAY);
  saveConfigNow();
  if (cfgLock) xSemaphoreGive(cfgLock);
}

static void loadConfig() {
  String text;
  const size_t size = prefs.isKey("cfgb") ? prefs.getBytesLength("cfgb") : 0;
  if (size) {
    std::unique_ptr<char[]> buf(new char[size + 1]);
    prefs.getBytes("cfgb", buf.get(), size);
    buf[size] = 0;
    text = buf.get();
  } else {
    text = prefs.getString("cfg", "");
  }
  if (!text.length()) return;
  cfgSaved = text;
  JsonDocument doc;
  if (deserializeJson(doc, text)) return;
  /* A field at a time, and a look at a time, so one this build does not know (stored by a newer one, before going back to this) costs only itself, not every field after it. */
  for (JsonPairConst field : doc.as<JsonObjectConst>()) {
    if (!strcmp(field.key().c_str(), "states")) {
      for (JsonPairConst state : field.value().as<JsonObjectConst>()) {
        JsonDocument one;
        one["states"][state.key()] = state.value();
        applyConfig(one.as<JsonVariantConst>());
      }
    } else {
      JsonDocument one;
      one[field.key()] = field.value();
      applyConfig(one.as<JsonVariantConst>());
    }
  }
}

/* ---------------------------------------------------- printer link -- */

/**
 * The bar's own link to a Bambu printer, for running without a controller.
 *
 * Bambu printers publish their state over MQTT on the LAN: TLS on port 8883,
 * user "bblp", the printer's access code as the password, topic
 * device/<serial>/report. Measured on an X2D (November 2026): one message a
 * second, 31,712 bytes each. That is far too big to hold in a buffer on this
 * chip beside a TLS session, and an ordinary MQTT library would either need
 * the whole message in memory or silently drop it. So the client here is a
 * small one of its own that reads the message as a stream and picks out the
 * four values the bar needs as they go past, in a few hundred bytes of RAM
 * whatever size the printer's message is.
 *
 * The certificate is not checked: the printer's is self-issued, the link is
 * on the LAN, and the access code is what authenticates it — the same
 * arrangement the stock firmware and every LAN client has.
 */
static String prIp, prSerial, prCode;
static SemaphoreHandle_t prLock;              // the strings above and the two below are read by the link's task and written by the web handler
/* Which kind of printer: Bambu (MQTT, above) or Klipper (Moonraker's HTTP API). For Klipper, prIp is the host, prCode an optional Moonraker API key, prSerial unused. */
enum PrinterKind : uint8_t { PR_BAMBU, PR_KLIPPER };
static uint8_t prKind = PR_BAMBU;
static uint16_t prPort = 7125;
static volatile bool prRestart = false;       // settings changed: drop the link and start again
/* Set while a firmware download runs: two TLS sessions at once leave the chip
   too little memory to be sure of finishing the one that matters. */
static volatile bool prPaused = false;
/* Set for the few seconds an add-on or update download needs the memory the printer link's secure session holds. */
static volatile bool prHold = false;
static volatile bool prHoldAck = false;      // the printer task has dropped its secure session and is waiting: the memory is free now
static volatile bool prConnected = false;
static char prError[48] = "";
static char prGcode[12] = "";                 // RUNNING, PAUSE, FINISH, IDLE, FAILED, PREPARE ...
static volatile int prPercent = -1;
static volatile float prBed = NAN;
static volatile uint32_t prLastMsgAt = 0;
static const uint32_t PRINTER_STALE_MS = 30000;

static void prSetError(const char *text) { strlcpy(prError, text, sizeof prError); }

/**
 * Stream scanner: reads the printer's JSON one character at a time, keeping
 * only the path to where it is (the key of each open container) and the few
 * values the bar needs. The path is what tells a tray's "id" from an AMS
 * unit's, so the loaded filament can be found without holding the message.
 *
 * Filament, as Bambu Studio numbers it: every AMS tray has a global id, ams_id * 4 + slot for a
 * regular AMS, the unit's own id (128-135) for an AMS HT, 254/255 for the
 * external spools. A single-nozzle printer says which is loaded in
 * ams.tray_now. A dual-nozzle one (H2D, X2D) cannot say it in one number, so
 * each nozzle reports its own in device.extruder.info[].snow, encoded
 * (ams_id << 8) | slot with 0xFFFF for empty, and bit 8 of
 * device.extruder.state is the nozzle in use.
 */
static struct {
  bool inString, escaped, tooLong, hadString, awaiting, capturing, valueIsString;
  char text[24]; uint8_t length;           // the string just read; a key if a colon follows (room for "nozzle_target_temper", 20)
  char key[24];                            // the key whose value comes next
  char value[16]; uint8_t valueLength;
  char path[8][14]; uint8_t depth;         // the key of each open container, "" for an array element
  uint16_t overflow;                       // containers open below the eighth level: counted only, so their closes do not unwind the levels above
  char gcode[12]; int percent; float bed; bool gotGcode, gotPercent, gotBed;
  int trayNow; bool gotTrayNow;
  int extState; bool gotExtState;
  /* The tray, AMS unit or nozzle being read, until its object closes. */
  int slotId; uint32_t slotColour; bool slotHasColour; char slotType[10];
  int unitId; uint8_t unitCount; int unitSlot[4]; uint32_t unitColour[4]; bool unitHas[4]; char unitType[4][10];
  int extId, extSnow;
  /* 0.7.16: how much is left on each spool, the AMS's dryness, the time left and the temperatures. -2: not in this message. */
  int slotRemain; int unitRemain[4]; int unitHumid, unitHumidRaw;
  int timeLeft; float nozzle, nozzleTarget, bedTarget, chamber;
  bool gotTimeLeft, gotNozzle, gotNozzleTarget, gotBedTarget, gotChamber;
  int stage; bool gotStage;   // 0.7.19: print.stg_cur, what the printer is doing (levelling, heating the bed...)
} scan;

/* What the printer has said about its filament, kept between messages: printers send only what changed. */
struct FilamentTray { int16_t tray; uint32_t colour; char type[10]; int8_t remain; };   // remain: percent left, -1 not known
static FilamentTray filTrays[26];   // 16 AMS trays, 8 in AMS HT units, 2 external spools
static uint8_t filTrayCount = 0;
static volatile int prTrayNow = -1;          // -1 never heard
static volatile int prActiveExt = 0;
static int prSnow[2] = {-1, -1};             // -1 never heard; 0xFFFF empty
static bool prSnowSeen = false;

static void filRemember(int tray, uint32_t colour, bool has, const char *type, int remain = -2) {
  for (int i = 0; i < filTrayCount; i++)
    if (filTrays[i].tray == tray) {
      if (!has) { filTrays[i] = filTrays[--filTrayCount]; return; }   // an emptied tray is forgotten
      filTrays[i].colour = colour; strlcpy(filTrays[i].type, type, sizeof filTrays[i].type);
      if (remain != -2) filTrays[i].remain = constrain(remain, -1, 100);
      return;
    }
  if (!has || filTrayCount >= sizeof filTrays / sizeof filTrays[0]) return;
  filTrays[filTrayCount].tray = tray; filTrays[filTrayCount].colour = colour;
  strlcpy(filTrays[filTrayCount].type, type, sizeof filTrays[filTrayCount].type);
  filTrays[filTrayCount].remain = remain == -2 ? -1 : constrain(remain, -1, 100);
  filTrayCount++;
}

/* Each AMS unit's dryness, kept between messages like the trays. level: the printer's 1 (wet) to 5 (dry); raw: relative humidity in percent, from units that send it. -1: not known. */
struct AmsUnit { int16_t id; int8_t level; int8_t raw; };
static AmsUnit amsUnits[12];
static uint8_t amsCount = 0;
static void amsRemember(int id, int level, int raw) {
  if (id < 0 || (level < 0 && raw < 0)) return;
  int i = 0;
  while (i < amsCount && amsUnits[i].id != id) i++;
  if (i == amsCount) {
    if (amsCount >= sizeof amsUnits / sizeof amsUnits[0]) return;
    amsUnits[amsCount++] = {(int16_t)id, -1, -1};
  }
  if (level >= 0) amsUnits[i].level = constrain(level, 0, 5);
  if (raw >= 0) amsUnits[i].raw = constrain(raw, 0, 100);
}
static volatile int prTimeLeft = -1;                  // minutes left on the print, -1 not known
/* Bambu: the stage the printer says it is at (print.stg_cur), -1 not known. Klipper: 1 while a print has started but not yet extruded (print_duration still 0), else 0. */
static volatile int prStage = -1;
static volatile float prNozzle = NAN, prNozzleTarget = NAN, prBedTarget = NAN, prChamber = NAN;

/* "F72323FF" is colour then alpha; an alpha of 00 (or no colour) is an empty tray. */
static bool trayColour(const char *text, uint32_t &out) {
  if (strlen(text) < 6) return false;
  char hex[7]; memcpy(hex, text, 6); hex[6] = 0;
  if (strspn(hex, "0123456789abcdefABCDEF") != 6) return false;
  if (strlen(text) >= 8 && text[6] == '0' && text[7] == '0') return false;
  out = strtoul(hex, nullptr, 16);
  return true;
}

static const char *scanUp(int n) { return scan.depth >= n ? scan.path[scan.depth - n] : "\x01"; }

static void scanReset() { memset(&scan, 0, sizeof scan); scan.slotId = scan.unitId = scan.extId = scan.extSnow = -1; scan.slotRemain = -2; scan.unitHumid = scan.unitHumidRaw = -1; }

static void scanFinish() {
  scan.value[scan.valueLength] = 0;
  scan.capturing = false;
  if (scan.overflow) { scan.key[0] = 0; return; }   // nothing the bar reads is that deep, and the path no longer says where it is
  const char *k = scan.key, *v = scan.value;
  const char *p1 = scanUp(1), *p2 = scanUp(2), *p3 = scanUp(3);
  if (!strcmp(k, "gcode_state")) { strlcpy(scan.gcode, v, sizeof scan.gcode); scan.gotGcode = true; }
  else if (!strcmp(k, "mc_percent")) { scan.percent = atoi(v); scan.gotPercent = true; }
  else if (!strcmp(k, "bed_temper")) { scan.bed = atof(v); scan.gotBed = true; }
  else if (!strcmp(p1, "print") && !strcmp(k, "mc_remaining_time")) { scan.timeLeft = atoi(v); scan.gotTimeLeft = true; }
  else if (!strcmp(p1, "print") && !strcmp(k, "nozzle_temper")) { scan.nozzle = atof(v); scan.gotNozzle = true; }
  else if (!strcmp(p1, "print") && !strcmp(k, "nozzle_target_temper")) { scan.nozzleTarget = atof(v); scan.gotNozzleTarget = true; }
  else if (!strcmp(p1, "print") && !strcmp(k, "bed_target_temper")) { scan.bedTarget = atof(v); scan.gotBedTarget = true; }
  else if (!strcmp(p1, "print") && !strcmp(k, "chamber_temper")) { scan.chamber = atof(v); scan.gotChamber = true; }
  else if (!strcmp(p1, "print") && !strcmp(k, "stg_cur")) { scan.stage = atoi(v); scan.gotStage = true; }
  else if (!strcmp(k, "tray_now") && !strcmp(p1, "ams")) { scan.trayNow = atoi(v); scan.gotTrayNow = true; }
  else if (!strcmp(k, "state") && !strcmp(p1, "extruder")) { scan.extState = atoi(v); scan.gotExtState = true; }
  /* A tray: inside ams.ams[].tray[], or an external spool (vt_tray, or vir_slot[] on dual-nozzle printers). */
  else if ((!p1[0] && (!strcmp(p2, "tray") || !strcmp(p2, "vir_slot"))) || !strcmp(p1, "vt_tray")) {
    if (!strcmp(k, "id")) scan.slotId = atoi(v);
    else if (!strcmp(k, "tray_color")) scan.slotHasColour = trayColour(v, scan.slotColour);
    else if (!strcmp(k, "tray_type")) strlcpy(scan.slotType, v, sizeof scan.slotType);
    else if (!strcmp(k, "remain")) scan.slotRemain = atoi(v);
  }
  /* An AMS unit: an element of ams.ams[]. */
  else if (!p1[0] && !strcmp(p2, "ams")) {
    if (!strcmp(k, "id")) scan.unitId = atoi(v);
    else if (!strcmp(k, "humidity")) scan.unitHumid = atoi(v);
    else if (!strcmp(k, "humidity_raw")) scan.unitHumidRaw = atoi(v);
  }
  /* A nozzle: an element of device.extruder.info[]. */
  else if (!p1[0] && !strcmp(p2, "info") && !strcmp(p3, "extruder")) {
    if (!strcmp(k, "id")) scan.extId = atoi(v);
    else if (!strcmp(k, "snow")) scan.extSnow = atoi(v);
  }
  scan.key[0] = 0;
}

static void scanTrayDone(bool external) {
  if (scan.slotId >= 0) {
    if (external) filRemember(scan.slotId, scan.slotColour, scan.slotHasColour, scan.slotType, scan.slotRemain);
    else if (scan.unitCount < 4) {
      const int i = scan.unitCount++;
      scan.unitSlot[i] = scan.slotId; scan.unitColour[i] = scan.slotColour; scan.unitHas[i] = scan.slotHasColour; scan.unitRemain[i] = scan.slotRemain;
      strlcpy(scan.unitType[i], scan.slotType, sizeof scan.unitType[i]);
    }
  }
  scan.slotId = -1; scan.slotHasColour = false; scan.slotType[0] = 0; scan.slotRemain = -2;
}

/* A container closes: whatever it held is complete. */
static void scanClose() {
  if (scan.overflow) { scan.overflow--; return; }
  if (!scan.depth) return;
  const bool element = !scan.path[scan.depth - 1][0];
  const char *closing = scan.path[scan.depth - 1];
  scan.depth--;
  const char *p1 = scanUp(1), *p2 = scanUp(2);
  if (element && !strcmp(p1, "tray")) scanTrayDone(false);
  else if ((element && !strcmp(p1, "vir_slot")) || !strcmp(closing, "vt_tray")) scanTrayDone(true);
  else if (element && !strcmp(p1, "ams")) {
    /* The unit's id may come before or after its trays, so they are placed only once the unit is complete. */
    if (scan.unitId >= 0)
      for (int i = 0; i < scan.unitCount; i++)
        filRemember(scan.unitId >= 128 ? scan.unitId : scan.unitId * 4 + scan.unitSlot[i], scan.unitColour[i], scan.unitHas[i], scan.unitType[i], scan.unitRemain[i]);
    amsRemember(scan.unitId, scan.unitHumid, scan.unitHumidRaw);
    scan.unitId = -1; scan.unitCount = 0; scan.unitHumid = scan.unitHumidRaw = -1;
  } else if (element && !strcmp(p1, "info") && !strcmp(p2, "extruder")) {
    if (scan.extId == 0 || scan.extId == 1) { prSnow[scan.extId] = scan.extSnow; prSnowSeen = true; }
    scan.extId = scan.extSnow = -1;
  }
}

static void scanOpen(const char *key) {
  /* Too deep to matter: nothing the bar reads lives there. It used to overwrite the eighth level's key and not count the level, so its close then unwound one that was still open, and the trays after it were read in the wrong place. */
  if (scan.depth < 8) { strlcpy(scan.path[scan.depth], key, sizeof scan.path[0]); scan.depth++; }
  else if (scan.overflow < 0xFFFF) scan.overflow++;
}

static void scanFeed(char c) {
  if (scan.capturing) {
    if (scan.valueIsString) {
      if (scan.escaped) scan.escaped = false;
      else if (c == '\\') { scan.escaped = true; return; }
      else if (c == '"') { scanFinish(); return; }
      if (scan.valueLength < sizeof scan.value - 1) scan.value[scan.valueLength++] = c;
      return;
    }
    if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\r') {
      scanFinish();
      if (c == '}' || c == ']') scanClose();
      return;
    }
    if (scan.valueLength < sizeof scan.value - 1) scan.value[scan.valueLength++] = c;
    return;
  }
  if (scan.inString) {
    if (scan.escaped) scan.escaped = false;
    else if (c == '\\') scan.escaped = true;
    else if (c == '"') { scan.inString = false; scan.hadString = true; scan.text[scan.length] = 0; }
    else if (scan.length < sizeof scan.text - 1) scan.text[scan.length++] = c;
    else scan.tooLong = true;
    return;
  }
  if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return;
  if (c == '"') {
    if (scan.awaiting) { scan.awaiting = false; scan.capturing = true; scan.valueIsString = true; scan.valueLength = 0; return; }
    scan.inString = true; scan.length = 0; scan.tooLong = false; scan.hadString = false;
    return;
  }
  if (c == ':') {
    /* A string followed by a colon was a key. */
    if (scan.hadString && !scan.tooLong) { strlcpy(scan.key, scan.text, sizeof scan.key); scan.awaiting = true; }
    scan.hadString = false;
    return;
  }
  scan.hadString = false;
  if (c == '{' || c == '[') { scanOpen(scan.awaiting ? scan.key : ""); scan.awaiting = false; scan.key[0] = 0; return; }
  if (c == '}' || c == ']') { scanClose(); return; }
  if (c == ',') return;
  if (scan.awaiting) {
    scan.awaiting = false;
    scan.capturing = true; scan.valueIsString = false; scan.valueLength = 0;
    scan.value[scan.valueLength++] = c;
  }
}

/* Printers that send only what changed leave fields out, so each value is kept until a newer one arrives. */
static void scanCommit() {
  if (scan.gotGcode) strlcpy(prGcode, scan.gcode, sizeof prGcode);
  if (scan.gotPercent) prPercent = scan.percent;
  if (scan.gotBed) prBed = scan.bed;
  if (scan.gotTrayNow) prTrayNow = scan.trayNow;
  if (scan.gotExtState) prActiveExt = (scan.extState >> 8) & 1;
  if (scan.gotTimeLeft) prTimeLeft = scan.timeLeft;
  if (scan.gotNozzle) prNozzle = scan.nozzle;
  if (scan.gotNozzleTarget) prNozzleTarget = scan.nozzleTarget;
  if (scan.gotBedTarget) prBedTarget = scan.bedTarget;
  if (scan.gotChamber) prChamber = scan.chamber;
  if (scan.gotStage) prStage = scan.stage;
  prLastMsgAt = millis();
}

/**
 * The filament in use, as a global tray id, or -1 for none. On a
 * dual-nozzle printer the active nozzle's own slot decides; otherwise
 * tray_now, where 255 means nothing loaded.
 */
static int filLoadedTray() {
  if (prSnowSeen) {
    const int s = prSnow[prActiveExt];
    if (s < 0 || s == 0xFFFF) return -1;
    const int ams = (s >> 8) & 255, slot = s & 255;
    if (ams == 255) return slot == 0 ? 254 : 255;
    return ams >= 128 ? ams : ams * 4 + slot;
  }
  return prTrayNow < 0 || prTrayNow == 255 ? -1 : prTrayNow;
}

static const FilamentTray *filFind(int tray) {
  for (int i = 0; i < filTrayCount; i++) if (filTrays[i].tray == tray) return &filTrays[i];
  return nullptr;
}

/* The last filament seen loaded, kept so the bar can still say it between prints (a dual-nozzle printer unloads after each one). */
static FilamentTray filLast = {-1, 0, "", -1};

static bool netRead(WiFiClientSecure &net, uint8_t *into, size_t count, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t idleFrom = millis();
  while (got < count) {
    if (prRestart) return false;
    int ready = net.available();
    if (ready > 0) {
      int n = net.read(into + got, min((size_t)ready, count - got));
      if (n > 0) { got += n; idleFrom = millis(); continue; }
    }
    if (!net.connected()) return false;
    if (millis() - idleFrom > timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(4));
  }
  return true;
}

static bool netSkip(WiFiClientSecure &net, uint32_t count, uint8_t *scratch, size_t scratchSize) {
  while (count) {
    size_t n = min((size_t)count, scratchSize);
    if (!netRead(net, scratch, n, 8000)) return false;
    count -= n;
  }
  return true;
}

static size_t putString(uint8_t *at, const String &text) {
  at[0] = text.length() >> 8;
  at[1] = text.length() & 255;
  memcpy(at + 2, text.c_str(), text.length());
  return 2 + text.length();
}

static void prWait(uint32_t ms) {
  uint32_t from = millis();
  while (!prRestart && millis() - from < ms) vTaskDelay(pdMS_TO_TICKS(200));
}

/**
 * Klipper, through Moonraker: one small query every two seconds, mapped onto
 * the same words the Bambu link uses (RUNNING, PAUSE, FINISH, FAILED, IDLE) so
 * the cool-down and everything after it is shared. Moonraker's query answer
 * is a few hundred bytes, so plain polling is simpler and lighter here than
 * holding its websocket open. Klipper itself shut down or in error counts as
 * an error on the bar: the printer cannot print in that state.
 */
static void klipperPoll(const String &host, uint16_t port, const String &key) {
  String address = host;
  /* The HTTP client does not look up .local names itself; mDNS does. */
  if (endsWithLocal(host)) {
    IPAddress found = MDNS.queryHost(host.substring(0, host.length() - 6), 2000);
    if (found == IPAddress()) { prConnected = false; prSetError("Could not find that .local name"); prWait(10000); return; }
    address = found.toString();
  }
  WiFiClient net;
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  const String url = "http://" + address + ":" + String(port) +
    "/printer/objects/query?webhooks=state&print_stats=state,print_duration&display_status=progress&virtual_sdcard=progress&heater_bed=temperature,target&extruder=temperature,target";
  if (!http.begin(net, url)) { prConnected = false; prSetError("That address does not work"); prWait(15000); return; }
  if (key.length()) http.addHeader("X-Api-Key", key);
  http.useHTTP10(true);   // the answer is read straight off the connection below, which a chunked HTTP/1.1 reply would garble
  const int code = http.GET();
  if (code <= 0) { http.end(); prConnected = false; prSetError("Klipper not reachable"); prWait(10000); return; }
  if (code == 401 || code == 403) { http.end(); prConnected = false; prSetError("Moonraker wants an API key"); prWait(30000); return; }
  if (code != 200) {
    http.end(); prConnected = false;
    char text[40]; snprintf(text, sizeof text, "Moonraker answered %d", code); prSetError(text);
    prWait(10000); return;
  }
  JsonDocument filter;
  filter["result"]["status"] = true;
  JsonDocument doc;
  const DeserializationError bad = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (bad) { prConnected = false; prSetError("Moonraker's answer did not read"); prWait(10000); return; }
  JsonObjectConst st = doc["result"]["status"];
  /* Without it every field below falls back to "ready" and "standby", and an answer from something that is not Moonraker read as an idle printer. */
  if (st.isNull()) { prConnected = false; prSetError("That does not look like Moonraker"); prWait(10000); return; }
  const char *hooks = st["webhooks"]["state"] | "ready";
  const char *ps = st["print_stats"]["state"] | "standby";
  const char *gcode = "IDLE";
  if (!strcmp(hooks, "shutdown") || !strcmp(hooks, "error")) gcode = "FAILED";
  else if (!strcmp(ps, "printing")) gcode = "RUNNING";
  else if (!strcmp(ps, "paused")) gcode = "PAUSE";
  else if (!strcmp(ps, "complete")) gcode = "FINISH";
  else if (!strcmp(ps, "error")) gcode = "FAILED";
  strlcpy(prGcode, gcode, sizeof prGcode);
  /* display_status follows M73 from the slicer when it sends it; virtual_sdcard is the file position. The larger is the better guess. */
  const float shown = max(st["display_status"]["progress"] | -1.0f, st["virtual_sdcard"]["progress"] | -1.0f);
  prPercent = shown < 0 ? -1 : (int)lroundf(shown * 100);
  prBed = st["heater_bed"]["temperature"].isNull() ? NAN : st["heater_bed"]["temperature"].as<float>();
  prBedTarget = st["heater_bed"]["target"].isNull() ? NAN : st["heater_bed"]["target"].as<float>();
  prNozzle = st["extruder"]["temperature"].isNull() ? NAN : st["extruder"]["temperature"].as<float>();
  prNozzleTarget = st["extruder"]["target"].isNull() ? NAN : st["extruder"]["target"].as<float>();
  /* Moonraker gives the time spent, not the time left: worked out from the progress once there is enough of it to go on. */
  {
    const float spent = st["print_stats"]["print_duration"] | -1.0f;
    prTimeLeft = !strcmp(gcode, "RUNNING") && spent > 0 && shown > 0.02f ? (int)lroundf(spent * (1 - shown) / shown / 60) : -1;
    /* Klipper counts print_duration from the first extrusion: before it, the start G-code is homing, heating and levelling. */
    prStage = !strcmp(gcode, "RUNNING") && spent == 0 ? 1 : 0;
  }
  prLastMsgAt = millis();
  prConnected = true;
  prSetError("");
  prWait(2000);
}

static void printerTask(void *) {
  static uint8_t buf[512];
  static uint8_t packet[200];
  for (;;) {
    prRestart = false;   // before the settings are read, so a change made while they are being read asks again rather than being lost
    xSemaphoreTake(prLock, portMAX_DELAY);
    String ip = prIp, serial = prSerial, code = prCode;
    const uint8_t kind = prKind;
    const uint16_t port = prPort;
    xSemaphoreGive(prLock);
    /* A different printer (or none) starts from nothing: what the last one said, its trays and its nozzles are not this one's. Done here, by the only task that writes them. */
    static String usedIp, usedSerial;
    static uint8_t usedKind = 255;
    static uint16_t usedPort = 0;
    if (ip != usedIp || serial != usedSerial || kind != usedKind || port != usedPort) {
      usedIp = ip; usedSerial = serial; usedKind = kind; usedPort = port;
      prGcode[0] = 0; prPercent = -1; prBed = NAN; prLastMsgAt = 0;
      filTrayCount = 0; prTrayNow = -1; prActiveExt = 0;
      prSnow[0] = prSnow[1] = -1; prSnowSeen = false;
      amsCount = 0; prTimeLeft = -1; prNozzle = prNozzleTarget = prBedTarget = prChamber = NAN; prStage = -1;
    }
    /* Acknowledged here too: the link is already down, and a download that asks for it to be let go would otherwise wait out its whole patience. */
    if (prPaused) { prConnected = false; prHoldAck = true; prSetError("Paused while a firmware update downloads"); prWait(1000); continue; }
    if (prHold) { prConnected = false; prHoldAck = true; prSetError("Let go for a moment while an add-on downloads"); prWait(250); continue; }
    if (!ip.length() || (kind == PR_BAMBU && !serial.length())) { prConnected = false; prSetError(""); prWait(2000); continue; }
    if (WiFi.status() != WL_CONNECTED) { prConnected = false; prSetError("Waiting for Wi-Fi"); prWait(2000); continue; }
    /* Klipper's link stays "connected" between polls, so it is not reset at the top of each pass. */
    if (kind == PR_KLIPPER) { klipperPoll(ip, port, code); continue; }
    prConnected = false;

    WiFiClientSecure net;
    net.setInsecure();
    net.setHandshakeTimeout(10);
    if (!net.connect(ip.c_str(), 8883)) { prSetError("Printer not reachable"); net.stop(); prWait(15000); continue; }

    /* CONNECT: MQTT 3.1.1, clean session, user name and password, 60 s keep-alive. */
    String clientId = "panda-" + String((uint32_t)(ESP.getEfuseMac() >> 24), HEX);
    size_t n = 0;
    uint8_t body[160];
    n += putString(body + n, "MQTT");
    body[n++] = 4; body[n++] = 0xC2; body[n++] = 0; body[n++] = 60;
    n += putString(body + n, clientId);
    n += putString(body + n, "bblp");
    n += putString(body + n, code);
    packet[0] = 0x10; packet[1] = n;   // every field is short, so the length fits in one byte
    memcpy(packet + 2, body, n);
    net.write(packet, n + 2);
    if (!netRead(net, buf, 4, 10000) || buf[0] != 0x20) { prSetError("Printer did not answer the sign-in"); net.stop(); prWait(15000); continue; }
    if (buf[3] != 0) {
      prSetError(buf[3] == 4 || buf[3] == 5 ? "Wrong access code" : "Printer refused the sign-in");   // the serial is not part of the sign-in, so it cannot be what was refused
      net.stop(); prWait(30000); continue;
    }

    /* SUBSCRIBE to the report topic, then the standard "send me everything" request. */
    String topic = "device/" + serial + "/report";
    n = 0;
    body[n++] = 0; body[n++] = 1;
    n += putString(body + n, topic);
    body[n++] = 0;
    packet[0] = 0x82; packet[1] = n; memcpy(packet + 2, body, n);
    net.write(packet, n + 2);
    String request = "device/" + serial + "/request";
    const char *pushall = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}";
    n = putString(body, request);
    memcpy(body + n, pushall, strlen(pushall)); n += strlen(pushall);
    packet[0] = 0x30; packet[1] = n; memcpy(packet + 2, body, n);
    net.write(packet, n + 2);

    prConnected = true;
    prSetError("");
    uint32_t lastPing = millis(), lastHeard = millis();
    const uint32_t linkedAt = millis();
    bool reported = false;
    while (!prRestart) {
      /* The printer takes the sign-in with any serial number, and the subscription to a wrong one simply never delivers: the page said "waiting for its first message" for ever. A pushall is answered at once, so a long silence means the serial. */
      if (!reported && !prError[0] && millis() - linkedAt > 20000) prSetError("No reports: check the printer's serial");
      /* Every 10 s rather than every 30: an answer to one is what keeps a quiet printer's last report counted as current (below). */
      if (millis() - lastPing > 10000) { const uint8_t ping[2] = {0xC0, 0}; net.write(ping, 2); lastPing = millis(); }
      if (millis() - lastHeard > 90000) break;
      if (net.available() <= 0) { if (!net.connected()) break; vTaskDelay(pdMS_TO_TICKS(10)); continue; }
      if (!netRead(net, buf, 1, 5000)) break;
      const uint8_t head = buf[0];
      uint32_t length = 0; int shift = 0; bool ok = true;
      do {
        if (!netRead(net, buf, 1, 5000)) { ok = false; break; }
        length |= (uint32_t)(buf[0] & 0x7F) << shift;
        shift += 7;
      } while ((buf[0] & 0x80) && shift < 28);
      if (!ok) break;
      lastHeard = millis();
      /* PINGRESP. Some printers report only what changes, and a cold idle one can say nothing for minutes: its last report stays true while it still answers, so it is not shown as offline after 30 s. Only once this link has had a real report. */
      if ((head >> 4) == 13 && reported) prLastMsgAt = millis();   // this link's own report, not one left from before a reconnect: a print that ended meanwhile would otherwise stay "printing"
      if ((head >> 4) != 3) { if (!netSkip(net, length, buf, sizeof buf)) break; continue; }
      /* PUBLISH: topic, then (QoS above 0 only) a packet id, then the JSON. */
      if (length < 2 || !netRead(net, buf, 2, 5000)) break;
      uint32_t topicLength = ((uint32_t)buf[0] << 8) | buf[1];
      uint32_t left = length - 2;
      uint32_t skip = topicLength + ((head & 0x06) ? 2 : 0);
      if (skip > left || !netSkip(net, skip, buf, sizeof buf)) break;
      left -= skip;
      scanReset();
      while (left) {
        size_t chunk = min((size_t)left, sizeof buf);
        if (!netRead(net, buf, chunk, 8000)) { ok = false; break; }
        for (size_t i = 0; i < chunk; i++) scanFeed((char)buf[i]);
        left -= chunk;
      }
      if (!ok) break;
      scanCommit();
      if (!reported) { reported = true; if (prError[0]) prSetError(""); }
    }
    net.stop();
    prConnected = false;
    if (!prRestart) { prSetError("Lost the printer; reconnecting"); prWait(5000); }
  }
}

/**
 * The pairing the stock firmware had, read from its saved settings so a bar
 * that was already paired stays paired. One 57-byte record (namespace
 * app_nvs, key bambu_mqtt_info): address at byte 0 (16 bytes), access code at
 * byte 16 (9 bytes), serial at byte 25 (16 bytes). As with the Wi-Fi record,
 * the layout was worked out from where the fields sit, not from their values.
 */
static bool readStockPairing(String &ip, String &serial, String &code) {
  nvs_handle_t handle;
  if (nvs_open("app_nvs", NVS_READONLY, &handle) != ESP_OK) return false;
  uint8_t blob[57];
  size_t length = sizeof blob;
  esp_err_t result = nvs_get_blob(handle, "bambu_mqtt_info", blob, &length);
  nvs_close(handle);
  if (result != ESP_OK || length < 41) return false;
  blob[15] = 0; blob[24] = 0; blob[40] = 0;
  ip = String((const char *)blob);
  code = String((const char *)blob + 16);
  serial = String((const char *)blob + 25);
  return ip.length() > 0 && serial.length() > 0;
}

/* What the printer link alone would have the bar show. When a print was seen to end, and the hottest the bed has been since. */
static uint32_t finishedAt = 0;
static uint32_t finishSeenAt = 0;   // when this bar first saw the print finished: the end itself, or its own start-up if that came later
static float finishedHot = NAN;
static char lastGcode[12] = "";

static bool gcodeIs(const char *what) { return strcmp(prGcode, what) == 0; }
static bool printerFresh() { return prConnected && prLastMsgAt && millis() - prLastMsgAt < PRINTER_STALE_MS; }

static void trackPrinter() {
  if (!printerFresh()) return;
  const bool wasBusy = !strcmp(lastGcode, "RUNNING") || !strcmp(lastGcode, "PAUSE") || !strcmp(lastGcode, "PREPARE");
  if (gcodeIs("FINISH")) {
    if (!finishSeenAt) finishSeenAt = millis() | 1;
    if (wasBusy) { finishedAt = millis() | 1; finishedHot = prBed; }
    if (!isnan(prBed) && (isnan(finishedHot) || prBed > finishedHot)) finishedHot = prBed;
  } else {
    finishedAt = 0;
    finishSeenAt = 0;
    finishedHot = NAN;
  }
  strlcpy(lastGcode, prGcode, sizeof lastGcode);
}

/* Getting ready to print: what Bambu printers call each step before the first layer (stg_cur), or Klipper before its first extrusion. Only at the very start of a print, so a filament change that heats the nozzle halfway through is still printing. */
static bool printerPreparing() {
  const int st = prStage;
  if (prKind == PR_KLIPPER) return st == 1;
  if (prPercent > 1) return false;
  switch (st) {
    case 1:  // auto bed levelling
    case 2:  // heating the bed
    case 3:  // vibration compensation
    case 7:  // heating the nozzle
    case 8:  // calibrating extrusion
    case 9:  // scanning the bed surface
    case 11: // identifying the build plate
    case 12: // calibrating the micro lidar
    case 13: // homing
    case 14: // cleaning the nozzle tip
    case 15: // checking the extruder temperature
    case 18: // calibrating the micro lidar
    case 19: // calibrating extrusion flow
    case 25: // calibrating motor noise
      return true;
    default:
      return false;
  }
}

static State printerState(float &fill) {
  fill = prPercent < 0 ? 0 : prPercent;
  if (gcodeIs("PAUSE")) return ST_PAUSED;
  if (gcodeIs("PREPARE") || gcodeIs("SLICING") || (gcodeIs("RUNNING") && printerPreparing())) return ST_PREPARING;
  if (gcodeIs("RUNNING")) return ST_PRINTING;
  if (gcodeIs("FAILED")) return ST_ERROR;
  if (!gcodeIs("FINISH")) return ST_IDLE;
  /* Finished. Both halves of the cool-down as "how much is left"; the bar drains with whichever has further to go. If the bar started up after the print ended it does not know when that was, so only the bed can speak. */
  const float timeLeft = finishedAt && coolMinutes ? max(0.0f, 1.0f - (millis() - finishedAt) / (coolMinutes * 60000.0f)) : 0;
  float bedLeft = 0;
  if (!isnan(prBed) && prBed > coolTarget) {
    const float span = max(1.0f, (isnan(finishedHot) ? prBed : finishedHot) - coolTarget);
    bedLeft = min(1.0f, (prBed - coolTarget) / span);
  }
  const float left = max(timeLeft, bedLeft);
  if (left > 0) { fill = left * 100; return ST_COOLING; }
  /* Nothing reports the part being taken off, so "ready" is not held for ever. */
  const uint32_t since = finishedAt ? finishedAt : finishSeenAt;
  if (since && millis() - since > 2UL * 3600000UL) return ST_IDLE;
  return ST_READY;
}

/* ----------------------------------------------------------- music -- */

/**
 * Music mode: the bar as a sound-level meter, as the stock firmware had it.
 *
 * The microphone is an ES8311 codec: settings over I2C (address 0x18), sound
 * over I2S. The pins are the ones the stock board file names (HARDWARE.md).
 * The register values are the standard ES8311 start-up for a 16 kHz, 16-bit
 * microphone with the chip clocked from our MCLK at 256 x the sample rate.
 *
 * BTT's own description of the stock effect is all there is to go on: the
 * lights "pulse rhythmically with the audio level, changing color from Blue
 * to Green to Yellow to Orange to Red". Here that is a meter that fills along
 * the bar, each LED taking its colour from where it sits on that scale.
 */
static bool codecFound = false;
static bool audioRunning = false;
/* The microphone start-up is the one part of this firmware written without the hardware on the bench. If it ever crashes the chip, the bar must still come back up reachable for another update — so the attempt is written down before it is made, and a build whose last attempt never got as far as running leaves the microphone alone. A new build tries again. */
static bool audioBlocked = false;
static bool audioConfirmed = false;
static volatile float micDb = -90;       // level of the last block, dB relative to full scale
static volatile float musicLevel = 0;    // 0-1, what the meter draws

static bool codecWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool codecInit() {
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Wire.beginTransmission(0x18);
  if (Wire.endTransmission() != 0) return false;
  codecWrite(0x00, 0x1F);   // reset
  delay(20);
  codecWrite(0x00, 0x00);
  codecWrite(0x00, 0x80);   // power on, the chip follows our clocks
  codecWrite(0x01, 0x3F);   // all clocks on, MCLK from its pin
  codecWrite(0x02, 0x00);   // MCLK 4.096 MHz used as it is
  codecWrite(0x03, 0x10);   // ADC oversampling
  codecWrite(0x04, 0x10);
  codecWrite(0x05, 0x00);
  codecWrite(0x06, 0x03);
  codecWrite(0x07, 0x00);
  codecWrite(0x08, 0xFF);
  codecWrite(0x09, 0x0C);   // 16-bit I2S in both directions
  codecWrite(0x0A, 0x0C);
  codecWrite(0x0D, 0x01);   // analogue section on
  codecWrite(0x0E, 0x02);   // microphone amplifier and ADC on
  codecWrite(0x12, 0x00);
  codecWrite(0x13, 0x10);
  codecWrite(0x1C, 0x6A);   // no ADC equaliser, remove DC
  codecWrite(0x37, 0x08);
  codecWrite(0x17, 0xC8);   // ADC volume
  codecWrite(0x14, 0x1A);   // analogue microphone, amplifier at its top step
  codecWrite(0x16, 0x04);   // +24 dB
  return true;
}

/* No sound output. 0.5.0 drove the chip's DAC for a test tone and nothing
   was heard at any volume, so the P2 appears to have no speaker fitted, and
   the tone was taken out again in 0.5.2. The audio port went back to
   input only. HARDWARE.md has the measurements. */

static void audioTask(void *) {
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = 16000;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.dma_buf_count = 4;
  config.dma_buf_len = 256;
  config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_MCLK;
  pins.bck_io_num = PIN_BCK;
  pins.ws_io_num = PIN_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_DIN;
  if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK || i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) vTaskDelete(nullptr);
  /* The codec is set up once its clock is running. */
  codecFound = codecInit();
  audioRunning = true;

  static int16_t block[512];   // 256 frames of left + right
  float floorDb = -60, peakDb = -30;
  for (;;) {
    size_t got = 0;
    if (i2s_read(I2S_NUM_0, block, sizeof block, &got, pdMS_TO_TICKS(250)) != ESP_OK || got < 8) continue;
    const int frames = got / 4;
    /* The chip is mono and which slot it answers in is not documented for this board, so both are measured and the louder one used. */
    float best = 0;
    for (int channel = 0; channel < 2; channel++) {
      long sum = 0;
      for (int i = 0; i < frames; i++) sum += block[i * 2 + channel];
      const float mean = (float)sum / frames;
      float squares = 0;
      for (int i = 0; i < frames; i++) { const float s = block[i * 2 + channel] - mean; squares += s * s; }
      const float rms = sqrtf(squares / frames) / 32768.0f;
      if (rms > best) best = rms;
    }
    const float db = 20.0f * log10f(best + 1e-6f);
    micDb = db;

    /* What counts as silent and as full, per setting. "auto" lets both drift towards what the room is doing. */
    float low, high;
    if (sensitivity == SENS_AUTO) {
      floorDb = db < floorDb ? db : floorDb + 0.02f;
      peakDb = db > peakDb ? db : peakDb - 0.03f;
      if (peakDb < floorDb + 12) peakDb = floorDb + 12;
      low = floorDb + 3; high = peakDb;
    } else {
      low = sensitivity == SENS_LOW ? -40 : sensitivity == SENS_MEDIUM ? -50 : -60;
      high = low + 35;
    }
    const float level = constrain((db - low) / (high - low), 0.0f, 1.0f);
    /* Up at once, down slowly: a meter that falls as fast as it rises just flickers. */
    musicLevel = level > musicLevel ? level : musicLevel * 0.88f + level * 0.12f;
  }
}

static void meterColour(float at, uint8_t &r, uint8_t &g, uint8_t &b) {
  static const uint8_t STOPS[5][3] = {{0, 0, 255}, {0, 255, 0}, {255, 255, 0}, {255, 128, 0}, {255, 0, 0}};
  const float x = constrain(at, 0.0f, 1.0f) * 4;
  const int i = x >= 4 ? 3 : (int)x;
  const float f = x - i;
  r = STOPS[i][0] + (STOPS[i + 1][0] - STOPS[i][0]) * f;
  g = STOPS[i][1] + (STOPS[i + 1][1] - STOPS[i][1]) * f;
  b = STOPS[i][2] + (STOPS[i + 1][2] - STOPS[i][2]) * f;
}

/* A short message on the LEDs after a button press: how many quarters of the bar to light, in white, and until when. */
static uint32_t noticeUntil = 0;
static uint8_t noticeQuarters = 0;

/* -------------------------------------------------------- schedule -- */

/**
 * A dark spell by the clock, kept on the bar itself so it works with nothing
 * else running.
 *
 * The bar has no clock of its own: the time comes from the internet (NTP)
 * once Wi-Fi is up, and the time zone is a POSIX rule string, which is what
 * the chip's C library understands and which carries the daylight-saving
 * dates with it. The page works that string out from the browser's zone.
 * Until the time is known the schedule does nothing — a bar that guessed the
 * time and went dark at the wrong hour would look broken.
 *
 * It does not change the brightness setting; it overrides it while the dark
 * spell lasts. So nothing has to remember what to put back in the morning.
 */
static bool schedOn = false;
static uint16_t schedFrom = 21 * 60, schedTo = 9 * 60;   // minutes after midnight, local time
static uint8_t schedLevel = 0;                           // brightness during the dark spell
static char timeTz[48] = "UTC0";
static char timeZone[40] = "UTC";                        // the zone's ordinary name, for the page only

static bool timeKnown() { return time(nullptr) > 1700000000; }

static int minutesNow() {
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  return local.tm_hour * 60 + local.tm_min;
}

static bool schedActive() {
  if (!schedOn || !timeKnown() || schedFrom == schedTo) return false;
  const int now = minutesNow();
  return schedFrom < schedTo ? (now >= schedFrom && now < schedTo) : (now >= schedFrom || now < schedTo);
}

static void applyTimeZone() { configTzTime(timeTz, "pool.ntp.org", "time.google.com", "time.cloudflare.com"); }

static bool parseClock(const char *text, uint16_t &out) {
  if (!text || strlen(text) != 5 || text[2] != ':') return false;
  if (!isdigit((unsigned char)text[0]) || !isdigit((unsigned char)text[1]) || !isdigit((unsigned char)text[3]) || !isdigit((unsigned char)text[4])) return false;
  const int hours = (text[0] - '0') * 10 + (text[1] - '0'), minutes = (text[3] - '0') * 10 + (text[4] - '0');
  if (hours > 23 || minutes > 59) return false;
  out = hours * 60 + minutes;
  return true;
}

static String clockText(uint16_t minutes) {
  char text[6];
  snprintf(text, sizeof text, "%02u:%02u", minutes / 60, minutes % 60);
  return String(text);
}

/* --------------------------------------------------------- add-ons -- */

/**
 * Add-ons: anything else that wants to use the bar, so anyone can build on
 * it — for example, a meter of an AI
 * assistant's usage for when the printer is doing nothing).
 *
 * An add-on is a program somewhere on the network, not code on the bar: it
 * introduces itself by posting what it wants shown (POST /api/addon), and
 * keeps posting. Each post is good for its "ttl" and then lapses, so a
 * script that has died stops being shown without anyone tidying up. The bar
 * remembers only which add-on its owner picked and when it may show.
 *
 * Three ways to say what to show, from least work to most control:
 *   look   - a colour and one of the bar's own effects, with a fill level
 *   meters - up to four values, each a section of the bar in its own colour
 *   frame  - all 25 LEDs, one colour each; post again to animate
 */
enum AddonKind : uint8_t { ADDON_LOOK, ADDON_METERS, ADDON_FRAME };
static const char *ADDON_KINDS[] = {"look", "meters", "frame"};
struct Addon {
  bool used;
  char id[16];
  char name[28];
  uint32_t at, ttlMs;
  uint8_t kind;
  Look look; float fill;
  uint8_t meterCount; float meterValue[4]; uint32_t meterColour[4];
  uint8_t pixels[LED_COUNT][3];
  bool duringPrint;   // shown over a print too, whatever "show it" says: it is about the print (time left, heating up)
};
static const int ADDON_SLOTS = 4;
static Addon addons[ADDON_SLOTS];
static char addonShow[16] = "";                 // the add-on the owner picked; empty for none
enum AddonWhen : uint8_t { WHEN_IDLE, WHEN_ALWAYS, WHEN_COUNT };
static const char *WHEN_NAMES[WHEN_COUNT] = {"idle", "always"};
static uint8_t addonWhen = WHEN_IDLE;

static bool addonFresh(const Addon &a) { return a.used && millis() - a.at < a.ttlMs; }

static Addon *addonById(const char *id) {
  for (int i = 0; i < ADDON_SLOTS; i++) if (addons[i].used && strcmp(addons[i].id, id) == 0) return &addons[i];
  return nullptr;
}

/* The picked add-on, if it is still posting and this is a moment it may show. "idle" means the printer is not busy with a print: printing, paused, cooling and errors keep the bar. */
static const Addon *addonNow(State state) {
  if (!addonShow[0]) return nullptr;
  const Addon *a = addonById(addonShow);
  if (!a || !addonFresh(*a)) return nullptr;
  if (addonWhen == WHEN_ALWAYS || a->duringPrint) return a;
  return state == ST_IDLE || state == ST_READY || state == ST_FINISHED || state == ST_OFFLINE || state == ST_OFF ? a : nullptr;
}

static void extraConfigToJson(JsonDocument &doc) {
  JsonObject schedule = doc["schedule"].to<JsonObject>();
  schedule["enabled"] = schedOn;
  schedule["from"] = clockText(schedFrom);
  schedule["to"] = clockText(schedTo);
  schedule["level"] = schedLevel;
  doc["time"]["tz"] = timeTz;
  doc["time"]["zone"] = timeZone;
  doc["addon"]["show"] = addonShow;
  doc["addon"]["when"] = WHEN_NAMES[addonWhen];
}

static const char *applyExtraConfig(JsonVariantConst doc) {
  JsonVariantConst schedule = doc["schedule"];
  if (!schedule["enabled"].isNull()) schedOn = schedule["enabled"].as<bool>();
  if (!schedule["from"].isNull() && !parseClock(schedule["from"].as<const char *>(), schedFrom)) return "Times are HH:MM, 24-hour";
  if (!schedule["to"].isNull() && !parseClock(schedule["to"].as<const char *>(), schedTo)) return "Times are HH:MM, 24-hour";
  if (!schedule["level"].isNull()) {
    int value;
    if (!intIn(schedule["level"], 0, 100, value)) return "Brightness is 0-100";
    schedLevel = value;
  }
  if (!doc["time"]["tz"].isNull()) {
    const char *tz = doc["time"]["tz"].as<const char *>();
    if (!tz || !*tz || strlen(tz) >= sizeof timeTz) return "The time zone rule is 1-47 characters";
    const bool changed = strcmp(timeTz, tz) != 0;   // the page sends the zone along with every change to the schedule
    strlcpy(timeTz, tz, sizeof timeTz);
    const char *zone = doc["time"]["zone"].as<const char *>();
    strlcpy(timeZone, zone && *zone ? zone : tz, sizeof timeZone);
    applyTimeZone();
    if (changed) faZoneChanged();
  }
  if (!doc["addon"]["show"].isNull()) {
    const char *id = doc["addon"]["show"].as<const char *>();
    if (!id || strlen(id) >= sizeof addonShow) return "Unknown add-on";
    strlcpy(addonShow, id, sizeof addonShow);
  }
  if (!doc["addon"]["when"].isNull()) {
    int at = findName(WHEN_NAMES, WHEN_COUNT, doc["addon"]["when"].as<const char *>());
    if (at < 0) return "When is idle or always";
    addonWhen = at;
  }
  return nullptr;
}

/* ------------------------------------------------------------ LEDs -- */

enum Source : uint8_t { SRC_NONE, SRC_PREVIEW, SRC_CONTROLLER, SRC_PRINTER };
static const char *SRC_NAMES[] = {"none", "preview", "controller", "printer"};

/* A state switched off on the page gives way to the next one that is on: cooling off goes straight to finished (or ready), preparing off shows printing. A look being tried is always shown as it is. */
static State stateSwitchedOn(State state) {
  for (int hops = 0; hops < ST_COUNT && state < ST_OFF && !looks[state].on; hops++) state = (State)ST_INSTEAD[state];
  return state < ST_OFF && !looks[state].on ? ST_OFF : state;
}

/* What to show, how full "fill" is, and who decided. A controller that is speaking wins over the bar's own printer link: it is assumed to know more. */
static State shownAsTold(float &fill, Source &source) {
  if (previewUntil && millis() < previewUntil) { fill = previewProgress; source = SRC_PREVIEW; return previewState; }
  previewUntil = 0;
  State state;
  if (everPushed && millis() - lastPushAt <= LINK_TIMEOUT_MS) { fill = progress; source = SRC_CONTROLLER; state = current; }
  else if (printerFresh()) { source = SRC_PRINTER; state = printerState(fill); }
  else { fill = 0; source = SRC_NONE; state = ST_OFFLINE; }
  return stateSwitchedOn(state);
}

/* The state now told and since when. Kept by the loop task alone (trackState); shown() is also asked from Home Assistant's task and only reads them. */
static volatile uint8_t stateNow = ST_COUNT;
static volatile uint32_t stateSince = 0;

/* Print time, counted whenever the bar is told a print is running, and kept across restarts (saved every 10 minutes of it). */
static volatile uint32_t printSeconds = 0;
static volatile uint32_t maintFrom = 0;          // printSeconds when maintenance was last marked done
static uint32_t printSecondsSaved = 0;

static void trackState() {
  float fill;
  Source source;
  const State state = shownAsTold(fill, source);
  if (source == SRC_PREVIEW) return;   // a look being tried says nothing about the printer
  /* Counted from what the printer says too, so switching the printing look off does not stop the maintenance count. */
  if (state == ST_PRINTING || state == ST_PREPARING || (source == SRC_PRINTER && gcodeIs("RUNNING"))) {
    printSeconds = printSeconds + 1;
    if (printSeconds - printSecondsSaved >= 600) { prefs.putUInt("printsec", printSeconds); printSecondsSaved = printSeconds; }
  }
  if (state != stateNow) { stateSince = 0; stateNow = state; }
  if (!stateSince) stateSince = millis() | 1;
}

/* Whether the state now told has been shown for longer than its look's limit. */
static bool stateOverLimit(State state) {
  const uint16_t limit = looks[state].limit;
  const uint32_t since = stateSince;
  return limit && stateNow == state && since && millis() - since > limit * 60000UL;
}

/* The same, except that a state shown for longer than its limit gives way: idle and offline go dark, the rest to the idle look (and an add-on, if one shows then) until the state changes. A failed print stays "failed" on the printer until the next one starts, which kept the error flashing for hours. A look being tried is always shown as it is. */
static State shown(float &fill, Source &source) {
  const State state = shownAsTold(fill, source);
  if (source == SRC_PREVIEW || state >= ST_OFF || !stateOverLimit(state)) return state;
  fill = 0;
  return state == ST_IDLE || state == ST_OFFLINE ? ST_OFF : stateSwitchedOn(ST_IDLE);
}

static void hsv(float h, uint8_t &r, uint8_t &g, uint8_t &b) {
  float x = h * 6.0f;
  int sector = (int)x % 6;
  float f = x - (int)x;
  float q = 1 - f;
  float rr = 0, gg = 0, bb = 0;
  switch (sector) {
    case 0: rr = 1; gg = f; break;
    case 1: rr = q; gg = 1; break;
    case 2: gg = 1; bb = f; break;
    case 3: gg = q; bb = 1; break;
    case 4: rr = f; bb = 1; break;
    default: rr = 1; bb = q; break;
  }
  r = rr * 255; g = gg * 255; b = bb * 255;
}

/* ------------------------------------------------- the lamp effects -- */

/* One colour per LED (0-255 floats) and how bright each is (0-1), for the lamp effects. Drawn once a frame, before the LEDs are filled in, because several of them keep memory between frames. */
static float fxColour[LED_COUNT][3];
static float fxLevel[LED_COUNT];

static float rnd() { return (esp_random() & 0xFFFFFF) / 16777216.0f; }

static void mix(const float a[3], const float b[3], float k, float out[3]) {
  for (int c = 0; c < 3; c++) out[c] = a[c] + (b[c] - a[c]) * k;
}

/* Smoothly round the two colours and back: x in any range, 1 = one trip. */
static void along(const float pal[2][3], float x, float out[3]) {
  const float f = fmodf(fmodf(x, 1) + 1, 1) * 2;
  const int i = (int)f;
  mix(pal[i % 2], pal[(i + 1) % 2], f - i, out);
}

/* x reduced to [0, period), worked in double and handed back as a float. The lamp effects' phases are taken this way so they stay exact however long the bar has been on, and do not jump when a shorter clock starts again (they did, every 2 h 47 min, 0.7.12). */
static float cyc(double x, double period) { return (float)fmod(x, period); }

static void paintEffect(uint8_t effect, uint32_t colour, uint32_t colour2, uint8_t speed, double t, float dt) {
  const float pal[2][3] = {
    {(float)((colour >> 16) & 255), (float)((colour >> 8) & 255), (float)(colour & 255)},
    {(float)((colour2 >> 16) & 255), (float)((colour2 >> 8) & 255), (float)(colour2 & 255)},
  };
  static const float WHITE[3] = {255, 255, 255};
  /* The lamp effects run at a speed of about 1-8 (4 is middling); the page's 1-100 maps onto that. */
  const float s = 0.5f + speed / 100.0f * 7.5f;
  const int N = LED_COUNT;
  static float heat[LED_COUNT], glow[LED_COUNT], tint[LED_COUNT][3];
  static float drops[8];
  static int dropCount = 0;
  /* Each effect's phases, once per frame: the work in double is ten calls, not ten for every LED. */
  const double TAU = 6.283185307179586, sd = s;
  const float waveDrift = cyc(t * 0.05 * sd, 1), waveBend = cyc(t * 0.6 * sd, TAU), waveLevel = cyc(t * 0.9 * sd, TAU);
  const float scroll = cyc(t * 0.07 * sd, 1), scanner = cyc(t * sd * 1.5, TAU), meteor = cyc(t * sd * 2.6, N + 14);
  const int theatre = (int)cyc(t * sd * 3, 3);
  const float aurora1 = cyc(t * sd * 0.35, TAU), aurora2 = cyc(t * sd * 0.25, TAU), auroraDrift = cyc(t * 0.02 * sd, 1);
  const float heart = cyc(t * sd * 0.25, 1), strobe = cyc(t * sd * 0.6, 1), ripple = cyc(t * sd * 1.6, TAU), stack = cyc(t * sd * 0.12, 2);
  static float flick[LED_COUNT];
  static float flickAll = 1;

  for (int i = 0; i < N; i++) {
    float *c = fxColour[i];
    float &k = fxLevel[i];
    k = 1;
    switch (effect) {
      case FX_GRADIENT: mix(pal[0], pal[1], (float)i / (N - 1), c); break;
      case FX_WAVES:
        along(pal, (float)i / N + waveDrift + 0.12f * sinf(waveBend + i / 3.0f), c);
        k = 0.55f + 0.45f * sinf(i / 2.5f - waveLevel);
        break;
      case FX_SCROLL: along(pal, (float)i / N + scroll, c); break;
      case FX_THEATRE: {
        const bool on = (i + theatre) % 3 == 0;
        memcpy(c, on ? pal[0] : pal[1], sizeof(float) * 3);
        k = on ? 1 : 0.25f;
        break;
      }
      case FX_SCANNER: {
        const float pos = (N - 1) * (0.5f - 0.5f * cosf(scanner));
        const float m = constrain(1 - fabsf(i - pos) / 3, 0.0f, 1.0f);
        mix(pal[1], pal[0], m, c);
        k = 0.25f + 0.75f * m;
        break;
      }
      case FX_METEOR: {
        /* A soft wide head and a long tail behind it; placed between LEDs so it glides rather than hops (a hop between LEDs reads as flicker). */
        const float pos = meteor;
        const float behind = pos - i;
        const float glowAt = behind < 0 ? expf(-(behind * behind) / 2.2f) : expf(-behind / 4.5f);
        along(pal, 1 - (float)i / N * 0.6f, c);
        k = sqrtf(constrain(glowAt, 0.0f, 1.0f));   // the bar squares levels later; this keeps the lamp's fall-off
        break;
      }
      case FX_AURORA: {
        const float v = constrain(0.5f + 0.5f * sinf(i * 0.35f + aurora1) * sinf(i * 0.17f - aurora2 + 1.7f) + 0.15f, 0.0f, 1.0f);
        along(pal, 0.5f * v + (float)i / N * 0.3f + auroraDrift, c);
        k = 0.5f + 0.5f * v;
        break;
      }
      case FX_HEARTBEAT: {
        /* Two beats close together, then a rest: about one a second at the middle speed. */
        auto beat = [](float x, float at) { const float d = (x - at) / 0.045f; return expf(-d * d); };
        const float b = max(beat(heart, 0.08f), 0.7f * beat(heart, 0.28f));
        mix(pal[1], pal[0], b, c);
        k = sqrtf(0.04f + 0.96f * b);
        break;
      }
      case FX_STROBE: {
        const bool on = strobe < 0.12f;
        memcpy(c, on ? pal[0] : pal[1], sizeof(float) * 3);
        k = on ? 1 : 0;
        break;
      }
      case FX_RIPPLE: {
        /* Rings running out from the middle of the bar. */
        const float w = 0.5f + 0.5f * sinf(fabsf(i - (N - 1) / 2.0f) * 0.9f - ripple);
        mix(pal[1], pal[0], w, c);
        k = 0.3f + 0.7f * w;
        break;
      }
      case FX_STACK: {
        /* The bar fills one LED after another in the first colour, then empties back into the second. */
        const float edge = stack < 1 ? stack * N : (2 - stack) * N;
        const float on = constrain(edge - i, 0.0f, 1.0f);
        mix(pal[1], pal[0], on, c);
        k = 0.35f + 0.65f * on;
        break;
      }
      default: break;   // the effects below are drawn for the whole bar at once
    }
  }

  if (effect == FX_FIRE) {
    /* Heat rises from the start of the bar, cools as it goes, and is fed at random near the base. Its own colours, like rainbow: fire is not fire in blue. */
    const int ticks = max(1, (int)roundf(dt * 30 * max(0.5f, s / 4)));
    for (int n = 0; n < ticks; n++) {
      for (int i = 0; i < N; i++) heat[i] = max(0.0f, heat[i] - rnd() * ((55.0f * 10 / N + 2) / 255));
      for (int i = N - 1; i >= 2; i--) heat[i] = (heat[i - 1] + heat[i - 2] + heat[i - 2]) / 3;
      if (rnd() < 0.65f) { const int at = (int)(rnd() * 4); heat[at] = min(1.0f, heat[at] + 0.6f + rnd() * 0.4f); }
    }
    for (int i = 0; i < N; i++) {
      const float x = constrain(heat[i], 0.0f, 1.0f) * 3;
      fxColour[i][0] = x < 1 ? x * 255 : 255;
      fxColour[i][1] = x < 1 ? 0 : x < 2 ? (x - 1) * 255 : 255;
      fxColour[i][2] = x < 2 ? 0 : (x - 2) * 255;
      fxLevel[i] = 1;
    }
  } else if (effect == FX_CANDLE) {
    /* A warm flicker: the whole flame wavers, and each LED a little on its own. Between the two colours, the first at the brightest. */
    flickAll = constrain(flickAll + (rnd() - 0.5f) * dt * s * 2.2f, 0.55f, 1.0f);
    flickAll += (0.85f - flickAll) * min(1.0f, dt * 1.5f);
    for (int i = 0; i < N; i++) {
      if (flick[i] <= 0) flick[i] = 0.8f;
      flick[i] = constrain(flick[i] + (rnd() - 0.5f) * dt * s * 1.6f, 0.5f, 1.0f);
      const float v = constrain(flickAll * flick[i], 0.0f, 1.0f);
      mix(pal[1], pal[0], v, fxColour[i]);
      fxLevel[i] = v;
    }
  } else if (effect == FX_TWINKLE) {
    for (int i = 0; i < N; i++) {
      glow[i] = max(0.0f, glow[i] - dt * 0.9f * s / 4);
      if (rnd() < dt * s * 0.5f) { glow[i] = 1; along(pal, rnd(), tint[i]); }
      memcpy(fxColour[i], tint[i], sizeof(float) * 3);
      fxLevel[i] = glow[i];
    }
  } else if (effect == FX_SPARKLE) {
    for (int i = 0; i < N; i++) {
      glow[i] = max(0.0f, glow[i] - dt * 6);
      if (rnd() < dt * s * 0.8f) glow[i] = 1;
      mix(pal[0], WHITE, glow[i], fxColour[i]);
      fxLevel[i] = 0.55f + 0.45f * glow[i];
    }
  } else if (effect == FX_RAIN) {
    /* Drops run from the far end of the bar back to the start, each with a short fading tail. */
    for (int d = dropCount - 1; d >= 0; d--) {
      drops[d] -= dt * s * 5;
      if (drops[d] < -4) drops[d] = drops[--dropCount];
    }
    if (dropCount < 8 && rnd() < dt * s * 0.9f) drops[dropCount++] = N - 1;
    for (int i = 0; i < N; i++) { memcpy(fxColour[i], pal[0], sizeof(float) * 3); fxLevel[i] = 0; }
    for (int d = 0; d < dropCount; d++)
      for (int j = 0; j < 4; j++) {
        const int i = (int)roundf(drops[d] + j);
        if (i >= 0 && i < N) fxLevel[i] = max(fxLevel[i], 1 - j / 4.0f);
      }
  }
}

/* 0-100 while new firmware is being written, -1 otherwise. While it is set the bar shows nothing but the progress: blue filling from LED 0, with a soft highlight running through the lit part so a slow download still looks alive. Never fully dark, even inside the schedule's dark spell: an update is exactly when the owner wants to see the bar. */
static volatile int8_t barProgress = -1;
static const uint32_t PROGRESS_COLOUR = 0x3B82F6;

/* Home Assistant can switch the light off, which darkens everything except an update's progress. Defined with the rest of Home Assistant, below. */
static volatile bool haOff = false;
static volatile bool haManual = false;
static Look haLook(0xFFFFFF, FX_SOLID, 50);

/* The one place the 25-LED picture in frame[] reaches the hardware, so every board shares every effect. */
static void wireOrder(uint8_t r, uint8_t g, uint8_t b, uint8_t wire[3]) {
  for (int c = 0; c < 3; c++) wire[c] = order[c] == 'R' ? r : order[c] == 'G' ? g : b;
}

#if defined(BOARD_RGB)
static void outBegin() {
  for (int s = 0; s < 2; s++) {
    pinMode(RGB_ENABLE_PINS[s], OUTPUT);
    digitalWrite(RGB_ENABLE_PINS[s], HIGH);
    for (int c = 0; c < 3; c++) {
      /* 5 kHz, 8 bits: above anything a camera or an eye picks up as flicker, and 8 bits is all the picture carries. */
      ledcSetup(s * 3 + c, 5000, 8);
      ledcAttachPin(RGB_PINS[s][c], s * 3 + c);
      ledcWrite(s * 3 + c, 0);
    }
  }
}
static void outShow() {
  for (int s = 0; s < 2; s++) {
    /* An average, so a half-full progress bar is half as bright rather than a guess at one LED. */
    int from = 0, to = LED_COUNT;
    if (rgbSplit) { from = s == 0 ? 0 : LED_COUNT / 2; to = s == 0 ? (LED_COUNT + 1) / 2 : LED_COUNT; }
    if (reversed && rgbSplit) { const int f = LED_COUNT - to, t = LED_COUNT - from; from = f; to = t; }
    uint32_t sum[3] = {0, 0, 0};
    for (int i = from; i < to; i++) for (int c = 0; c < 3; c++) sum[c] += frame[i][c];
    const int n = max(1, to - from);
    uint8_t wire[3];
    wireOrder(sum[0] / n, sum[1] / n, sum[2] / n, wire);
    for (int c = 0; c < 3; c++) ledcWrite(s * 3 + c, wire[c]);
  }
}
#else
static void outBegin() {
#if defined(BOARD_STATUS)
  strip.updateLength(stripLength);
#endif
  strip.begin();
  strip.clear();
  strip.show();
}
static void outShow() {
#if defined(BOARD_STATUS)
  if (strip.numPixels() != stripLength) { strip.clear(); strip.show(); strip.updateLength(stripLength); }
  const int n = strip.numPixels();
#else
  const int n = LED_COUNT;
#endif
  for (int j = 0; j < n; j++) {
    /* Nearest of the 25: a longer strip shows the same picture, stretched. */
    const int i = n == LED_COUNT ? j : min(LED_COUNT - 1, j * LED_COUNT / n);
    uint8_t wire[3];
    wireOrder(frame[i][0], frame[i][1], frame[i][2], wire);
    strip.setPixelColor(reversed ? n - 1 - j : j, wire[0], wire[1], wire[2]);
  }
  strip.show();
}
#endif

static void renderProgress() {
  const float lit = barProgress / 100.0f * LED_COUNT;
  const float level = max((schedActive() ? schedLevel : brightness) / 100.0f, 0.5f);
  const float head = (millis() % ((LED_COUNT + 6) * 100UL)) / 100.0f - 3.0f;   // ten LEDs a second; the remainder is taken in whole milliseconds, which a float of the uptime cannot hold after a few days
  for (int i = 0; i < LED_COUNT; i++) {
    float v = lit - i >= 1 ? 0.75f : lit - i > 0 ? max(lit - i, 0.2f) * 0.75f : 0.12f;
    if (i < lit) v += 0.25f * max(0.0f, 1 - fabsf(i - head) / 3.0f);
    const float scale = v * v * level;
    const uint8_t r = ((PROGRESS_COLOUR >> 16) & 255) * scale, g = ((PROGRESS_COLOUR >> 8) & 255) * scale, b = (PROGRESS_COLOUR & 255) * scale;
    frame[i][0] = r; frame[i][1] = g; frame[i][2] = b;
  }
  outShow();
}

static void render() {
  if (barProgress >= 0) return renderProgress();
  float fill;
  Source source;
  const State state = shown(fill, source);
  /* Music mode takes the bar over, except while a look is being tried from the page. */
  /* A colour or effect chosen in Home Assistant holds the bar until it is handed back ("Follow the printer"); only a look being tried from the page goes over it. */
  const bool manual = haManual && source != SRC_PREVIEW;
  const bool music = !manual && mode == MODE_MUSIC && source != SRC_PREVIEW;
  const bool notice = noticeUntil && millis() < noticeUntil;
  if (!notice) noticeUntil = 0;
  /* An add-on gets the bar only in the mode that follows the printer, and never over a look being tried. */
  const Addon *addon = manual || music || source == SRC_PREVIEW ? nullptr : addonNow(state);
  /* An add-on that sends a "look" is drawn by the same effects as the bar's own states. */
  const Look look = manual ? haLook : addon && addon->kind == ADDON_LOOK ? addon->look : looks[state];
  if (addon && addon->kind == ADDON_LOOK) fill = addon->fill;
  if (manual) fill = 100;
  const bool dark = state == ST_OFF && !addon && !manual;
  const float fillTo = fill / 100.0f * LED_COUNT;
  /* Seconds, starting again every 10,000 of them. A float of the whole uptime runs out of digits: the effects went jerky after a couple of weeks and the error flash stopped after 39 days (0.7.11). At 10,000 s every speed on the page has done a whole number of breathe, flash, chase and rainbow cycles, so nothing jumps there. */
  const float t = (millis() % 10000000UL) / 1000.0f;
  /* 1-100 on the page is 0.1 to 2 cycles a second. */
  const float rate = 0.1f + look.speed / 100.0f * 1.9f;
  /* The dark spell's level wins; otherwise the state's own brightness, where it has one. Only the bar's own looks have one: a look from Home Assistant or an add-on keeps the bar's. */
  const uint8_t ownLevel = manual || (addon && addon->kind == ADDON_LOOK) ? 0 : looks[state].level;
  const float level = (schedActive() ? schedLevel : ownLevel ? ownLevel : brightness) / 100.0f;
  const uint8_t baseR = (look.colour >> 16) & 255, baseG = (look.colour >> 8) & 255, baseB = look.colour & 255;
  /* Meters share the bar equally, with one dark LED between neighbours so two full meters do not read as one. */
  const int meters = addon && addon->kind == ADDON_METERS ? addon->meterCount : 0;
  const float meterSpan = meters ? (LED_COUNT - (meters - 1)) / (float)meters : 0;
  static uint32_t lastFrameMs = 0;
  const float dt = lastFrameMs ? min(0.1f, (millis() - lastFrameMs) / 1000.0f) : 0.025f;
  lastFrameMs = millis();
  /* What an unlit LED shows: 0.2 squared below is a 4% glow (0.05 came out as nothing at all on the real LEDs, 0.1.0), or fully off when the owner turned the glow off (0.7.17: "the LEDs that should be off sit at 1%"). */
  const float unlit = glow ? 0.2f : 0;
  const bool lampFx = !dark && look.effect >= FX_GRADIENT;
  if (lampFx) paintEffect(look.effect, look.colour, secondColour(look), look.speed, esp_timer_get_time() / 1e6, dt);   // microseconds since start in 64 bits: no wrap for centuries

  for (int i = 0; i < LED_COUNT; i++) {
    uint8_t r = baseR, g = baseG, b = baseB;
    float v = 1;
    if (notice) {
      r = g = b = 255;
      v = i < noticeQuarters * LED_COUNT / 4 ? 0.7f : 0;
    } else if (music) {
      meterColour((float)i / (LED_COUNT - 1), r, g, b);
      const float lit = musicLevel * LED_COUNT - i;
      v = lit >= 1 ? 1 : lit > 0 ? lit : 0;
    } else if (addon && addon->kind == ADDON_FRAME) {
      r = addon->pixels[i][0]; g = addon->pixels[i][1]; b = addon->pixels[i][2];
    } else if (meters) {
      const int which = min(meters - 1, (int)(i / (meterSpan + 1)));
      const float within = i - which * (meterSpan + 1);
      const uint32_t colour = addon->meterColour[which];
      r = (colour >> 16) & 255; g = (colour >> 8) & 255; b = colour & 255;
      if (within >= meterSpan) v = 0;   // the gap
      else {
        const float lit = addon->meterValue[which] / 100.0f * meterSpan - within;
        v = lit >= 1 ? 1 : lit > 0 ? max(lit, unlit) : unlit;
      }
    } else switch (dark ? FX_SOLID : look.effect) {
      case FX_BREATHE: v = (glow ? 0.10f : 0) + (glow ? 0.90f : 1) * (0.5f - 0.5f * cosf(2 * PI * t * rate * 0.5f)); break;
      case FX_FILL: {
        /* The LED the edge is in is lit in proportion, so progress moves smoothly rather than a whole LED at a time. The rest stay faintly lit so the bar never looks switched off mid-print. */
        float lit = fillTo - i;
        v = lit >= 1 ? 1 : lit > 0 ? max(lit, unlit) : unlit;
        break;
      }
      case FX_FLASH: v = fmodf(t * rate * 2, 1) < 0.5f ? 1 : (glow ? 0.15f : 0); break;
      case FX_CHASE: {
        float head = fmodf(t * rate * LED_COUNT * 0.5f, LED_COUNT);
        float behind = fmodf(head - i + LED_COUNT, LED_COUNT);
        v = behind < 6 ? 1 - behind / 6 : unlit;
        if (v < unlit) v = unlit;
        break;
      }
      case FX_RAINBOW: hsv(fmodf((float)i / LED_COUNT + t * rate * 0.2f, 1), r, g, b); break;
      default:
        if (lampFx) {
          r = constrain(fxColour[i][0], 0.0f, 255.0f); g = constrain(fxColour[i][1], 0.0f, 255.0f); b = constrain(fxColour[i][2], 0.0f, 255.0f);
          v = fxLevel[i];
        }
        break;
    }
    if ((dark && !music && !notice) || haOff) v = 0;
    /* Squared: LEDs are far from linear, and without it "breathe" spends most of its time looking fully on. */
    const float scale = v * v * level;
    r = r * scale; g = g * scale; b = b * scale;
    frame[i][0] = r; frame[i][1] = g; frame[i][2] = b;
  }
  outShow();
}

/* ----------------------------------------------------------- Wi-Fi -- */

static String wifiSsid, wifiPass;
static bool apUp = false;
static bool apByButton = false;       // asked for by hand, so it is not taken down again just because Wi-Fi is fine
static uint32_t connectedSince = 0;   // 0 while not connected
static uint32_t lastTryAt = 0;

/**
 * The Wi-Fi the bar was already on, read from stock's own settings.
 *
 * Stock keeps it in one 228-byte record (namespace app_nvs, key wifi_info):
 * network name at byte 0 (33 bytes), password at byte 33 (64 bytes). The
 * layout was worked out from where the fields sit in the backup, without the
 * values being looked at. Reading it here means nobody has to type the
 * password into anything for the first boot.
 */
static bool readStockWifi(String &ssid, String &pass) {
  nvs_handle_t handle;
  if (nvs_open("app_nvs", NVS_READONLY, &handle) != ESP_OK) return false;
  uint8_t blob[228];
  size_t length = sizeof blob;
  esp_err_t result = nvs_get_blob(handle, "wifi_info", blob, &length);
  nvs_close(handle);
  if (result != ESP_OK || length < 97) return false;
  blob[32] = 0;
  blob[96] = 0;
  ssid = String((const char *)blob);
  pass = String((const char *)blob + 33);
  return ssid.length() > 0;
}

static String apPassword() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char text[16];
  snprintf(text, sizeof text, "panda%02x%02x%02x", mac[3], mac[4], mac[5]);
  return String(text);
}

/**
 * The bar's own Wi-Fi network (access point), as stock had: the way in when
 * the home network is gone, renamed or mistyped. By default it comes up by
 * itself after a minute without Wi-Fi and goes away a minute after Wi-Fi is
 * back; it can be kept on all the time instead. It cannot be switched off for
 * good: a bar with no Wi-Fi and no hotspot could only be reached over USB.
 */
static String apSsid, apPass;
static bool apAlways = false;

static void loadAp() {
  apSsid = prefs.getString("apssid", AP_DEFAULT_SSID);
  apPass = prefs.getString("appass", "");
  if (apPass.length() < 8) apPass = apPassword();
  apAlways = prefs.getBool("apalways", false);
}

static void startAp() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  apUp = true;
}

static void stopAp() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apUp = false;
}

static void startWifi() {
  WiFi.persistent(false);   // our settings live in our own namespace, not the driver's
  WiFi.setHostname(STOCK_HOST);   // the name stock used, so the router sees the same device. Before mode(): the name is only taken up when the mode changes
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  loadAp();
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  if (!wifiSsid.length()) readStockWifi(wifiSsid, wifiPass);
  if (wifiSsid.length()) WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  lastTryAt = millis();
}

/* ------------------------------------------------- staying recoverable -- */

static bool onTrial = false;
static const uint32_t TRIAL_GIVE_UP_MS = 120000;
static const uint32_t TRIAL_SETUP_MS = 600000;   // with no network to join there is nothing to prove until someone gives it one: the setup network is offered for this long first
static const uint32_t TRIAL_PROVEN_MS = 20000;
static uint32_t trialFrom = 0;                    // the wait for Wi-Fi is counted from here: the start, or the moment new Wi-Fi details were given
static uint32_t trialAllow = TRIAL_GIVE_UP_MS;    // and lasts this long
static const char TRIAL_NO_UPDATE[] = "This version is still on trial, and the other slot is its way back, so nothing is written there yet. Try again once it has been on Wi-Fi for 20 seconds.";
static const uint32_t AP_AFTER_MS = 60000;

static bool otherSlot(const esp_partition_t *&part, esp_app_desc_t &desc) {
  part = esp_ota_get_next_update_partition(nullptr);
  return part && esp_ota_get_partition_description(part, &desc) == ESP_OK;
}

/* The board and build an image was made for, from its tag; both empty for an image without one (stock, or Panda State before 0.7.14). The tag sits a few hundred bytes in, so the first part of an image is enough. */
static void imageTag(const uint8_t *data, size_t length, char *board, size_t boardSize, char *build, size_t buildSize) {
  board[0] = build[0] = 0;
  const size_t look = min(length, (size_t)640);
  for (size_t at = 0; at + BOARD_TAG_PREFIX < look; at++) {
    if (memcmp(data + at, PANDA_BOARD_TAG, BOARD_TAG_PREFIX)) continue;
    size_t i = 0, j = at + BOARD_TAG_PREFIX;
    for (; j < look && data[j] != ';'; j++) {
      if ((!islower(data[j]) && !isdigit(data[j])) || i + 1 >= boardSize) { board[0] = 0; return; }   // not a tag after all
      board[i++] = (char)data[j];
    }
    if (j >= look || !i) { board[0] = 0; return; }   // no ";": not a tag after all
    board[i] = 0;
    for (i = 0, j++; j < look && data[j] && data[j] >= ' ' && i + 1 < buildSize; j++) build[i++] = (char)data[j];
    build[i] = 0;
    return;
  }
}

/* The other slot's tag, read straight from the flash. */
static void otherSlotTag(const esp_partition_t *part, char *board, size_t boardSize, char *build, size_t buildSize) {
  static uint8_t head[640];
  board[0] = build[0] = 0;
  if (part && esp_partition_read(part, 0, head, sizeof head) == ESP_OK) imageTag(head, sizeof head, board, boardSize, build, buildSize);
}

/* Hand the bar to the firmware in the other slot. Returns only if there is nothing bootable there. */
static void bootOtherSlot() {
  const esp_partition_t *part;
  esp_app_desc_t desc;
  if (!otherSlot(part, desc)) return;
  if (esp_ota_set_boot_partition(part) != ESP_OK) return;
  memset(frame, 0, sizeof frame);
  outShow();
  delay(200);
  ESP.restart();
}

/* The Arduino core would otherwise mark every new image good before setup() runs, which is exactly the decision this firmware wants to make itself. */
extern "C" bool verifyRollbackLater() { return true; }

/* Whether the boot record already has this image down as good. It is written there when a build proves itself, and a newly installed or switched-to image never starts with it, so it says the same as the "okbuild" note without needing room in the settings store: with that store full the note was never written, and every start was a trial (0.7.12). */
/* The boot record speaks for this image only while it is the one the record selects. If the bootloader ever starts another slot because the selected image would not load, the record's good mark and its "mark as good" both belong to that other image. */
static bool runningIsBootSlot() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  return running && boot && running->address == boot->address;
}

static bool imageMarkedGood() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  return runningIsBootSlot() && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_VALID;
}

/* The trial is failed: hand the bar to the other slot. Returns only if there is nothing there to go back to (flashed by USB), and then this build carries on as it is. */
static void failTrial() {
  logLine("Trial failed: handing the bar back to the other slot");
  /* The bootloader's own rollback first, if stock's bootloader has it; our own switch of slots if it does not. */
  esp_ota_mark_app_invalid_rollback_and_reboot();
  bootOtherSlot();
  onTrial = false;
}

static void watchTrial() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  /* When the network was last lost (or boot, if it has never been up). Kept apart from lastTryAt, which moves on every retry. */
  static uint32_t lostAt = 0;
  if (connected && !connectedSince) connectedSince = millis();
  if (!connected && (connectedSince || !lostAt)) lostAt = millis();
  if (!connected) connectedSince = 0;

  if (onTrial) {
    if (connectedSince && millis() - connectedSince > TRIAL_PROVEN_MS) {
      if (runningIsBootSlot()) esp_ota_mark_app_valid_cancel_rollback();
      prefs.putString("okbuild", BUILD_ID);
      if (prefs.isKey("micblame")) prefs.remove("micblame");
      onTrial = false;
      logLine("Trial passed: this version is kept");
    } else if (!connectedSince && millis() - trialFrom > trialAllow) {
      failTrial();   // if it comes back there is nothing to go back to: carry on and offer the setup network
    } else if (!apUp && !wifiSsid.length()) {
      /* No network name from this firmware or from stock, so there is nothing to join and no other way to be given one: the setup network comes up now rather than after the trial has already been lost (0.7.12). */
      startAp();
    }
    /* The same retry a proven build makes, below. Wi-Fi details given through the setup network are joined by one call, and that call can be refused while the radio is busy: without this they were never tried again before the trial ran out. */
    if (onTrial && !connected && wifiSsid.length() && millis() - lastTryAt > 30000) {
      lastTryAt = millis();
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
    return;
  }

  /* A proven build that has lost the network keeps trying, and after a while also offers its own network so the Wi-Fi details can be changed. */
  /* With no network name saved at all (a bar flashed from new) there is nothing to wait for: the setup network comes up at once. */
  if (!apUp && (apAlways || (!connected && (millis() - lostAt > AP_AFTER_MS || !wifiSsid.length())))) startAp();
  if (connected && apUp && !apByButton && !apAlways && millis() - connectedSince > 60000) stopAp();
  if (!connected && wifiSsid.length() && millis() - lastTryAt > 30000) {
    lastTryAt = millis();
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
}

/* ---------------------------------------------------------- button -- */

/**
 * The button on the bar.
 *
 * What stock does with it was read from its code only as far as this: one
 * kind of press steps a four-value setting that Music mode reads (taken here
 * to be the sensitivity), a second kind toggles something not identified, and
 * a hold of five seconds raises a system event, also not identified. So the
 * single press matches stock as far as it is known, and the other two are
 * choices made here:
 *   press once   - next Music sensitivity (low, medium, high, auto)
 *   press twice  - switch between Music and following the printer
 *   hold 5 s     - bring up the setup Wi-Fi network
 * Each is answered on the LEDs for a moment so the press is seen to land.
 */
static void buttonAction(uint8_t kind) {
#if !defined(BOARD_P2)
  /* Without the P2's microphone, presses have nothing to do; only the hold for the hotspot means anything. */
  if (kind != 3) return;
#endif
  if (kind == 1) {
    sensitivity = (sensitivity + 1) % SENS_COUNT;
    saveConfig();
    noticeQuarters = sensitivity + 1;
    noticeUntil = millis() + 700;
  } else if (kind == 2) {
    mode = mode == MODE_MUSIC ? MODE_STATUS : MODE_MUSIC;
    saveConfig();
    noticeQuarters = mode == MODE_MUSIC ? 4 : 2;
    noticeUntil = millis() + 400;
  } else if (kind == 3 && !apUp) {
    startAp();
    apByButton = true;
    noticeQuarters = 4;
    noticeUntil = millis() + 1500;
  }
}

static bool buttonDown = false;

static void pollButton() {
  static uint32_t changedAt = 0, pressedAt = 0, releasedAt = 0;
  static bool raw = false, held = false;
  static uint8_t clicks = 0;
  const bool now = digitalRead(PIN_BUTTON) == LOW;
  if (now != raw) { raw = now; changedAt = millis(); }
  /* 30 ms of agreement before a change counts: the contacts bounce. */
  if (millis() - changedAt > 30 && raw != buttonDown) {
    buttonDown = raw;
    if (buttonDown) { pressedAt = millis(); held = false; }
    else { if (!held) { clicks++; releasedAt = millis(); } }
  }
  if (buttonDown && !held && millis() - pressedAt > 5000) { held = true; clicks = 0; buttonAction(3); }
  /* A press is not acted on until it is clear a second one is not following it. */
  if (!buttonDown && clicks && millis() - releasedAt > 400) { buttonAction(clicks >= 2 ? 2 : 1); clicks = 0; }
}

static void onlineToJson(JsonDocument &doc);   // in the online section, below
static void haToJson(JsonDocument &doc);       // in the Home Assistant section, below

/* ------------------------------------------------------------- API -- */

static void sendJson(int code, JsonDocument &doc) {
  String text;
  serializeJson(doc, text);
  server.send(code, "application/json", text);
}

static void sendResult(bool ok, const char *message = nullptr) {
  JsonDocument doc;
  doc["ok"] = ok;
  if (message) doc["message"] = message;
  sendJson(ok ? 200 : 400, doc);
}

static bool readBody(JsonDocument &doc) {
  /* The largest the page sends is an add-on file with its address, at most 12,000 characters. The web server has already taken the whole body by now, so this only keeps a huge one from being parsed as well, but that is most of the memory it would cost. */
  if (!server.hasArg("plain")) { sendResult(false, "Body must be JSON"); return false; }
  const String body = server.arg("plain");   // a copy either way: taken once
  if (body.length() > 16384) {
    sendResult(false, "That is far bigger than anything the bar takes");
    return false;
  }
  if (deserializeJson(doc, body)) {
    sendResult(false, "Body must be JSON");
    return false;
  }
  return true;
}

static void handleStatus() {
  JsonDocument doc;
  doc["firmware"] = FW_NAME;
  doc["version"] = FW_VERSION;
  doc["build"] = BUILD_ID;
  doc["uptime"] = millis() / 1000;
  /* Why the bar last started: a crash or a watchdog says something is wrong, a restart or power-on does not. */
  {
    const esp_reset_reason_t why = esp_reset_reason();
    doc["resetReason"] = why == ESP_RST_POWERON ? "power on" : why == ESP_RST_SW ? "restart" : why == ESP_RST_PANIC ? "crash"
      : (why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT) ? "watchdog" : why == ESP_RST_BROWNOUT ? "brownout" : "other";
  }
  /* The settings store, measured: it is 12 KB shared with stock in the other slot, and a save that does not fit fails without a sound. */
  {
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
      doc["nvs"]["used"] = st.used_entries;
      doc["nvs"]["free"] = st.free_entries;
      doc["nvs"]["total"] = st.total_entries;
    }
    doc["nvs"]["lastSaveOk"] = cfgSaveOk;
  }
  doc["heap"] = ESP.getFreeHeap();
  /* The largest single piece: a TLS session needs one big block, so this, not the total, says whether HTTPS will start. */
  doc["heapBlock"] = ESP.getMaxAllocHeap();
  doc["ip"] = WiFi.localIP().toString();
  doc["hostname"] = hostName;
  doc["hostnameDefault"] = STOCK_HOST;
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["setupNetwork"] = apUp;
  doc["wifiName"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : wifiSsid;
  doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  /* The hotspot. Its password is sent only while it is still the printed default (which anyone can work out from the MAC address on a label); one the owner chose is never sent back. */
  JsonObject ap = doc["ap"].to<JsonObject>();
  ap["up"] = apUp;
  ap["always"] = apAlways;
  ap["ssid"] = apSsid;
  ap["ip"] = WiFi.softAPIP().toString();
  ap["clients"] = apUp ? WiFi.softAPgetStationNum() : 0;
  if (apPass == apPassword()) ap["defaultPassword"] = apPass;
  else ap["defaultPassword"] = nullptr;
  doc["onTrial"] = onTrial;
  doc["leds"] = LED_COUNT;
  doc["board"] = BOARD_ID;
  doc["boardName"] = BOARD_NAME;

  float fill;
  Source source;
  const State state = shown(fill, source);
  doc["state"] = ST_NAMES[state];
  doc["source"] = haManual && source != SRC_PREVIEW ? "homeassistant" : SRC_NAMES[source];
  doc["lightOff"] = (bool)haOff;
  doc["said"] = ST_NAMES[current];
  doc["progress"] = fill;
  doc["previewing"] = source == SRC_PREVIEW;
  /* Seconds since a controller last spoke; null if none has since boot. */
  if (everPushed) doc["linkAge"] = (millis() - lastPushAt) / 1000;
  else doc["linkAge"] = nullptr;

  doc["mode"] = MODE_NAMES[mode];

  /* The clock and the dark spell. */
  JsonObject clk = doc["clock"].to<JsonObject>();
  clk["known"] = timeKnown();
  if (timeKnown()) clk["now"] = clockText(minutesNow());
  clk["dark"] = schedActive();

  /* Add-ons that have posted, and which one (if any) has the bar at this moment. */
  JsonArray list = doc["addons"].to<JsonArray>();
  for (int i = 0; i < ADDON_SLOTS; i++) {
    if (!addons[i].used) continue;
    JsonObject one = list.add<JsonObject>();
    one["id"] = addons[i].id;
    one["name"] = addons[i].name;
    one["kind"] = ADDON_KINDS[addons[i].kind];
    one["age"] = (millis() - addons[i].at) / 1000;
    one["fresh"] = addonFresh(addons[i]);
  }
  const Addon *onBar = mode == MODE_MUSIC || source == SRC_PREVIEW ? nullptr : addonNow(state);
  if (onBar) doc["addonShowing"] = onBar->id;
  else doc["addonShowing"] = nullptr;

  onlineToJson(doc);
  haToJson(doc);

  /* The microphone: whether the chip answered, how loud the last block was (dB below full scale), and what the meter makes of it. */
  JsonObject mic = doc["mic"].to<JsonObject>();
  mic["found"] = codecFound;
  mic["running"] = audioRunning;
  mic["blocked"] = audioBlocked;
  mic["db"] = (float)micDb;
  mic["level"] = (float)musicLevel;
  doc["buttonDown"] = buttonDown;

  /* The bar's own printer link. The access code is never sent back out. */
  JsonObject printer = doc["printer"].to<JsonObject>();
  xSemaphoreTake(prLock, portMAX_DELAY);
  printer["paired"] = prIp.length() > 0 && (prKind == PR_KLIPPER || prSerial.length() > 0);
  printer["type"] = prKind == PR_KLIPPER ? "klipper" : "bambu";
  printer["ip"] = prIp;
  printer["serial"] = prSerial;
  printer["port"] = prPort;
  /* Whether a Moonraker key is set, never the key. */
  printer["hasKey"] = prKind == PR_KLIPPER && prCode.length() > 0;
  xSemaphoreGive(prLock);
  printer["connected"] = (bool)prConnected;
  printer["error"] = prError;
  if (printerFresh()) {
    float own;
    printer["gcode"] = prGcode;
    if (prPercent >= 0) printer["percent"] = (int)prPercent;
    if (!isnan(prBed)) printer["bed"] = (float)prBed;
    printer["state"] = ST_NAMES[printerState(own)];
  }
  /* The filament: what is loaded now, or (loaded false) the last one seen loaded. Null if the printer has never said. */
  {
    const int tray = printerFresh() ? filLoadedTray() : -1;
    const FilamentTray *now = tray >= 0 ? filFind(tray) : nullptr;
    const FilamentTray *show = now ? now : filLast.tray != -1 ? &filLast : nullptr;
    if (show) {
      JsonObject fil = printer["filament"].to<JsonObject>();
      fil["colour"] = colourText(show->colour);
      fil["type"] = show->type;
      fil["loaded"] = now != nullptr;
    } else printer["filament"] = nullptr;
    /* Every tray the printer has described, by Bambu Studio's global tray number. */
    JsonArray trays = printer["trays"].to<JsonArray>();
    for (int i = 0; i < filTrayCount; i++) {
      JsonObject one = trays.add<JsonObject>();
      one["tray"] = filTrays[i].tray;
      one["colour"] = colourText(filTrays[i].colour);
      one["type"] = filTrays[i].type;
    }
  }

  const esp_partition_t *running = esp_ota_get_running_partition();
  doc["slot"] = running ? running->label : "?";
  const esp_partition_t *other;
  esp_app_desc_t desc;
  JsonObject otherDoc = doc["otherSlot"].to<JsonObject>();
  if (otherSlot(other, desc)) {
    otherDoc["label"] = other->label;
    otherDoc["project"] = desc.project_name;
    otherDoc["version"] = desc.version;
    otherDoc["built"] = String(desc.date) + " " + desc.time;
    /* What the description cannot say: that it is Panda State, which version, for which board. */
    char board[16], build[48];
    otherSlotTag(other, board, sizeof board, build, sizeof build);
    if (board[0]) { otherDoc["board"] = board; otherDoc["panda"] = build; }
    // Whether it is this firmware again (an older Panda State) or something else, normally stock: the page offers the backup for the latter.
    const esp_app_desc_t *mine = esp_ota_get_app_description();
    otherDoc["ours"] = mine && strcmp(mine->project_name, desc.project_name) == 0;
  } else {
    otherDoc["label"] = other ? other->label : "?";
    otherDoc["project"] = nullptr;
  }

  String hex;
  hex.reserve(LED_COUNT * 6);
  char two[7];
  for (int i = 0; i < LED_COUNT; i++) {
    snprintf(two, sizeof two, "%02x%02x%02x", frame[i][0], frame[i][1], frame[i][2]);
    hex += two;
  }
  doc["frame"] = hex;

  JsonDocument config;
  configToJson(config);
  doc["config"] = config;
  JsonArray effects = doc["effects"].to<JsonArray>();
  for (int i = 0; i < FX_COUNT; i++) effects.add(FX_NAMES[i]);
  sendJson(200, doc);
}

/* What a controller sends every few seconds, over HTTP or MQTT: {"state":"printing","progress":42}. */
static const char *applyPushedState(JsonVariantConst doc) {
  int at = findName(ST_NAMES, ST_COUNT, doc["state"].as<const char *>());
  if (at < 0 || at == ST_OFFLINE) return "Unknown state";
  current = (State)at;
  if (!doc["progress"].isNull()) progress = constrain(doc["progress"].as<float>(), 0.0f, 100.0f);
  lastPushAt = millis();
  everPushed = true;
  return nullptr;
}

static void handleState() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  const char *error = applyPushedState(doc.as<JsonVariantConst>());
  sendResult(!error, error);
}

static void handleConfig() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  {
    String keys;
    for (JsonPairConst field : doc.as<JsonObjectConst>()) { if (keys.length()) keys += ", "; keys += field.key().c_str(); }
    logLine("Settings changed: %s", keys.c_str());
  }
  /* All or nothing: fields are applied one after another, and a bad one used to leave the good ones before it changed on the bar but unsaved. */
  JsonDocument before;
  configToJson(before);
  const char *error = applyConfig(doc.as<JsonVariantConst>());
  if (error) { applyConfig(before.as<JsonVariantConst>()); logLine("  refused: %s", error); return sendResult(false, error); }
  saveConfig();
  // Say so when the store refused it: the change works now but would be gone after a restart.
  if (!cfgSaveOk) { logLine("  not saved: the settings store is full"); return sendResult(false, "Changed, but the bar could not save it: it will be lost on a restart"); }
  sendResult(true);
}

/* Try a look for a few seconds without it being taken for the printer's state. */
static void handlePreview() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  int at = findName(ST_NAMES, ST_COUNT, doc["state"].as<const char *>());
  if (at < 0) return sendResult(false, "Unknown state");
  int seconds = doc["seconds"].isNull() ? 8 : doc["seconds"].as<int>();
  previewState = (State)at;
  previewProgress = doc["progress"].isNull() ? 60 : constrain(doc["progress"].as<float>(), 0.0f, 100.0f);
  previewUntil = seconds <= 0 ? 0 : millis() + (uint32_t)constrain(seconds, 1, 120) * 1000;
  sendResult(true);
}

/**
 * An add-on saying what it wants shown. See the add-ons note above for the
 * three shapes. {"id":"x","remove":true} takes one away at once.
 */
static const char *addonIdError(const char *id) {
  if (!id || !*id || strlen(id) > 15) return "id is 1-15 characters";
  for (const char *c = id; *c; c++) if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '-' || *c == '_')) return "id uses a-z, 0-9, - and _";
  return nullptr;
}

/* Posted add-ons arrive on the web server's task and file add-ons on the online task; both write the same four slots. */
static SemaphoreHandle_t addonLock;

static Addon *addonSlotFor(const char *id) {
  Addon *slot = addonById(id);
  /* A free place, or failing that one whose add-on has stopped posting. The add-on the owner picked is never pushed out by a stranger. */
  for (int i = 0; i < ADDON_SLOTS && !slot; i++) if (!addons[i].used) slot = &addons[i];
  for (int i = 0; i < ADDON_SLOTS && !slot; i++) if (!addonFresh(addons[i]) && strcmp(addons[i].id, addonShow) != 0) slot = &addons[i];
  return slot;
}

/* Checks an add-on post (or a file add-on's content, which has the same shape) and puts it in its slot. Returns why not, or nullptr. */
/* dryRun: every check, and nothing changed, so a file can be judged before it replaces one that works. */
static const char *addonApply(JsonVariantConst doc, bool *picked, bool dryRun = false) {
  const char *id = doc["id"].as<const char *>();
  if (const char *bad = addonIdError(id)) return bad;
  xSemaphoreTake(addonLock, portMAX_DELAY);
  const char *error = nullptr;
  Addon *slot = addonById(id);
  if (doc["remove"].as<bool>()) {
    if (slot && !dryRun) slot->used = false;
  } else if (!dryRun && !(slot = addonSlotFor(id))) {
    error = "Four add-ons are already posting; remove one first";
  } else {
    error = [&]() -> const char * {
  Addon next = {};
  next.used = true;
  strlcpy(next.id, id, sizeof next.id);
  const char *name = doc["name"].as<const char *>();
  strlcpy(next.name, name && *name ? name : id, sizeof next.name);
  const int ttl = doc["ttl"].isNull() ? 120 : doc["ttl"].as<int>();
  if (ttl < 1 || ttl > 3600) return "ttl is 1-3600 seconds";
  next.ttlMs = (uint32_t)ttl * 1000;
  next.at = millis();

  if (!doc["frame"].isNull()) {
    next.kind = ADDON_FRAME;
    JsonArrayConst list = doc["frame"].as<JsonArrayConst>();
    const char *hex = doc["frame"].as<const char *>();
    for (int i = 0; i < LED_COUNT; i++) {
      uint32_t colour = 0;
      /* Either a list of 25 "#RRGGBB", or the same as one run of 150 hex digits. */
      const bool ok = hex ? (strlen(hex) == LED_COUNT * 6 && parseColour(hex + i * 6, colour))
                          : (list.size() == LED_COUNT && parseColour(list[i].as<const char *>(), colour));
      if (!ok) return "frame is 25 colours: a list of #RRGGBB, or 150 hex digits";
      next.pixels[i][0] = (colour >> 16) & 255; next.pixels[i][1] = (colour >> 8) & 255; next.pixels[i][2] = colour & 255;
    }
  } else if (!doc["meters"].isNull()) {
    next.kind = ADDON_METERS;
    JsonArrayConst list = doc["meters"].as<JsonArrayConst>();
    if (list.size() < 1 || list.size() > 4) return "meters is a list of 1-4";
    for (JsonVariantConst one : list) {
      uint32_t colour = 0xFFFFFF;
      if (!one["colour"].isNull() && !parseColour(one["colour"].as<const char *>(), colour)) return "Colour must be #RRGGBB";
      next.meterColour[next.meterCount] = colour;
      next.meterValue[next.meterCount] = constrain(one["value"].as<float>(), 0.0f, 100.0f);
      next.meterCount++;
    }
  } else if (!doc["look"].isNull()) {
    next.kind = ADDON_LOOK;
    JsonVariantConst look = doc["look"];
    next.look = {0xFFFFFF, FX_SOLID, 50};
    if (!look["colour"].isNull() && !parseColour(look["colour"].as<const char *>(), next.look.colour)) return "Colour must be #RRGGBB";
    if (!look["colour2"].isNull() && !parseColour(look["colour2"].as<const char *>(), next.look.colour2)) return "Second colour must be #RRGGBB";
    if (!look["effect"].isNull()) {
      int fx = findName(FX_NAMES, FX_COUNT, look["effect"].as<const char *>());
      if (fx < 0) return "Unknown effect";
      next.look.effect = fx;
    }
    if (!look["speed"].isNull()) next.look.speed = constrain(look["speed"].as<int>(), 1, 100);
    next.fill = look["progress"].isNull() ? 100 : constrain(look["progress"].as<float>(), 0.0f, 100.0f);
  } else {
    return "Send one of look, meters or frame";
  }
  if (!dryRun) *slot = next;
  return nullptr;
    }();
  }
  xSemaphoreGive(addonLock);
  if (picked) *picked = strcmp(addonShow, id) == 0;
  return error;
}

static void handleAddon() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  bool picked = false;
  if (const char *error = addonApply(doc.as<JsonVariantConst>(), &picked)) return sendResult(false, error);
  JsonDocument answer;
  answer["ok"] = true;
  /* So a script can tell whether it is the one on the bar right now, and say so to whoever runs it. */
  answer["picked"] = picked;
  sendJson(200, answer);
}

/* What the bar can be asked to do, for anyone writing something to talk to it. */
static const char API_DOC[] PROGMEM = R"JSON({"about":"Panda State, open firmware for BIQU Panda Status P2, Panda Status, Panda Lux RGB and Panda RGB Controller (board in /api/status). JSON in, JSON out, no sign-in; every POST answers {ok, message?}.",
"endpoints":[
{"call":"GET /api/log","does":"A report for a bug report: version, state, settings and a log of what the owner did and what went wrong, kept across restarts. Holds no Wi-Fi name or password, access code, serial or Home Assistant login"},
{"call":"GET /api/status","does":"Everything: what is showing and why, printer link, microphone, clock, add-ons, settings, the 25 LEDs as hex"},
{"call":"POST /api/state","body":{"state":"idle|printing|paused|finished|cooling|ready|error|off","progress":"0-100"},"does":"A controller saying what the printer is doing. Repeat at least every 45 s; when it stops, the bar's own printer link takes over"},
{"call":"POST /api/addon","body":{"id":"a-z0-9-_ up to 15","name":"shown on the page","ttl":"seconds this post stays valid, default 120","look":{"colour":"#RRGGBB","colour2":"#RRGGBB","effect":"any effect listed under /api/config","speed":"1-100","progress":"0-100"},"meters":[{"value":"0-100","colour":"#RRGGBB"}],"frame":"25 colours as a list of #RRGGBB or 150 hex digits"},"does":"An add-on saying what it wants shown: send ONE of look, meters (1-4) or frame, and keep posting. Shown only if picked under Add-ons. {id, remove:true} withdraws it"},
{"call":"POST /api/config","body":{"brightness":"0-100","mode":"status|music","music":{"sensitivity":"low|medium|high|auto"},"states":{"<state>":{"colour":"#RRGGBB","colour2":"#RRGGBB or auto","on":"true|false","effect":"solid|breathe|fill|flash|chase|rainbow|gradient|waves|scroll|theatre|scanner|meteor|fire|twinkle|sparkle|rain|aurora","speed":"1-100"}},"error":{"minutes":"0-1440: how long an error is shown before the idle look returns; 0 until it clears"},"cool":{"minutes":"0-120","target":"20-60"},"schedule":{"enabled":true,"from":"HH:MM","to":"HH:MM","level":"0-100"},"time":{"tz":"POSIX rule","zone":"label"},"addon":{"show":"<id> or empty","when":"idle|always"},"order":"GRB|RGB|...","reversed":false,"glow":"true: unlit LEDs keep a faint glow; false: fully off"},"does":"Change any subset of the settings; saved"},
{"call":"POST /api/preview","body":{"state":"<state>","progress":"0-100","seconds":"1-120"},"does":"Show a state briefly without it counting as the printer's"},
{"call":"POST /api/printer","body":{"ip":"...","serial":"...","code":"..."},"does":"Pair with a Bambu printer; {forget:true} unpairs"},
{"call":"POST /api/printer","body":{"type":"klipper","host":"IP or name.local","port":"7125","key":"Moonraker API key, optional"},"does":"Follow a Klipper printer through Moonraker instead (polled every 2 s)"},
{"call":"POST /api/ha","body":{"on":"true|false","host":"broker","port":"1883","user":"...","password":"...","prefix":"homeassistant"},"does":"Home Assistant over MQTT with discovery: a light, sensors and (P2) a Mode select appear by themselves. <topic>/state/set takes the same JSON as POST /api/state. Status in /api/status under homeAssistant"},
{"call":"POST /api/wifi","body":{"ssid":"...","password":"..."},"does":"Save Wi-Fi and restart to join it (joined without a restart while a new version is on trial)"},
{"call":"GET /api/wifi/scan","does":"Networks in range. The first call starts a scan and answers {scanning:true}; ask again for {networks:[{ssid,rssi,open}]}. ?again starts a fresh one"},
{"call":"POST /api/ap","body":{"always":"true|false","ssid":"1-32","password":"8-63, or empty for the default"},"does":"The bar's own hotspot: by default it comes up after a minute without Wi-Fi; always keeps it on. At 192.168.4.1"},
{"call":"POST /api/addonfile","body":{"url":"https://... an add-on file to fetch at every start","file":"or the add-on file itself, up to 4000 characters","remove":true},"does":"An add-on the bar runs itself: look, meters or frame as above, or frames {fps, list} for an animation, or source {url, path, min, max, colour, every} for a live meter read from a web address, or filament {effect, speed} for the colour of the filament the printer is using. It is shown as soon as it loads. Its state is in /api/status under addonFile"},
{"call":"POST /api/update","body":{"check":true,"install":true,"auto":"true|false"},"does":"Look for a newer release on GitHub, or install it (checked against the release's SHA256SUMS, into the other slot, on trial). auto: look once a day and say so on the page; it never installs by itself. Progress is in /api/status under update"},
{"call":"POST /update","body":"multipart file","does":"Install a firmware .bin into the other slot and restart into it"},
{"call":"POST /api/slot | /api/reboot | /api/factory","does":"Start the other slot's firmware | restart | clear this firmware's settings"}
]})JSON";

static void handleWifi() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  const char *ssid = doc["ssid"].as<const char *>();
  const char *pass = doc["password"].as<const char *>();
  if (!ssid || !*ssid || strlen(ssid) > 32) return sendResult(false, "Network name is 1-32 characters");
  if (pass && strlen(pass) > 63) return sendResult(false, "Password is at most 63 characters");
  const bool had = prefs.isKey("ssid");
  const String oldSsid = prefs.getString("ssid", ""), oldPass = prefs.getString("pass", "");
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass ? pass : "");
  /* Read back rather than trusting the write: a full settings store refuses it without a word, and half of a new network (its name with the old password) would leave the bar on neither. What was there is put back. */
  if (prefs.getString("ssid", "") != ssid || prefs.getString("pass", "") != (pass ? pass : "")) {
    if (had) { prefs.putString("ssid", oldSsid); prefs.putString("pass", oldPass); }
    else { prefs.remove("ssid"); prefs.remove("pass"); }
    logLine("Wi-Fi details not saved: the settings store is full");
    return sendResult(false, "The bar could not save that: its settings store is full. Removing an add-on file makes room.");
  }
  logLine("Wi-Fi details changed%s", onTrial ? " (on trial: joining without a restart)" : "; restarting to join");
  if (onTrial) {
    /* On trial a restart may be the end of it: a bootloader that rolls back takes a second start of an unproven image as its failure. So the network is joined as the bar stands, and the wait for it starts again from here. */
    wifiSsid = ssid;
    wifiPass = pass ? pass : "";
    /* At least two minutes from now to join it, and never less than was already left: a mistyped password in the first minute of the ten must not cut the rest of them short. */
    const uint32_t spent = millis() - trialFrom;
    if (spent >= trialAllow || trialAllow - spent < TRIAL_GIVE_UP_MS) { trialFrom = millis(); trialAllow = TRIAL_GIVE_UP_MS; }
    apByButton = false;   // the setup network has done what it was raised for: it goes once the bar has been on Wi-Fi for a minute, as it would have after the restart this path leaves out
    sendResult(true, (String("Saved. Joining it now, without a restart, because this version is still on trial. On that network the bar is at http://") + hostName + ".local").c_str());
    delay(300);   // the answer leaves before the radio moves to the new network's channel
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    lastTryAt = millis();
    return;
  }
  sendResult(true, "Saved. Restarting to join it.");
  delay(500);
  ESP.restart();
}

/* The bar's own network: {"always":bool, "ssid":"...", "password":"8-63 characters"}, any subset. */
static void handleAp() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  if (!doc["ssid"].isNull()) {
    const String ssid = doc["ssid"] | "";
    if (!ssid.length() || ssid.length() > 32) return sendResult(false, "The hotspot's name is 1-32 characters");
    prefs.putString("apssid", ssid);
  }
  if (!doc["password"].isNull()) {
    const String pass = doc["password"] | "";
    /* WPA2 needs 8; an open hotspot would let anyone nearby change the bar's Wi-Fi. Empty goes back to the default. */
    if (pass.length() && (pass.length() < 8 || pass.length() > 63)) return sendResult(false, "The hotspot's password is 8-63 characters (or empty for the default)");
    if (pass.length()) prefs.putString("appass", pass); else prefs.remove("appass");
  }
  if (!doc["always"].isNull()) prefs.putBool("apalways", doc["always"].as<bool>());
  loadAp();
  /* Applied now. "Always" starts it; back to fallback with Wi-Fi working takes it down; otherwise one that is up restarts with the new name and password. */
  const char *said = "Saved";
  if (apAlways) { startAp(); said = "Saved. The hotspot is on."; }
  else if (apUp && !apByButton && WiFi.status() == WL_CONNECTED) { stopAp(); said = "Saved. The hotspot is off while Wi-Fi works."; }
  else if (apUp) { startAp(); said = "Saved. The hotspot restarted with these settings."; }
  sendResult(true, said);
}

/* Networks in range, for the Wi-Fi card. The first call starts a scan; the page asks again until it has finished. */
static void handleWifiScan() {
  const int16_t found = WiFi.scanComplete();
  JsonDocument doc;
  if (found == WIFI_SCAN_RUNNING) { doc["scanning"] = true; return sendJson(200, doc); }
  if (found < 0 || server.hasArg("again")) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    doc["scanning"] = true;
    return sendJson(200, doc);
  }
  JsonArray list = doc["networks"].to<JsonArray>();
  for (int i = 0; i < found; i++) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool seen = false;
    for (JsonVariantConst one : list) if (ssid == one["ssid"].as<const char *>()) { seen = true; break; }
    if (seen) continue;   // the same network from several access points: the strongest comes first
    JsonObject one = list.add<JsonObject>();
    one["ssid"] = ssid;
    one["rssi"] = WiFi.RSSI(i);
    one["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(200, doc);
}

/* Pair with a printer, or {"forget":true} to unpair. The link's task picks the change up by itself. */
static void handlePrinter() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  String ip, serial, code;
  const bool klipper = !doc["forget"].as<bool>() && strcmp(doc["type"] | "bambu", "klipper") == 0;
  logLine("Printer pairing changed: %s", doc["forget"].as<bool>() ? "unpaired" : klipper ? "Klipper" : "Bambu Lab");
  uint16_t port = 7125;
  if (klipper) {
    ip = doc["host"] | "";
    code = doc["key"] | "";
    ip.trim(); code.trim();
    if (!doc["port"].isNull()) {
      const int p = doc["port"].as<int>();
      if (p < 1 || p > 65535) return sendResult(false, "The port is 1-65535");
      port = p;
    }
    if (!ip.length() || ip.length() > 63 || ip.indexOf(' ') >= 0 || ip.indexOf('/') >= 0 || ip.indexOf(':') >= 0)
      return sendResult(false, "Give Klipper's address, e.g. 192.168.1.60 or mainsail.local, without http:// or a port");
    if (code.length() > 64) return sendResult(false, "That API key is too long");
  } else if (!doc["forget"].as<bool>()) {
    ip = doc["ip"].as<const char *>() ? doc["ip"].as<const char *>() : "";
    serial = doc["serial"].as<const char *>() ? doc["serial"].as<const char *>() : "";
    code = doc["code"].as<const char *>() ? doc["code"].as<const char *>() : "";
    ip.trim(); serial.trim(); code.trim();
    IPAddress parsed;
    if (!parsed.fromString(ip)) return sendResult(false, "The printer's address should look like 192.168.1.50");
    if (serial.length() < 8 || serial.length() > 20) return sendResult(false, "The serial number is 8-20 characters");
    if (code.length() < 4 || code.length() > 16) return sendResult(false, "The access code is 4-16 characters");
  }
  prefs.putString("pip", ip);
  prefs.putString("psn", serial);
  prefs.putString("pcode", code);
  prefs.putUChar("ptype", klipper ? PR_KLIPPER : PR_BAMBU);
  prefs.putUShort("pport", port);
  /* Remembered, so an unpaired bar does not quietly pair itself again from the stock firmware's old record. */
  prefs.putBool("pset", true);
  xSemaphoreTake(prLock, portMAX_DELAY);
  prIp = ip; prSerial = serial; prCode = code;
  prKind = klipper ? PR_KLIPPER : PR_BAMBU; prPort = port;
  xSemaphoreGive(prLock);
  prGcode[0] = 0; prPercent = -1; prBed = NAN; prLastMsgAt = 0; prConnected = false;
  lastGcode[0] = 0; finishedAt = 0; finishSeenAt = 0; finishedHot = NAN;   // the last printer's print is not this one's
  prRestart = true;
  sendResult(true, !ip.length() ? "Unpaired." : klipper ? "Saved. Asking Klipper." : "Saved. Connecting to the printer.");
}

/* The firmware in the other slot, as a file: after the first install that is
   the board's own stock firmware, and a copy of it is the way back if an
   update later writes over that slot. Only the image is sent (its length read
   from its own header), never the settings store, so no Wi-Fi password or
   printer code is in it. Put it back with Install file. */
static void handleOtherSlotDownload() {
  const esp_partition_t *part;
  esp_app_desc_t desc;
  if (!otherSlot(part, desc)) return sendResult(false, "There is no firmware in the other slot");
  const esp_partition_pos_t pos = {part->address, part->size};
  esp_image_metadata_t meta = {};
  if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK || !meta.image_len || meta.image_len > part->size)
    return sendResult(false, "The firmware in the other slot does not check out, so it was not copied");
  String name = String(desc.project_name) + "-" + desc.version + "-backup.bin";
  for (size_t i = 0; i < name.length(); i++) if (!isalnum((unsigned char)name[i]) && name[i] != '.' && name[i] != '-' && name[i] != '_') name.setCharAt(i, '_');
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[4096]);
  if (!buf) return sendResult(false, "Not enough memory right now; try again in a moment");
  server.setContentLength(meta.image_len);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.send(200, "application/octet-stream", "");
  for (uint32_t at = 0; at < meta.image_len;) {
    const uint32_t n = min<uint32_t>(4096, meta.image_len - at);
    if (esp_partition_read(part, at, buf.get(), n) != ESP_OK) break;
    server.sendContent((const char *)buf.get(), n);
    at += n;
  }
}

static void handleSlot() {
  const esp_partition_t *part;
  esp_app_desc_t desc;
  if (!otherSlot(part, desc)) return sendResult(false, "There is no firmware in the other slot");
  {
    char board[16], build[48];
    otherSlotTag(part, board, sizeof board, build, sizeof build);
    if (board[0] && strcmp(board, BOARD_ID)) return sendResult(false, "The other slot holds Panda State for another board, so the bar stays on this one");
  }
  /* Checked in full before promising anything: a slot left half-written by a failed update still has a readable header. */
  if (esp_ota_set_boot_partition(part) != ESP_OK) { logLine("Switch slot refused: the other slot's firmware does not check out"); return sendResult(false, "The firmware in the other slot does not check out, so the bar stays on this one"); }
  logLine("Switching to the other slot (%s %s)", desc.project_name, desc.version);
  sendResult(true, "Restarting into the other slot");
  delay(500);
  /* Not bootOtherSlot(): that sets the boot slot again, and a second write lands on the boot record's other entry, the one for the slot being left. With both entries on the new slot a failed trial there "went back" to itself first, and the slot left behind had lost its good mark (0.7.12). */
  memset(frame, 0, sizeof frame);
  outShow();
  delay(200);
  ESP.restart();
}

/* Forget everything this firmware has saved (looks, mode, pairing, Wi-Fi) and start again. Stock's own settings are a separate namespace and are not touched, so the bar comes back on the Wi-Fi and pairing stock had. */
static void handleFactory() {
  logLine("Settings reset by the owner");
  /* The note that this build has proved itself is not a setting. Without it the restart is a trial again, and a bar reset because its Wi-Fi changed would wait two minutes with no setup network and then hand itself to the other slot. */
  const String proven = prefs.getString("okbuild", "");
  prefs.clear();
  if (proven.length()) prefs.putString("okbuild", proven);
  sendResult(true, "Settings cleared. Restarting.");
  delay(500);
  ESP.restart();
}

static void handleReboot() {
  logLine("Restart asked for from the page");
  sendResult(true, "Restarting");
  delay(500);
  ESP.restart();
}

static bool uploadOk = false;   // this upload began an update and nothing has gone wrong with it
static bool uploadOnTrial = false;   // it was turned away because the running build has not proved itself yet
static bool uploadFirst = false;     // the next piece is the start of the file
static char uploadBoard[16] = "";    // the board the file was made for, when it was turned away for being another one's
static void handleUpdateDone() {
  const bool ok = uploadOk;
  uploadOk = false;
  if (!ok && uploadOnTrial) { uploadOnTrial = false; logLine("Firmware upload refused: this version is on trial"); return sendResult(false, TRIAL_NO_UPDATE); }
  if (!ok && uploadBoard[0]) {
    const String why = String("That file is Panda State for another board (") + uploadBoard + "); this is a " BOARD_NAME ", whose files are named panda-" BOARD_ID "-custom-<version>.bin. Nothing was written.";
    logLine("Firmware upload refused: made for the %s board", uploadBoard);
    uploadBoard[0] = 0;
    return sendResult(false, why.c_str());
  }
  if (!ok) { logLine("Firmware upload failed: %s", Update.hasError() ? Update.errorString() : "no file, or another update running"); return sendResult(false, Update.hasError() ? Update.errorString() : "No firmware file arrived, or another update is already running"); }
  logLine("Firmware uploaded: restarting into it");
  sendResult(true, "Installed. Restarting into it.");
  delay(800);
  ESP.restart();
}

static void handleUpdateChunk() {
  if (!server.uploading()) return;   // a POST without a form around the file comes here with no upload behind it
  HTTPUpload &upload = server.upload();
  /* The whole upload arrives inside this one handler, so the loop that draws the LEDs does not run meanwhile: the progress is drawn from here. The request's size includes the form around the file, close enough for a progress bar. */
  static uint32_t drawnAt = 0;
  const size_t whole = server.clientContentLength();
  if (upload.status == UPLOAD_FILE_START) {
    /* Refused, without an error of its own, while an update from GitHub is being written: there is one updater and it is busy. */
    /* Not while this build is on trial either: the other slot holds the last firmware known to work, and it is where a failed trial goes back to. */
    uploadOnTrial = onTrial;
    uploadFirst = true;
    uploadBoard[0] = 0;
    uploadOk = !onTrial && Update.begin(UPDATE_SIZE_UNKNOWN);
    barProgress = uploadOk ? 0 : -1;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    /* The first piece (1,436 bytes) holds the board tag, if the file has one. It is read before anything is written, so a file for another board leaves the other slot as it was: the updater only writes once it has 4 KB. */
    if (uploadOk && uploadFirst) {
      uploadFirst = false;
      char board[16], build[48];
      imageTag(upload.buf, upload.currentSize, board, sizeof board, build, sizeof build);
      if (board[0] && strcmp(board, BOARD_ID)) { Update.abort(); uploadOk = false; strlcpy(uploadBoard, board, sizeof uploadBoard); barProgress = -1; }
    }
    if (uploadOk && !Update.hasError()) Update.write(upload.buf, upload.currentSize);
    if (uploadOk && whole) barProgress = (int8_t)min<uint64_t>(99, (uint64_t)upload.totalSize * 100 / whole);
  } else if (upload.status == UPLOAD_FILE_END) {
    uploadOk = uploadOk && Update.end(true);
    barProgress = uploadOk ? 100 : -1;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadOk) Update.abort();   // only an update this upload began: the one from GitHub is not ours to stop
    uploadOk = false;
    uploadOnTrial = false;
    barProgress = -1;
  }
  if (barProgress >= 0 && millis() - drawnAt > 150) { drawnAt = millis(); renderProgress(); }
}

/* ---------------------------------------------------------- online -- */

/**
 * Everything that reaches out to the internet: checking GitHub for a newer
 * release and installing it, and add-ons that are a file rather than a
 * program on the network. One task does all of it, so a slow server never
 * freezes the LEDs or the page, and only one HTTPS session runs at a time.
 *
 * HTTPS is checked against the root certificates in roots.h, never skipped:
 * a firmware image from a server that could not prove who it is would be the
 * one thing worse than no update at all. The image is also checked against
 * the release's SHA256SUMS.txt before it is allowed to boot.
 */

/**
 * An HTTP(S) GET that follows redirects. GitHub's downloads always redirect,
 * from github.com to a different file server. The Arduino client's own
 * redirect handling reuses the first connection for the second host, and
 * measured on 0.6.0 that failed every time ("could not connect") while both
 * hosts worked when asked directly; so each hop here is a fresh connection.
 */
static bool prPaired() {
  xSemaphoreTake(prLock, portMAX_DELAY);
  const bool paired = prIp.length() && prSerial.length();
  xSemaphoreGive(prLock);
  return paired;
}

/* A String that stops growing at cap: the HTTP client's copy then ends with a write error instead of reading on until the memory runs out. getString() had no ceiling for an answer sent without a length (chunked), which is how many APIs answer. */
struct CappedString : StreamString {
  size_t cap;
  bool overCap = false;      // refused because the answer is bigger than cap
  bool noMemory = false;     // refused because the memory to grow into was not there in one piece
  explicit CappedString(size_t c) : cap(c) {}
  size_t write(const uint8_t *data, size_t size) override {
    if (length() + size > cap) { overCap = true; return 0; }
    const size_t done = StreamString::write(data, size);
    if (!done && size) noMemory = true;
    return done;
  }
  size_t write(uint8_t c) override { return write(&c, 1); }
};

struct Fetch {
  WiFiClientSecure tls;
  WiFiClient plain;
  HTTPClient http;
  String host;                 // the server of the last hop, for the error message
  bool held = false;           // the printer link was let go for this download; picked up again when it is done
  bool holdFirst = false;      // let the printer link go before the first try, not after it fails: a failed handshake costs seconds someone is watching
  bool http10 = false;         // ask in HTTP/1.0, so the answer is never chunked and can be parsed straight off the connection
  void hold() {
    held = true;
    prHoldAck = false;
    prHold = true;
    prRestart = true;
    for (int i = 0; i < 150 && !prHoldAck; i++) delay(100);
    delay(300);
  }
  int begin(String url, uint32_t timeoutMs = 15000) {
    if (holdFirst && !held && url.startsWith("https://") && prKind == PR_BAMBU && prPaired()) hold();
    static const char *keep[] = {"Location"};
    http.useHTTP10(http10);
    for (int hop = 0; hop < 5; hop++) {
      http.end();
      tls.stop();
      plain.stop();
      const bool secure = url.startsWith("https://");
      const int hostAt = url.indexOf("://") + 3;
      host = url.substring(hostAt, url.indexOf('/', hostAt));
      if (secure) { tls.setCACert(ROOT_CAS); tls.setHandshakeTimeout(12); }
      http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
      http.setTimeout(timeoutMs);
      http.setUserAgent("panda-p2-firmware/" FW_VERSION);
      if (!(secure ? http.begin(tls, url) : http.begin(plain, url))) return -100;
      http.addHeader("Accept", "application/json, */*");
      http.collectHeaders(keep, 1);
      int code = http.GET();
      /* Two secure sessions at once (the printer's and this one) leave too
         little memory in one piece for checking a 4096-bit root certificate,
         and that fails as "certificate verification failed": measured on the
         P2 with 38 KB as the largest free block, where 80-90 KB had always
         worked. So a secure download that cannot connect while the Bambu
         link is up lets the link go for those few seconds and tries again.
         "Up" means paired, not just connected: a link in the middle of
         reconnecting holds a handshake's worth of memory too, and checking
         prConnected alone let exactly that case fail (a live meter showing
         X509 errors on and off). It waits for the printer task to say it has
         let go, which can take a handshake timeout, rather than guessing. */
      if (code < 0 && secure && !held && prKind == PR_BAMBU && prPaired()) {
        hold();
        http.end();
        tls.stop();
        tls.setCACert(ROOT_CAS);
        if (!http.begin(tls, url)) return -100;
        http.addHeader("Accept", "application/json, */*");
        http.collectHeaders(keep, 1);
        code = http.GET();
      }
      if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308) return code;
      String next = http.header("Location");
      if (next.startsWith("//")) next = String(secure ? "https:" : "http:") + next;   // another server, same scheme
      else if (next.startsWith("/")) next = String(secure ? "https://" : "http://") + host + next;   // a path on the same server
      /* Never from a secure connection to a plain one: what came back over it could not be trusted. It shows as the redirect it was. */
      if (!next.length() || (secure && !next.startsWith("https://"))) return code;
      url = next;
    }
    return -1;
  }
  /* The answer, at most cap bytes of it. 0 with all of it in out; HTTPC_ERROR_STREAM_WRITE when it is bigger than that; another negative code when the connection failed on the way. */
  int body(String &out, size_t cap) {
    const int size = http.getSize();
    if (size > 0 && (size_t)size > cap) return HTTPC_ERROR_STREAM_WRITE;
    CappedString got(cap);
    if (size > 0) got.reserve(size + 1);
    const int result = http.writeToStream(&got);
    /* The client gives the same error for a reader that would take no more, for one that had no memory left, and for an answer cut short; the reader knows which. 0.7.14 called a shortage of memory "connection lost". */
    if (result == HTTPC_ERROR_STREAM_WRITE) return got.overCap ? HTTPC_ERROR_STREAM_WRITE : got.noMemory ? HTTPC_ERROR_TOO_LESS_RAM : HTTPC_ERROR_CONNECTION_LOST;
    if (result < 0) return result;
    out = std::move(static_cast<String &>(got));
    return 0;
  }
  /* What went wrong, in enough detail to act on: which server, which step, and what the secure connection itself reported. */
  String error(int code) {
    if (code > 0) return host + " answered " + String(code);
    if (code == -100) return "The address could not be read";
    if (code == HTTPC_ERROR_TOO_LESS_RAM) return host + ": not enough memory in one piece for the answer (largest free block " + String(ESP.getMaxAllocHeap() / 1024) + " KB); it is tried again later";
    char tlsWhy[96] = "";
    const int tlsCode = tls.lastError(tlsWhy, sizeof tlsWhy);
    String why = host + ": " + HTTPClient::errorToString(code);
    if (tlsCode) why += " (secure connection: " + String(tlsWhy) + ")";
    return why;
  }
  ~Fetch() { http.end(); tls.stop(); if (held) { prHold = false; prHoldAck = false; } }
};

/* "v0.6.1" against "0.6.0": numbers compared part by part. */
static bool versionNewer(const char *latest, const char *current) {
  int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
  sscanf(latest + (latest[0] == 'v' || latest[0] == 'V'), "%d.%d.%d", &a[0], &a[1], &a[2]);
  sscanf(current, "%d.%d.%d", &b[0], &b[1], &b[2]);
  for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
  return false;
}

enum UpdPhase : uint8_t { UPD_IDLE, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_DONE, UPD_FAILED };
static const char *UPD_NAMES[] = {"idle", "checking", "current", "available", "downloading", "installed", "failed"};
static volatile uint8_t updPhase = UPD_IDLE;
static volatile uint8_t updPercent = 0;
static volatile uint8_t updWant = 0;              // 1 check, 2 install: asked for by the page, done by the online task
static char updLatest[24] = "";
static String updAssetName, updAssetUrl, updSumsUrl, updPageUrl, updMessage;
static uint32_t updCheckedAt = 0;                // millis() of the last answer from GitHub; 0 never
static bool updAuto = true;                      // look once a day and say so on the page; never installs by itself

static bool updCheck() {
  updPhase = UPD_CHECKING;
  Fetch f;
  f.http10 = true;   // the answer is parsed as it arrives, below, which a chunked one would garble
  const int code = f.begin("https://api.github.com/repos/" UPDATE_REPO "/releases/latest");
  if (code != 200) { updMessage = code == 404 ? String("No release published yet") : f.error(code); updPhase = UPD_FAILED; return false; }
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["html_url"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  /* Parsed as it arrives, keeping only the fields above: the whole answer (12.6 KB in 0.7.14) never has to fit in memory at once. Collecting it first needed that much in one piece beside two secure connections, and failed when it was not there. */
  JsonDocument doc;
  const DeserializationError bad = deserializeJson(doc, f.http.getStream(), DeserializationOption::Filter(filter));
  if (bad) { updMessage = String("GitHub's answer could not be read (") + bad.c_str() + ")"; updPhase = UPD_FAILED; return false; }
  strlcpy(updLatest, doc["tag_name"] | "", sizeof updLatest);
  updPageUrl = doc["html_url"] | "";
  updAssetName = updAssetUrl = updSumsUrl = "";
  for (JsonVariantConst a : doc["assets"].as<JsonArrayConst>()) {
    String name = a["name"] | "";
    if (name.startsWith("panda-" BOARD_ID "-custom-") && name.endsWith(".bin")) { updAssetName = name; updAssetUrl = a["browser_download_url"] | ""; }
    if (name == "SHA256SUMS.txt") updSumsUrl = a["browser_download_url"] | "";
  }
  updCheckedAt = millis();
  if (!versionNewer(updLatest, FW_VERSION)) { updMessage = ""; updPhase = UPD_CURRENT; return true; }
  if (!updAssetUrl.length() || !updSumsUrl.length()) { updMessage = String("Release ") + updLatest + " has no firmware file or checksum list"; updPhase = UPD_FAILED; return false; }
  updMessage = "";
  updPhase = UPD_AVAILABLE;
  return true;
}

static bool updFail(const String &why) {
  barProgress = -1;
  updMessage = why;
  updPhase = UPD_FAILED;
  prPaused = false;
  prRestart = true;
  return false;
}

static bool updInstall() {
  if (updPhase != UPD_AVAILABLE && !updCheck()) return false;
  if (updPhase != UPD_AVAILABLE) return updFail("Already on the newest release");

  /* The expected checksum first: a list a few lines long. */
  String expected;
  {
    Fetch f;
    const int code = f.begin(updSumsUrl);
    if (code != 200) return updFail("Checksum list: " + f.error(code));
    String sums;
    if (f.body(sums, 4096) < 0) return updFail("The checksum list could not be read");
    const int at = sums.indexOf(updAssetName);
    if (at < 0) return updFail("The checksum list does not mention " + updAssetName);
    const int lineStart = sums.lastIndexOf('\n', at) + 1;
    expected = sums.substring(lineStart, lineStart + 64);
    expected.toLowerCase();
    if (expected.length() != 64) return updFail("The checksum list could not be read");
  }

  updPhase = UPD_DOWNLOADING;
  updPercent = 0;
  barProgress = 0;
  prPaused = true;
  prRestart = true;
  vTaskDelay(pdMS_TO_TICKS(1500));   // let the printer link close and give its memory back

  Fetch f;
  const int code = f.begin(updAssetUrl, 20000);
  if (code != 200) return updFail("Download: " + f.error(code));
  const int total = f.http.getSize();
  if (total <= 0) return updFail("The download did not say how big it is");
  if (!Update.begin(total)) return updFail(String("Will not fit: ") + Update.errorString());

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  WiFiClient *stream = f.http.getStreamPtr();
  static uint8_t buf[2048];
  int written = 0;
  uint32_t lastByteAt = millis();
  while (written < total) {
    const int ready = stream->available();
    if (ready <= 0) {
      if (millis() - lastByteAt > 20000) { Update.abort(); mbedtls_sha256_free(&sha); return updFail("The download stalled"); }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    const int n = stream->readBytes(buf, min(ready, (int)sizeof buf));
    if (n <= 0) continue;
    lastByteAt = millis();
    mbedtls_sha256_update_ret(&sha, buf, n);
    if (Update.write(buf, n) != (size_t)n) { Update.abort(); mbedtls_sha256_free(&sha); return updFail(String("Writing failed: ") + Update.errorString()); }
    written += n;
    updPercent = (uint8_t)((int64_t)written * 100 / total);
    barProgress = updPercent;
  }
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  if (expected != hex) { Update.abort(); return updFail("The download did not match its checksum, so it was not installed"); }
  /* Writes the boot record: the new build starts next, on trial, and hands
     the bar back to this one by itself if it cannot get on Wi-Fi. */
  if (!Update.end()) return updFail(String("Could not finish: ") + Update.errorString());
  updMessage = String("Installed ") + updLatest + ". Restarting into it.";
  updPhase = UPD_DONE;
  barProgress = 100;
  vTaskDelay(pdMS_TO_TICKS(1500));
  ESP.restart();
  return true;
}

/* -------------------------------------------------- file add-ons -- */

/**
 * An add-on that is a file instead of a program: a few hundred bytes of JSON
 * the bar runs itself, uploaded from the page or picked from the store. A
 * store add-on is fetched from its address every time the bar starts and only
 * the address is kept, so the bar holds no library of them.
 *
 *   { "id": "aurora", "name": "Aurora", "description": "...", "author": "...",
 *     and ONE of:
 *     "look" | "meters" | "frame"   exactly as POST /api/addon takes them
 *     "frames": { "fps": 1-20, "list": ["150 hex digits", ...] }   up to 30, looped
 *     "source": { "url": "https://...", "path": "a.b.0.c", "min": 0, "max": 100,
 *                 "colour": "#RRGGBB", "every": 30-86400 }   a live meter
 *   }
 */
enum FileKind : uint8_t { FA_NONE, FA_STATIC, FA_FRAMES, FA_SOURCE, FA_FILAMENT, FA_COUNTDOWN, FA_PRINTER, FA_MAINT };
static const char *FA_NAMES[] = {"none", "static", "frames", "source", "filament", "countdown", "printer", "maintenance"};
/* What a "printer" add-on shows as its meter. */
enum PrinterReading : uint8_t { PRD_REMAIN, PRD_HUMIDITY, PRD_TIMELEFT, PRD_HEATING, PRD_CHAMBER, PRD_COUNT };
static const char *PRD_NAMES[PRD_COUNT] = {"remain", "humidity", "timeleft", "heating", "chamber"};
static uint8_t faReading = PRD_REMAIN;
static bool faColourSet = false;      // the file gave a colour; otherwise the reading picks one
static float faTotal = 0;             // time left: the most seen in this print, which the meter drains from
static const char *faUnit = "";
static uint32_t faMaintHours = 200;
static String faTask;
/* A countdown: a meter that drains from "from" to "to" (local times, in the
   bar's own time zone), then the done look for doneHours, then nothing. */
static time_t faCdFrom = 0, faCdTo = 0;
static uint8_t faCdDone = FX_RAINBOW;
static uint16_t faCdDoneHours = 12;
static bool parseLocalTime(const char *text, time_t &out) {
  if (!text) return false;
  struct tm t = {};
  if (!strptime(text, "%Y-%m-%dT%H:%M", &t)) return false;
  t.tm_isdst = -1;  // the zone rule decides, so a countdown across a daylight-saving change still ends on the hour asked
  out = mktime(&t);
  return out > 0;
}
static uint8_t faFilEffect = FX_SOLID, faFilSpeed = 40;
static const int FA_MAX_FRAMES = 30;
static const size_t FA_MAX_UPLOAD = 4000;         // what fits comfortably in the settings store
static const size_t FA_MAX_FETCH = 12000;
static volatile bool faReload = true;
static bool faPick = false;                      // show it as soon as it loads (it was just installed)
static uint8_t faKind = FA_NONE;
static char faId[16] = "", faName[28] = "", faAuthor[40] = "";
static String faUrl, faDesc, faError, faShow, faSrcUrl, faPath;
static String faInline;  // a store file handed over by the page along with its address, used once instead of fetching it
static String faBase;  // a second path: when set, the meter shows the percentage change from it to path (a share price against yesterday's close)
static uint8_t faFrames[FA_MAX_FRAMES][LED_COUNT][3];
static uint8_t faFrameCount = 0, faFps = 4, faFrameAt = 0;
static float faMin = 0, faMax = 100, faValue = NAN;
static uint32_t faColour = 0xFFFFFF, faEveryMs = 300000, faFetchedAt = 0, faPushedAt = 0, faTriedAt = 0;
static uint32_t faRetryAt = 0;                   // a failed fetch of a store add-on waits until then
static uint8_t faFails = 0;                      // failed fetches in a row; the first is retried quietly

/* A countdown's two times are local ones, turned into moments when the file loads. A new time zone makes them different moments, so it loads again. */
static void faZoneChanged() {
  if (faKind != FA_COUNTDOWN) return;
  faReload = true;
  faRetryAt = millis();
}

static bool hexPixels(const char *hex, uint8_t out[LED_COUNT][3]) {
  if (!hex || strlen(hex) != LED_COUNT * 6) return false;
  for (int i = 0; i < LED_COUNT; i++) {
    uint32_t c;
    if (!parseColour(hex + i * 6, c)) return false;
    out[i][0] = (c >> 16) & 255; out[i][1] = (c >> 8) & 255; out[i][2] = c & 255;
  }
  return true;
}

/* Reads a definition and gets it ready to run. Returns why not, or nullptr. */
static const char *faLoad(const String &text) {
  JsonDocument doc;
  if (deserializeJson(doc, text)) return "The add-on file is not valid JSON";
  const char *id = doc["id"].as<const char *>();
  if (const char *bad = addonIdError(id)) return bad;
  strlcpy(faId, id, sizeof faId);
  strlcpy(faName, doc["name"] | id, sizeof faName);
  strlcpy(faAuthor, doc["author"] | "", sizeof faAuthor);
  faDesc = doc["description"] | "";
  faKind = FA_NONE;
  faValue = NAN;
  faFetchedAt = faPushedAt = 0;
  faTriedAt = 0;  // a new add-on gets its first reading straight away, not after the 30 s spacing left over from the one before it

  if (!doc["frames"].isNull()) {
    JsonArrayConst list = doc["frames"]["list"].as<JsonArrayConst>();
    if (list.size() < 1 || list.size() > FA_MAX_FRAMES) return "frames.list holds 1-30 frames";
    faFrameCount = 0;
    for (JsonVariantConst one : list)
      if (!hexPixels(one.as<const char *>(), faFrames[faFrameCount++])) return "Each frame is 150 hex digits (25 colours)";
    faFps = constrain(doc["frames"]["fps"] | 4, 1, 20);
    faFrameAt = 0;
    faKind = FA_FRAMES;
  } else if (!doc["filament"].isNull()) {
    /* The filament the printer is using, as a colour: {"filament":{"effect":"solid","speed":40}}. */
    const int fx = findName(FX_NAMES, FX_COUNT, doc["filament"]["effect"] | "solid");
    if (fx < 0) return "filament.effect is not one of the effects";
    faFilEffect = fx;
    faFilSpeed = constrain(doc["filament"]["speed"] | 40, 1, 100);
    faKind = FA_FILAMENT;
  } else if (!doc["countdown"].isNull()) {
    /* {"countdown":{"from":"2026-11-14T09:00","to":"2026-12-25T07:00","colour":"#22C55E","done":"rainbow","doneHours":12}} */
    JsonVariantConst cd = doc["countdown"];
    if (!parseLocalTime(cd["to"] | "", faCdTo)) return "countdown.to is a local time, YYYY-MM-DDTHH:MM";
    if (!parseLocalTime(cd["from"] | "", faCdFrom)) return "countdown.from is a local time, YYYY-MM-DDTHH:MM";
    if (faCdTo <= faCdFrom) return "countdown.to must be after countdown.from";
    faColour = 0x22C55E;
    if (!cd["colour"].isNull() && !parseColour(cd["colour"].as<const char *>(), faColour)) return "countdown.colour must be #RRGGBB";
    const int fx = findName(FX_NAMES, FX_COUNT, cd["done"] | "rainbow");
    if (fx < 0) return "countdown.done is not one of the effects";
    faCdDone = fx;
    faCdDoneHours = constrain(cd["doneHours"] | 12, 0, 168);
    faKind = FA_COUNTDOWN;
  } else if (!doc["source"].isNull()) {
    JsonVariantConst src = doc["source"];
    faSrcUrl = src["url"] | "";
    if (!faSrcUrl.startsWith("http://") && !faSrcUrl.startsWith("https://")) return "source.url must be an http:// or https:// address";
    faPath = src["path"] | "";
    faBase = src["base"] | "";
    if (faBase.length() && !faPath.length()) return "source.base needs a source.path to compare with it";
    faMin = src["min"] | 0.0f;
    faMax = src["max"] | 100.0f;
    if (faMax == faMin) return "source.min and source.max must differ";
    faColour = 0xFFFFFF;
    if (!src["colour"].isNull() && !parseColour(src["colour"].as<const char *>(), faColour)) return "source.colour must be #RRGGBB";
    faEveryMs = (uint32_t)constrain(src["every"] | 300, 30, 86400) * 1000;
    faKind = FA_SOURCE;
  } else if (!doc["printer"].isNull()) {
    /* {"printer":{"show":"remain|humidity|timeleft|heating|chamber","colour":"#RRGGBB","min":..,"max":..}}: something the printer reports, as a meter. */
    JsonVariantConst pr = doc["printer"];
    const int r = findName(PRD_NAMES, PRD_COUNT, pr["show"] | "");
    if (r < 0) return "printer.show is remain, humidity, timeleft, heating or chamber";
    faReading = r;
    faColourSet = !pr["colour"].isNull();
    faColour = 0xFFFFFF;
    if (faColourSet && !parseColour(pr["colour"].as<const char *>(), faColour)) return "printer.colour must be #RRGGBB";
    faMin = pr["min"] | (r == PRD_HUMIDITY ? 10.0f : r == PRD_CHAMBER ? 20.0f : 0.0f);
    faMax = pr["max"] | (r == PRD_HUMIDITY ? 50.0f : r == PRD_CHAMBER ? 45.0f : 100.0f);
    if (faMax == faMin) return "printer.min and printer.max must differ";
    faTotal = 0;
    faUnit = r == PRD_REMAIN ? "% left" : r == PRD_HUMIDITY ? "" : r == PRD_TIMELEFT ? "min left" : r == PRD_HEATING ? "% heated" : "°C";
    faKind = FA_PRINTER;
  } else if (!doc["maintenance"].isNull()) {
    /* {"maintenance":{"hours":200,"task":"Grease the rods","colour":"#F59E0B"}}: a reminder after so many hours of printing. */
    JsonVariantConst m = doc["maintenance"];
    int hours;
    hours = 200;
    if (!m["hours"].isNull() && !intIn(m["hours"], 1, 10000, hours)) return "maintenance.hours is 1-10000";
    faMaintHours = hours;
    faTask = m["task"] | "";
    faColour = 0xF59E0B;
    if (!m["colour"].isNull() && !parseColour(m["colour"].as<const char *>(), faColour)) return "maintenance.colour must be #RRGGBB";
    faUnit = "print hours";
    faKind = FA_MAINT;
  } else {
    /* Same shape as a post: check it the same way, through the same code. */
    JsonDocument show;
    show["id"] = faId;
    show["name"] = faName;
    show["ttl"] = 120;
    for (const char *key : {"look", "meters", "frame"}) if (!doc[key].isNull()) show[key] = doc[key];
    if (show.size() <= 3) return "Give one of look, meters, frame, frames or source";
    if (const char *bad = addonApply(show.as<JsonVariantConst>(), nullptr)) return bad;
    faShow = "";
    serializeJson(show, faShow);
    faPushedAt = millis();
    faKind = FA_STATIC;
  }
  return nullptr;
}

/* Whether a file would load, without loading it: faLoad's checks on nothing but locals. A file went into the store on its id and the name of its kind alone, replaced the one that worked, and only then failed to load (0.7.13). */
static const char *faCheck(const String &text) {
  JsonDocument doc;
  if (deserializeJson(doc, text)) return "The add-on file is not valid JSON";
  if (const char *bad = addonIdError(doc["id"].as<const char *>())) return bad;
  uint32_t colour;
  if (!doc["frames"].isNull()) {
    JsonArrayConst list = doc["frames"]["list"].as<JsonArrayConst>();
    if (list.size() < 1 || list.size() > FA_MAX_FRAMES) return "frames.list holds 1-30 frames";
    uint8_t scratch[LED_COUNT][3];
    for (JsonVariantConst one : list)
      if (!hexPixels(one.as<const char *>(), scratch)) return "Each frame is 150 hex digits (25 colours)";
  } else if (!doc["filament"].isNull()) {
    if (findName(FX_NAMES, FX_COUNT, doc["filament"]["effect"] | "solid") < 0) return "filament.effect is not one of the effects";
  } else if (!doc["countdown"].isNull()) {
    JsonVariantConst cd = doc["countdown"];
    time_t from, to;
    if (!parseLocalTime(cd["to"] | "", to)) return "countdown.to is a local time, YYYY-MM-DDTHH:MM";
    if (!parseLocalTime(cd["from"] | "", from)) return "countdown.from is a local time, YYYY-MM-DDTHH:MM";
    if (to <= from) return "countdown.to must be after countdown.from";
    if (!cd["colour"].isNull() && !parseColour(cd["colour"].as<const char *>(), colour)) return "countdown.colour must be #RRGGBB";
    if (findName(FX_NAMES, FX_COUNT, cd["done"] | "rainbow") < 0) return "countdown.done is not one of the effects";
  } else if (!doc["source"].isNull()) {
    JsonVariantConst src = doc["source"];
    const String url = src["url"] | "", path = src["path"] | "", base = src["base"] | "";
    if (!url.startsWith("http://") && !url.startsWith("https://")) return "source.url must be an http:// or https:// address";
    if (base.length() && !path.length()) return "source.base needs a source.path to compare with it";
    if ((src["max"] | 100.0f) == (src["min"] | 0.0f)) return "source.min and source.max must differ";
    if (!src["colour"].isNull() && !parseColour(src["colour"].as<const char *>(), colour)) return "source.colour must be #RRGGBB";
  } else if (!doc["printer"].isNull()) {
    JsonVariantConst pr = doc["printer"];
    const int r = findName(PRD_NAMES, PRD_COUNT, pr["show"] | "");
    if (r < 0) return "printer.show is remain, humidity, timeleft, heating or chamber";
    if (!pr["colour"].isNull() && !parseColour(pr["colour"].as<const char *>(), colour)) return "printer.colour must be #RRGGBB";
    if ((pr["min"] | (r == PRD_HUMIDITY ? 10.0f : r == PRD_CHAMBER ? 20.0f : 0.0f)) == (pr["max"] | (r == PRD_HUMIDITY ? 50.0f : r == PRD_CHAMBER ? 45.0f : 100.0f))) return "printer.min and printer.max must differ";
  } else if (!doc["maintenance"].isNull()) {
    JsonVariantConst m = doc["maintenance"];
    int hours;
    hours = 200;
    if (!m["hours"].isNull() && !intIn(m["hours"], 1, 10000, hours)) return "maintenance.hours is 1-10000";
    if (!m["colour"].isNull() && !parseColour(m["colour"].as<const char *>(), colour)) return "maintenance.colour must be #RRGGBB";
  } else {
    JsonDocument show;
    show["id"] = doc["id"];
    show["name"] = doc["name"] | doc["id"].as<const char *>();
    show["ttl"] = 120;
    for (const char *key : {"look", "meters", "frame"}) if (!doc[key].isNull()) show[key] = doc[key];
    if (show.size() <= 3) return "Give one of look, meters, frame, frames, source, filament, countdown, printer or maintenance";
    return addonApply(show.as<JsonVariantConst>(), nullptr, true);
  }
  return nullptr;
}

/**
 * The add-on library: up to LIB_SLOTS add-on files kept on the bar, so
 * switching between them needs no download (0.7.18). One of them runs at a
 * time, as before; the others wait.
 *
 * The settings store is 12 KB shared with stock, far too small for several
 * files, so they are kept at the top of the "coredump" partition instead
 * (64 KB, in every BTT image's table). A crash report is written from the
 * bottom of that partition and only as far as it needs, which is well short
 * of the top 24 KB used here; each slot carries a checksum, so one that a
 * crash report did reach is read as empty rather than as a broken file.
 *
 * Each slot is one 4 KB flash sector: a header, the address it came from (if
 * any), then the file.
 */
static const int LIB_SLOTS = 6;
static const uint32_t LIB_SECTOR = 4096, LIB_MAGIC = 0x4C415350;   // "PSAL"
struct LibHead { uint32_t magic, crc; uint16_t len, urlLen; char id[16]; char name[28]; };
struct LibEntry { bool used; char id[16]; char name[28]; String url; uint16_t len; };
static LibEntry lib[LIB_SLOTS];
static const esp_partition_t *libPart = nullptr;
static SemaphoreHandle_t libLock;
static char libActive[16] = "";   // the slot that runs, by id (kept in the settings as "falib"); empty when none does

static uint32_t libOffset(int slot) { return libPart->size - (uint32_t)(LIB_SLOTS - slot) * LIB_SECTOR; }

static uint32_t libCrc(const LibHead &h, const char *url, const char *text) {
  uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)h.id, sizeof h.id + sizeof h.name);
  crc = esp_rom_crc32_le(crc, (const uint8_t *)url, h.urlLen);
  return esp_rom_crc32_le(crc, (const uint8_t *)text, h.len);
}

/* Reads what each slot holds, once at start. A slot is used only if its checksum is right. */
static void libBegin() {
  libLock = xSemaphoreCreateMutex();
  libPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!libPart || libPart->size < LIB_SLOTS * LIB_SECTOR * 2) { libPart = nullptr; logLine("Add-on library: no room on this bar's flash"); return; }
  std::unique_ptr<char[]> buf(new char[LIB_SECTOR]);
  int count = 0;
  for (int i = 0; i < LIB_SLOTS; i++) {
    lib[i].used = false;
    if (esp_partition_read(libPart, libOffset(i), buf.get(), LIB_SECTOR) != ESP_OK) continue;
    LibHead h;
    memcpy(&h, buf.get(), sizeof h);
    if (h.magic != LIB_MAGIC || sizeof h + h.urlLen + h.len > LIB_SECTOR || !h.len) continue;
    const char *url = buf.get() + sizeof h, *text = url + h.urlLen;
    if (libCrc(h, url, text) != h.crc) continue;
    lib[i].used = true;
    memcpy(lib[i].id, h.id, sizeof h.id); lib[i].id[sizeof lib[i].id - 1] = 0;
    memcpy(lib[i].name, h.name, sizeof h.name); lib[i].name[sizeof lib[i].name - 1] = 0;
    lib[i].url = "";
    lib[i].url.reserve(h.urlLen);
    for (int c = 0; c < h.urlLen; c++) lib[i].url += url[c];
    lib[i].len = h.len;
    count++;
  }
  logLine("Add-on library: %d of %d slots in use", count, LIB_SLOTS);
}

static int libFind(const char *id) {
  for (int i = 0; i < LIB_SLOTS; i++) if (lib[i].used && !strcmp(lib[i].id, id)) return i;
  return -1;
}

/* A slot's file, or an empty string. */
static String libText(int slot) {
  String text;
  if (!libPart || slot < 0 || slot >= LIB_SLOTS) return text;
  xSemaphoreTake(libLock, portMAX_DELAY);
  if (lib[slot].used) {
    const uint16_t urlLen = lib[slot].url.length(), len = lib[slot].len;
    std::unique_ptr<char[]> buf(new char[len + 1]);
    if (esp_partition_read(libPart, libOffset(slot) + sizeof(LibHead) + urlLen, buf.get(), len) == ESP_OK) { buf[len] = 0; text = buf.get(); }
  }
  xSemaphoreGive(libLock);
  return text;
}

/* Keeps a checked file: in the slot of the same id if there is one (an add-on added again replaces its old copy), else in an empty one.
   Returns why not, or nullptr. */
static const char *libSave(const String &text, const String &url, char *idOut = nullptr) {
  if (!libPart) return "This bar has nowhere to keep add-ons";
  if (sizeof(LibHead) + url.length() + text.length() > LIB_SECTOR) return "The add-on file is too big to keep on the bar (about 3,900 characters at most)";
  JsonDocument filter;
  filter["id"] = true;
  filter["name"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, text, DeserializationOption::Filter(filter))) return "The add-on file is not valid JSON";
  LibHead h = {};
  h.magic = LIB_MAGIC;
  strlcpy(h.id, doc["id"] | "", sizeof h.id);
  strlcpy(h.name, doc["name"] | h.id, sizeof h.name);
  h.len = text.length();
  h.urlLen = url.length();
  h.crc = libCrc(h, url.c_str(), text.c_str());
  xSemaphoreTake(libLock, portMAX_DELAY);
  int slot = libFind(h.id);
  for (int i = 0; i < LIB_SLOTS && slot < 0; i++) if (!lib[i].used) slot = i;
  if (slot < 0) { xSemaphoreGive(libLock); return "All 6 add-on slots on the bar are full: remove one under Add-ons first"; }
  std::unique_ptr<char[]> buf(new char[LIB_SECTOR]);
  memset(buf.get(), 0xFF, LIB_SECTOR);
  memcpy(buf.get(), &h, sizeof h);
  memcpy(buf.get() + sizeof h, url.c_str(), h.urlLen);
  memcpy(buf.get() + sizeof h + h.urlLen, text.c_str(), h.len);
  const uint32_t at = libOffset(slot);
  bool ok = esp_partition_erase_range(libPart, at, LIB_SECTOR) == ESP_OK && esp_partition_write(libPart, at, buf.get(), LIB_SECTOR) == ESP_OK;
  if (ok) {
    /* Read back: a write that did not take is said now, not found at the next start. */
    std::unique_ptr<char[]> check(new char[LIB_SECTOR]);
    ok = esp_partition_read(libPart, at, check.get(), LIB_SECTOR) == ESP_OK && !memcmp(check.get(), buf.get(), LIB_SECTOR);
  }
  if (ok) {
    lib[slot].used = true;
    strlcpy(lib[slot].id, h.id, sizeof lib[slot].id);
    strlcpy(lib[slot].name, h.name, sizeof lib[slot].name);
    lib[slot].url = url;
    lib[slot].len = h.len;
  } else lib[slot].used = false;
  xSemaphoreGive(libLock);
  if (!ok) return "The bar could not write the add-on to its flash";
  logLine("Add-on library: %s kept in slot %d", h.id, slot + 1);
  if (idOut) strlcpy(idOut, h.id, sizeof h.id);
  return nullptr;
}

static void libRemove(int slot) {
  if (!libPart || slot < 0 || slot >= LIB_SLOTS) return;
  xSemaphoreTake(libLock, portMAX_DELAY);
  esp_partition_erase_range(libPart, libOffset(slot), LIB_SECTOR);
  logLine("Add-on library: %s removed from slot %d", lib[slot].id, slot + 1);
  lib[slot].used = false;
  lib[slot].url = "";
  xSemaphoreGive(libLock);
}

/* Paths like "data.0.price": numbers index arrays, anything else names a key. */
static void pathFilter(JsonDocument &filter, const String &path) {
  JsonVariant cursor = filter.as<JsonVariant>();
  int from = 0;
  while (from <= (int)path.length()) {
    int dot = path.indexOf('.', from);
    if (dot < 0) dot = path.length();
    const String part = path.substring(from, dot);
    const bool index = part.length() && strspn(part.c_str(), "0123456789") == part.length();
    // Merged into what is already there, so two paths through the same object both survive the filter.
    if (index) {
      JsonArray list = cursor.is<JsonArray>() ? cursor.as<JsonArray>() : cursor.to<JsonArray>();
      cursor = list.size() ? list[0].as<JsonVariant>() : list.add<JsonVariant>();
    } else {
      JsonObject obj = cursor.is<JsonObject>() ? cursor.as<JsonObject>() : cursor.to<JsonObject>();
      JsonVariant next = obj[part];
      cursor = next.isNull() ? obj[part].to<JsonVariant>() : next;
    }
    from = dot + 1;
  }
  cursor.set(true);
}

static bool pathNumber(JsonDocument &doc, const String &path, float &out) {
  JsonVariantConst v = doc.as<JsonVariantConst>();
  int from = 0;
  while (from <= (int)path.length() && !v.isNull()) {
    int dot = path.indexOf('.', from);
    if (dot < 0) dot = path.length();
    const String part = path.substring(from, dot);
    const bool index = part.length() && strspn(part.c_str(), "0123456789") == part.length();
    v = index ? v[part.toInt()] : v[part];
    from = dot + 1;
  }
  if (v.is<float>()) { out = v.as<float>(); return true; }
  if (v.is<const char *>()) {
    char *end = nullptr;
    const char *text = v.as<const char *>();
    out = strtof(text, &end);
    return end != text;
  }
  return false;
}

/* A number out of a JSON answer, by a path like "data.0.price"; an empty path means the answer is the number. */
static bool faFetchValue(float &out) {
  Fetch f;
  f.holdFirst = !faFetchedAt;  // the first reading is the one someone is waiting for on the page
  const int code = f.begin(faSrcUrl);
  if (code != 200) { faError = f.error(code); return false; }
  String body;
  const int got = f.body(body, 32768);
  if (got == HTTPC_ERROR_STREAM_WRITE) { faError = "The answer is too big for the bar (over 32 KB)"; return false; }
  if (got < 0) { faError = f.error(got); return false; }
  if (!faPath.length()) {
    char *end = nullptr;
    out = strtof(body.c_str(), &end);
    if (end == body.c_str()) { faError = "The answer is not a number"; return false; }
    return true;
  }
  /* Only the paths are kept from the answer, so a large reply costs little memory. */
  JsonDocument filter;
  pathFilter(filter, faPath);
  if (faBase.length()) pathFilter(filter, faBase);
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) { faError = "The answer is not JSON"; return false; }
  float value;
  if (!pathNumber(doc, faPath, value)) { faError = "Nothing numeric at \"" + faPath + "\""; return false; }
  if (!faBase.length()) { out = value; return true; }
  float base;
  if (!pathNumber(doc, faBase, base) || base == 0) { faError = "Nothing to compare with at \"" + faBase + "\""; return false; }
  out = (value - base) / base * 100.0f;
  return true;
}

static void faRemove() {
  if (faId[0]) {
    JsonDocument gone;
    gone["id"] = faId;
    gone["remove"] = true;
    addonApply(gone.as<JsonVariantConst>(), nullptr);
  }
  faKind = FA_NONE;
  faId[0] = faName[0] = faAuthor[0] = 0;
  faDesc = faError = faShow = "";
}

/* One step of the file add-on: load it if needed, then keep it on the bar. */
static void faTick() {
  if (faReload) {
    faReload = false;   // before the address is read, so one stored meanwhile sets it again; put back below while this one still has to wait
    /* The library first: a kept file loads straight from flash, with nothing to fetch. */
    xSemaphoreTake(libLock, portMAX_DELAY);
    const int slot = libActive[0] ? libFind(libActive) : -1;
    const String url = slot >= 0 ? lib[slot].url : prefs.getString("faurl", "");
    xSemaphoreGive(libLock);
    const bool fetch = slot < 0 && url.length() && !faInline.length();
    /* Nothing is taken down until the new one can actually be fetched. */
    if (fetch && WiFi.status() != WL_CONNECTED) { faReload = true; faError = "Waiting for Wi-Fi to fetch it"; return; }
    /* A certificate is checked against today's date, which the bar only has once it has asked the internet: before that every HTTPS fetch fails with a certificate error (seen right after each restart, 0.6.3). */
    if (fetch && !timeKnown()) { faReload = true; faError = ""; return; }
    faRemove();
    faUrl = url;
    String text;
    if (slot >= 0) {
      text = libText(slot);
      if (!text.length()) { faError = "The bar could not read this add-on back from its flash: add it again"; faPick = false; return; }
    } else if (!faUrl.length()) {
      const size_t size = prefs.getBytesLength("fajson");
      if (size && size <= FA_MAX_UPLOAD) {
        std::unique_ptr<char[]> buf(new char[size + 1]);
        prefs.getBytes("fajson", buf.get(), size);
        buf[size] = 0;
        text = buf.get();
      }
    }
    /* The page already downloaded the file to show it in the store, and hands it over with its address: used as it is, it loads at once instead of
       the bar fetching it again from GitHub (11-12 s on the P2, most of it the secure connection juggling memory with the printer's). The address is
       still kept, and the bar fetches from it at its next start. */
    if (slot < 0 && faUrl.length() && faInline.length()) { text = faInline; faInline = ""; }
    else if (slot < 0 && faUrl.length()) {
      Fetch f;
      const int code = f.begin(faUrl);
      if (code != 200) {
        /* A first failure is retried quietly after a few seconds: a one-off
           "certificate verification failed" from GitHub cleared on the very
           next attempt (seen on the P2, 0.7.1), and was shown to the owner as
           a broken add-on for a minute. Only a second failure in a row is
           reported, with the largest free block at that moment, so the next
           one says whether memory was short. */
        faReload = true;
        if (++faFails < 2) { faRetryAt = millis() + 5000; return; }
        faError = "Fetching it: " + f.error(code) + " (largest free block " + String(ESP.getMaxAllocHeap() / 1024) + " KB). Trying again in a minute.";
        faRetryAt = millis() + 60000;
        return;
      }
      const int got = f.body(text, FA_MAX_FETCH);
      if (got == HTTPC_ERROR_STREAM_WRITE) { faError = "The add-on file is too big"; faPick = false; return; }
      if (got < 0) { faReload = true; faError = "Fetching it: " + f.error(got) + ". Trying again in a minute."; faRetryAt = millis() + 60000; return; }
      faFails = 0;
    }
    if (!text.length()) {
      /* Down as running, but its slot is gone (a crash report reached it): said, rather than the add-on just vanishing. */
      if (libActive[0]) faError = "This add-on is no longer kept on the bar: add it again from the store";
      faPick = false;
      return;
    }
    if (const char *bad = faLoad(text)) { faError = bad; faKind = FA_NONE; faPick = false; return; }
    faError = "";
    /* One fetched from its address, or kept the old way from before the library (0.7.17 and earlier): into the library now, so it is never fetched again and frees its room in the settings. One too big for a slot, or with every slot taken, carries on the old way. */
    if (slot < 0) {
      char id[16];
      if (!libSave(text, faUrl, id)) {
        strlcpy(libActive, id, sizeof libActive);
        prefs.putString("falib", libActive);
        prefs.remove("faurl");
        prefs.remove("fajson");
      }
    }
    if (faPick) {
      faPick = false;
      strlcpy(addonShow, faId, sizeof addonShow);
      saveConfig();
    }
  }
  const uint32_t now = millis();
  if (faKind == FA_STATIC && now - faPushedAt > 30000) {
    JsonDocument show;
    if (!deserializeJson(show, faShow)) addonApply(show.as<JsonVariantConst>(), nullptr);
    faPushedAt = now;
  } else if (faKind == FA_FRAMES && now - faPushedAt >= 1000u / faFps) {
    xSemaphoreTake(addonLock, portMAX_DELAY);
    if (Addon *slot = addonSlotFor(faId)) {
      Addon next = {};
      next.used = true;
      strlcpy(next.id, faId, sizeof next.id);
      strlcpy(next.name, faName, sizeof next.name);
      next.at = now;
      next.ttlMs = 10000;
      next.kind = ADDON_FRAME;
      memcpy(next.pixels, faFrames[faFrameAt], sizeof next.pixels);
      *slot = next;
    }
    xSemaphoreGive(addonLock);
    faFrameAt = (faFrameAt + 1) % faFrameCount;
    faPushedAt = now;
  } else if (faKind == FA_FILAMENT && now - faPushedAt >= 1000) {
    faPushedAt = now;
    const int tray = printerFresh() ? filLoadedTray() : -1;
    const FilamentTray *loaded = tray >= 0 ? filFind(tray) : nullptr;
    const FilamentTray *show = loaded ? loaded : filLast.tray != -1 ? &filLast : nullptr;
    if (!show) { faError = "Waiting for the printer to say which filament it has (Bambu printers with an AMS or external spool)"; return; }
    faError = "";
    /* An LED cannot shine black: a black or near-black filament is shown as a dim grey rather than as nothing. */
    uint32_t colour = show->colour;
    const uint8_t r = colour >> 16, g = colour >> 8, b = colour;
    if (max(r, max(g, b)) < 40) colour = 0x303030;
    xSemaphoreTake(addonLock, portMAX_DELAY);
    if (Addon *slot = addonSlotFor(faId)) {
      Addon next = {};
      next.used = true;
      strlcpy(next.id, faId, sizeof next.id);
      strlcpy(next.name, faName, sizeof next.name);
      next.at = now;
      next.ttlMs = 10000;
      next.kind = ADDON_LOOK;
      /* What is loaded now in the add-on's own effect; between prints, the last one used, breathing slowly. */
      next.look = loaded ? Look(colour, faFilEffect, faFilSpeed) : Look(colour, FX_BREATHE, 15);
      next.fill = 100;
      *slot = next;
    }
    xSemaphoreGive(addonLock);
  } else if (faKind == FA_PRINTER && now - faPushedAt >= 2000) {
    faPushedAt = now;
    const bool fresh = printerFresh();
    const bool printing = fresh && (!strcmp(prGcode, "RUNNING") || !strcmp(prGcode, "PAUSE") || !strcmp(prGcode, "PREPARE"));
    float fill = NAN;
    uint32_t colour = faColour;
    bool duringPrint = false;
    const char *why = "";
    if (!fresh) why = "Waiting for the bar's own link to the printer (Printer section)";
    else if (faReading == PRD_REMAIN) {
      const int tray = filLoadedTray();
      const FilamentTray *t = tray >= 0 ? filFind(tray) : nullptr;
      if (!t) why = "Waiting for the printer to say which spool is loaded";
      else if (t->remain < 0) why = "The loaded spool does not say how much is left: only Bambu spools with their tag report it";
      else {
        fill = t->remain;
        faValue = t->remain;
        if (!faColourSet) { colour = t->colour; if (max(colour >> 16 & 255, max(colour >> 8 & 255, colour & 255)) < 40) colour = 0x303030; }
      }
    } else if (faReading == PRD_HUMIDITY) {
      /* The AMS that feeds the loaded spool, or the first one heard. */
      const int tray = filLoadedTray();
      const int unit = tray < 0 || tray >= 254 ? -1 : tray >= 128 ? tray : tray / 4;
      const AmsUnit *u = nullptr;
      for (int i = 0; i < amsCount; i++) if (amsUnits[i].id == unit) u = &amsUnits[i];
      if (!u && amsCount) u = &amsUnits[0];
      if (!u || (u->raw < 0 && u->level < 1)) why = "Waiting for an AMS to report its humidity (Bambu printers with an AMS)";
      else {
        /* The meter is dampness: full is wet. The percentage where the AMS sends one, else its level (1 wet to 5 dry). */
        if (u->raw >= 0) { fill = constrain((u->raw - faMin) / (faMax - faMin) * 100, 0.0f, 100.0f); faValue = u->raw; faUnit = "% humidity"; }
        else { fill = (5 - u->level) * 25.0f; faValue = u->level; faUnit = "level (5 is driest)"; }
        if (!faColourSet) colour = fill < 40 ? 0x22C55E : fill < 70 ? 0xF59E0B : 0xEF4444;
      }
    } else if (faReading == PRD_TIMELEFT) {
      duringPrint = true;
      const int left = prTimeLeft;
      if (!printing || left < 0) { faTotal = 0; why = printing ? "Waiting for the printer's time estimate" : ""; }
      else {
        if (left > faTotal) faTotal = left;
        fill = faTotal > 0 ? left / faTotal * 100 : 100;
        faValue = left;
      }
    } else if (faReading == PRD_HEATING) {
      duringPrint = true;
      /* How far the nozzle and bed are from room temperature to their targets; the slower one decides. Shown only while something heats. */
      float part = 2, n = prNozzle, nt = prNozzleTarget, b = prBed, bt = prBedTarget;
      if (!isnan(n) && !isnan(nt) && nt > 40 && n < nt - 2) part = min(part, (n - 25) / (nt - 25));
      if (!isnan(b) && !isnan(bt) && bt > 30 && b < bt - 2) part = min(part, (b - 25) / (bt - 25));
      if (part < 2) { fill = constrain(part * 100, 0.0f, 100.0f); faValue = fill; }
    } else {
      duringPrint = true;
      const float c = prChamber;
      if (isnan(c) || c <= 0) why = "This printer does not report its chamber temperature";
      else if (printing) { fill = constrain((c - faMin) / (faMax - faMin) * 100, 0.0f, 100.0f); faValue = c; }
    }
    faError = why;
    if (isnan(fill)) return;
    xSemaphoreTake(addonLock, portMAX_DELAY);
    if (Addon *slot = addonSlotFor(faId)) {
      Addon next = {};
      next.used = true;
      strlcpy(next.id, faId, sizeof next.id);
      strlcpy(next.name, faName, sizeof next.name);
      next.at = now;
      next.ttlMs = 10000;   // goes as soon as the reading does: heating done, print over
      next.kind = ADDON_METERS;
      next.meterCount = 1;
      next.meterValue[0] = fill;
      next.meterColour[0] = colour;
      next.duringPrint = duringPrint;
      *slot = next;
    }
    xSemaphoreGive(addonLock);
  } else if (faKind == FA_MAINT && now - faPushedAt >= 2000) {
    faPushedAt = now;
    const float done = (printSeconds - maintFrom) / 3600.0f;
    faValue = done;
    faError = "";
    if (done < faMaintHours) return;   // nothing to say until it is due
    xSemaphoreTake(addonLock, portMAX_DELAY);
    if (Addon *slot = addonSlotFor(faId)) {
      Addon next = {};
      next.used = true;
      strlcpy(next.id, faId, sizeof next.id);
      strlcpy(next.name, faName, sizeof next.name);
      next.at = now;
      next.ttlMs = 10000;
      next.kind = ADDON_LOOK;
      next.look = Look(faColour, FX_BREATHE, 20);
      next.fill = 100;
      *slot = next;
    }
    xSemaphoreGive(addonLock);
  } else if (faKind == FA_COUNTDOWN && now - faPushedAt >= 1000) {
    faPushedAt = now;
    /* Its dates are local times, so nothing is shown until the clock and zone are known: a guess would be off by hours. */
    if (!timeKnown()) { faError = "Waiting for the clock"; return; }
    const time_t t = time(nullptr);
    const bool done = t >= faCdTo;
    if (done && t >= faCdTo + (time_t)faCdDoneHours * 3600) { faError = ""; return; }  // finished and celebrated: the bar goes back to the printer
    faError = "";
    xSemaphoreTake(addonLock, portMAX_DELAY);
    if (Addon *slot = addonSlotFor(faId)) {
      Addon next = {};
      next.used = true;
      strlcpy(next.id, faId, sizeof next.id);
      strlcpy(next.name, faName, sizeof next.name);
      next.at = now;
      next.ttlMs = 10000;
      if (done) {
        next.kind = ADDON_LOOK;
        next.look = Look(faColour, faCdDone, 50);
        next.fill = 100;
      } else {
        // What is left, as a meter that empties: full when it was set, empty at the moment it ends.
        next.kind = ADDON_METERS;
        next.meterCount = 1;
        next.meterValue[0] = constrain((float)(faCdTo - t) / (float)(faCdTo - faCdFrom) * 100.0f, 0.0f, 100.0f);
        next.meterColour[0] = faColour;
      }
      *slot = next;
    }
    xSemaphoreGive(addonLock);
  } else if (faKind == FA_SOURCE) {
    if ((!faFetchedAt || now - faFetchedAt >= faEveryMs) && (!faTriedAt || now - faTriedAt > 30000) && WiFi.status() == WL_CONNECTED && timeKnown()) {
      faTriedAt = now;
      float value;
      if (faFetchValue(value)) { faValue = value; faFetchedAt = now; faError = ""; }
    }
    /* Shown for as long as the reading is at most three fetches old (two minutes at least), by pushing it again every 15 s for that long. The slot's own lifetime cannot say this: it stops at an hour, and a meter read every half hour asked for an hour and a half and was refused every time (Air quality never appeared, 0.7.11). */
    if (!isnan(faValue) && now - faPushedAt > 15000 && now - faFetchedAt < max<uint32_t>(120000, faEveryMs * 3)) {
      JsonDocument show;
      show["id"] = faId;
      show["name"] = faName;
      show["ttl"] = 120;
      JsonObject meter = show["meters"].to<JsonArray>().add<JsonObject>();
      meter["value"] = constrain((faValue - faMin) / (faMax - faMin) * 100.0f, 0.0f, 100.0f);
      char colour[8];
      snprintf(colour, sizeof colour, "#%06lX", (unsigned long)faColour);
      meter["colour"] = colour;
      addonApply(show.as<JsonVariantConst>(), nullptr);
      faPushedAt = now;
    }
  }
}

static void onlineTask(void *) {
  uint32_t onlineSince = 0;
  uint32_t wantSince = 0;      // when the page's request for a check or an install was first seen
  uint32_t dailyTriedAt = 0;   // when the daily look was last tried, answered or not
  for (;;) {
    const bool online = WiFi.status() == WL_CONNECTED;
    if (!online) onlineSince = 0;
    else if (!onlineSince) onlineSince = millis();

    /* A request made before the clock is set waits for it (a few seconds after Wi-Fi), rather than failing on the certificate. Not without end: where the time cannot be had at all it says so after half a minute, and the add-ons below are kept going meanwhile. */
    if (!updWant) wantSince = 0;
    else if (!wantSince) wantSince = millis() | 1;
    const bool waitingForClock = updWant && online && !timeKnown();
    if (waitingForClock && millis() - wantSince > 30000) {
      updWant = 0;
      updMessage = "The bar has not been able to get the time, which it needs before it can trust GitHub's answer";
      updPhase = UPD_FAILED;
    }
    if (!waitingForClock) {
      if (updWant == 1) { updWant = 0; if (online) updCheck(); else { updMessage = "Not on Wi-Fi"; updPhase = UPD_FAILED; } }
      if (updWant == 2) { updWant = 0; if (online) updInstall(); else { updMessage = "Not on Wi-Fi"; updPhase = UPD_FAILED; } }
    }
    /* Once a day, a couple of minutes after joining Wi-Fi: only to say on the page that there is one. */
    if (updAuto && online && timeKnown() && onlineSince && millis() - onlineSince > 120000 && updPhase != UPD_DOWNLOADING
        && (!updCheckedAt || millis() - updCheckedAt > 86400000UL) && (!dailyTriedAt || millis() - dailyTriedAt > 3600000UL)) {
      /* When it was tried, kept apart from when GitHub last answered: a look that fails is tried again in an hour. With only the answer's time to go by, one failed look after a good one was repeated on every pass, each of them dropping the printer link to make room (0.7.11). */
      dailyTriedAt = millis() | 1;
      updCheck();
    }
    if (!faReload || (int32_t)(millis() - faRetryAt) >= 0) faTick();
    vTaskDelay(pdMS_TO_TICKS(faKind == FA_FRAMES ? 20 : 250));
  }
}

/* What the page shows about the file add-on and about updates. */
static void onlineToJson(JsonDocument &doc) {
  /* The file add-on, if there is one: where it came from and how it is doing. */
  JsonObject file = doc["addonFile"].to<JsonObject>();
  file["from"] = faUrl.length() ? "store" : libActive[0] || prefs.isKey("fajson") ? "upload" : "";
  file["url"] = faUrl;
  file["id"] = faId;
  file["name"] = faName;
  file["author"] = faAuthor;
  file["description"] = faDesc;
  file["kind"] = FA_NAMES[faKind];
  file["loading"] = (bool)faReload;
  file["error"] = faError;
  if ((faKind == FA_SOURCE || faKind == FA_PRINTER || faKind == FA_MAINT) && !isnan(faValue)) file["value"] = faValue;
  if (faKind == FA_PRINTER || faKind == FA_MAINT) file["unit"] = faUnit;
  if (faKind == FA_MAINT) { file["every"] = faMaintHours; file["task"] = faTask; file["due"] = !isnan(faValue) && faValue >= faMaintHours; }
  if (faKind == FA_COUNTDOWN && timeKnown()) file["endsIn"] = (long)(faCdTo - time(nullptr));
  /* Every add-on kept on the bar, the running one among them. */
  JsonArray kept = doc["library"].to<JsonArray>();
  if (libPart) {
    xSemaphoreTake(libLock, portMAX_DELAY);
    for (int i = 0; i < LIB_SLOTS; i++) {
      if (!lib[i].used) continue;
      JsonObject one = kept.add<JsonObject>();
      one["id"] = lib[i].id;
      one["name"] = lib[i].name;
      one["url"] = lib[i].url;
      one["active"] = !strcmp(lib[i].id, libActive);
    }
    xSemaphoreGive(libLock);
  }
  doc["librarySlots"] = libPart ? LIB_SLOTS : 0;

  /* Updates from GitHub. */
  JsonObject upd = doc["update"].to<JsonObject>();
  upd["phase"] = UPD_NAMES[updPhase];
  upd["latest"] = updLatest;
  upd["percent"] = updPercent;
  upd["message"] = updMessage;
  upd["page"] = updPageUrl;
  upd["auto"] = updAuto;
  upd["repo"] = UPDATE_REPO;
  if (updCheckedAt) upd["checkedAgo"] = (millis() - updCheckedAt) / 1000;
  else upd["checkedAgo"] = nullptr;
}

static void handleUpdateApi() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  if (!doc["auto"].isNull()) { updAuto = doc["auto"].as<bool>(); prefs.putBool("updauto", updAuto); }
  if (doc["check"].as<bool>()) updWant = 1;
  if (doc["install"].as<bool>()) {
    if (updPhase == UPD_DOWNLOADING) return sendResult(false, "Already installing");
    if (onTrial) return sendResult(false, TRIAL_NO_UPDATE);
    updWant = 2;
  }
  sendResult(true);
}

static void handleAddonFile() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  {
    /* The server only: an owner's own address can carry a key in its path or query. */
    String where = doc["url"] | "";
    const int hostAt = where.indexOf("://");
    if (hostAt >= 0) { where = where.substring(hostAt + 3); const int end = where.indexOf('/'); if (end >= 0) where = where.substring(0, end); }
    logLine("Add-on file: %s%s", doc["remove"].as<bool>() ? "removed" : !doc["url"].isNull() ? "from " : "uploaded", doc["remove"].as<bool>() || doc["url"].isNull() ? "" : where.c_str());
  }
  if (doc["maintenanceDone"].as<bool>()) {
    maintFrom = printSeconds;
    prefs.putUInt("printsec", printSeconds);
    printSecondsSaved = printSeconds;
    if (prefs.putUInt("maintfrom", maintFrom) != sizeof(uint32_t)) return sendResult(false, "Marked done, but the bar could not save it: it will be lost on a restart");
    logLine("Maintenance marked done");
    return sendResult(true, "Done: the count starts again");
  }
  if (doc["remove"].as<bool>()) {
    /* With an id, that kept add-on; without one, the one running (as before the library). */
    const char *id = doc["id"] | "";
    const bool running = !*id || !strcmp(id, libActive) || !strcmp(id, faId);
    if (*id) {
      const int slot = libFind(id);
      if (slot < 0 && !running) return sendResult(false, "The bar does not have that add-on");
      libRemove(slot);
    } else libRemove(libActive[0] ? libFind(libActive) : -1);
    if (!running) return sendResult(true, "Removed");
    libActive[0] = 0;
    prefs.remove("falib");
    prefs.remove("faurl");
    prefs.remove("fajson");
    if (faId[0] && strcmp(addonShow, faId) == 0) { addonShow[0] = 0; saveConfig(); }
    faReload = true;
    faRetryAt = millis();
    return sendResult(true, "Removed");
  }
  /* Switching to an add-on the bar already keeps: nothing is downloaded. */
  if (!doc["use"].isNull()) {
    const char *id = doc["use"] | "";
    if (libFind(id) < 0) return sendResult(false, "The bar does not have that add-on: add it from the store");
    strlcpy(libActive, id, sizeof libActive);
    prefs.putString("falib", libActive);
    prefs.remove("faurl");
    prefs.remove("fajson");
    faInline = "";
    faPick = true;
    faFails = 0;
    faReload = true;
    faRetryAt = millis();
    return sendResult(true, "Loading");
  }
  if (!doc["url"].isNull()) {
    const String url = doc["url"] | "";
    if (!url.startsWith("https://") || url.length() > 300) return sendResult(false, "The add-on's address must start with https://");
    /* The copy the page hands over is checked like an upload, before the add-on there now is let go. (One fetched by the bar itself is checked when it arrives.) */
    if (!doc["file"].isNull()) {
      String text;
      serializeJson(doc["file"], text);
      if (const char *bad = faCheck(text)) return sendResult(false, bad);
    }
    faInline = "";
    String text;
    if (!doc["file"].isNull()) serializeJson(doc["file"], text);
    char id[16];
    /* The copy the page handed over goes straight into the library. One too big for a slot, or sent without a copy, is fetched by the bar from its address, and kept once it has it. */
    if (text.length() && sizeof(LibHead) + url.length() + text.length() <= LIB_SECTOR && libPart) {
      if (const char *bad = libSave(text, url, id)) return sendResult(false, bad);
      strlcpy(libActive, id, sizeof libActive);
      prefs.putString("falib", libActive);
      prefs.remove("faurl");
    } else {
      libActive[0] = 0;
      prefs.remove("falib");
      prefs.putString("faurl", url);
      if (text.length() <= FA_MAX_FETCH) faInline = text;  // too big: the bar fetches it itself, as before
    }
    prefs.remove("fajson");
  } else if (!doc["file"].isNull()) {
    String text;
    serializeJson(doc["file"], text);
    if (text.length() > FA_MAX_UPLOAD) return sendResult(false, "The file is over 4,000 characters; host it and use its address instead");
    /* Checked in full before it is kept, so a broken file never replaces a working one. */
    if (const char *bad = faCheck(text)) return sendResult(false, bad);
    /* Into the library; a write that fails leaves the add-on that was running carrying on. */
    char id[16];
    if (const char *bad = libSave(text, "", id)) return sendResult(false, bad);
    strlcpy(libActive, id, sizeof libActive);
    prefs.putString("falib", libActive);
    prefs.remove("faurl");
    prefs.remove("fajson");
  } else {
    return sendResult(false, "Send url, file, use or remove");
  }
  faPick = true;
  faFails = 0;
  faReload = true;
  faRetryAt = millis();
  sendResult(true, "Loading");
}

/* -------------------------------------------------- Home Assistant -- */

/**
 * Home Assistant over MQTT, with discovery: point the bar at the broker
 * Home Assistant already uses (the Mosquitto add-on, usually) and a device
 * appears with no YAML. It carries:
 *  - a light: on/off, brightness, colour and effect. A colour or effect chosen
 *    there holds the bar until "Follow the printer" is picked again; off
 *    darkens it whatever else is going on.
 *  - sensors: what the bar is showing, the print's progress, Wi-Fi signal.
 *  - a Mode select (P2 only: Music needs its microphone).
 * And it listens on <base>/state/set for the same JSON as POST /api/state, so
 * an automation can drive the bar like any other controller.
 *
 * Plain MQTT on the LAN (port 1883 by default), user and password optional:
 * the same trust as the rest of this page. The password is never sent back.
 */
static bool haOn = false;
static String haHost, haUser, haPass, haPrefix = "homeassistant";
static uint16_t haPort = 1883;
static SemaphoreHandle_t haLock;               // the settings above, written by the page, read by the task
static volatile bool haRestart = false;
static volatile bool haConnected = false;
static volatile bool haResend = false;         // Home Assistant restarted: send discovery again
static char haError[48] = "";
static String haId, haBase;
static String haLivePrefix = "homeassistant";   // the prefix of the connection that is up; only the task touches it
static const char *HA_FOLLOW = "Follow the printer";

static void haLoad() {
  haOn = prefs.getBool("haon", false);
  haHost = prefs.getString("hahost", "");
  haPort = prefs.getUShort("haport", 1883);
  haUser = prefs.getString("hauser", "");
  haPass = prefs.getString("hapass", "");
  haPrefix = prefs.getString("haprefix", "homeassistant");
  while (haPrefix.endsWith("/")) haPrefix.remove(haPrefix.length() - 1);   // one saved before the page took them off
  if (!haPrefix.length()) haPrefix = "homeassistant";
  char id[20];
  snprintf(id, sizeof id, "panda_%06x", (uint32_t)(ESP.getEfuseMac() >> 24) & 0xFFFFFF);
  haId = id;
  haBase = String("panda/") + id;
}

static void haMessage(char *topicText, byte *payload, unsigned int length) {
  const String topic(topicText);
  if (topic == haLivePrefix + "/status") {
    if (length == 6 && !memcmp(payload, "online", 6)) haResend = true;
    return;
  }
  if (topic == haBase + "/mode/set") {
    String value; value.reserve(length);
    for (unsigned i = 0; i < length; i++) value += (char)payload[i];
    JsonDocument doc;
    doc["mode"] = value;
    if (!applyConfig(doc.as<JsonVariantConst>())) saveConfig();
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  if (topic == haBase + "/state/set") { applyPushedState(doc.as<JsonVariantConst>()); return; }
  if (topic != haBase + "/light/set") return;
  if (!doc["state"].isNull()) haOff = strcmp(doc["state"] | "ON", "OFF") == 0;
  if (!doc["brightness"].isNull()) { brightness = constrain(doc["brightness"].as<int>(), 0, 100); saveConfig(); }
  if (!doc["color"].isNull()) {
    haLook.colour = ((uint32_t)constrain(doc["color"]["r"] | 0, 0, 255) << 16) | ((uint32_t)constrain(doc["color"]["g"] | 0, 0, 255) << 8) | constrain(doc["color"]["b"] | 0, 0, 255);
    haManual = true;
  }
  if (!doc["effect"].isNull()) {
    const char *effect = doc["effect"] | "";
    if (!strcmp(effect, HA_FOLLOW)) haManual = false;
    else {
      const int fx = findName(FX_NAMES, FX_COUNT, effect);
      if (fx >= 0) { haLook.effect = fx; haManual = true; }
    }
  }
}

/* One entity's discovery record. */
static void haAnnounce(PubSubClient &mq, const char *component, const char *object, JsonDocument &config) {
  config["uniq_id"] = haId + "_" + object;
  config["obj_id"] = haId + "_" + object;
  config["avty_t"] = haBase + "/status";
  JsonObject dev = config["dev"].to<JsonObject>();
  dev["ids"].to<JsonArray>().add(haId);
  dev["name"] = BOARD_NAME;
  dev["mf"] = "BIQU hardware, custom firmware";
  dev["mdl"] = BOARD_NAME;
  dev["sw"] = FW_VERSION;
  dev["cu"] = "http://" + WiFi.localIP().toString() + "/";
  String text;
  serializeJson(config, text);
  mq.publish((haLivePrefix + "/" + component + "/" + haId + "/" + object + "/config").c_str(), text.c_str(), true);
}

static void haDiscovery(PubSubClient &mq) {
  {
    JsonDocument c;
    c["name"] = nullptr;   // the light is the device itself
    c["schema"] = "json";
    c["cmd_t"] = haBase + "/light/set";
    c["stat_t"] = haBase + "/light/state";
    c["brightness"] = true;
    c["brightness_scale"] = 100;
    c["supported_color_modes"].to<JsonArray>().add("rgb");
    c["effect"] = true;
    JsonArray list = c["effect_list"].to<JsonArray>();
    list.add(HA_FOLLOW);
    for (int i = 0; i < FX_COUNT; i++) list.add(FX_NAMES[i]);
    haAnnounce(mq, "light", "light", c);
  }
  {
    JsonDocument c;
    c["name"] = "Showing";
    c["stat_t"] = haBase + "/info";
    c["val_tpl"] = "{{ value_json.state }}";
    c["ic"] = "mdi:printer-3d";
    haAnnounce(mq, "sensor", "showing", c);
  }
  {
    JsonDocument c;
    c["name"] = "Progress";
    c["stat_t"] = haBase + "/info";
    c["val_tpl"] = "{{ value_json.progress }}";
    c["unit_of_meas"] = "%";
    c["stat_cla"] = "measurement";
    c["ic"] = "mdi:progress-clock";
    haAnnounce(mq, "sensor", "progress", c);
  }
  {
    JsonDocument c;
    c["name"] = "Wi-Fi signal";
    c["stat_t"] = haBase + "/info";
    c["val_tpl"] = "{{ value_json.rssi }}";
    c["unit_of_meas"] = "dBm";
    c["dev_cla"] = "signal_strength";
    c["ent_cat"] = "diagnostic";
    haAnnounce(mq, "sensor", "rssi", c);
  }
#if defined(BOARD_P2)
  {
    JsonDocument c;
    c["name"] = "Mode";
    c["cmd_t"] = haBase + "/mode/set";
    c["stat_t"] = haBase + "/info";
    c["val_tpl"] = "{{ value_json.mode }}";
    JsonArray options = c["options"].to<JsonArray>();
    for (int i = 0; i < MODE_COUNT; i++) options.add(MODE_NAMES[i]);
    haAnnounce(mq, "select", "mode", c);
  }
#endif
}

static String haLightState() {
  float fill;
  Source source;
  const State state = shown(fill, source);
  const uint32_t colour = haManual ? haLook.colour : looks[state].colour;
  JsonDocument d;
  d["state"] = haOff ? "OFF" : "ON";
  d["brightness"] = brightness;
  d["color_mode"] = "rgb";
  d["color"]["r"] = (colour >> 16) & 255;
  d["color"]["g"] = (colour >> 8) & 255;
  d["color"]["b"] = colour & 255;
  d["effect"] = haManual ? FX_NAMES[haLook.effect] : HA_FOLLOW;
  String text;
  serializeJson(d, text);
  return text;
}

static String haInfo() {
  float fill;
  Source source;
  const State state = shown(fill, source);
  JsonDocument d;
  d["state"] = haOff ? "light off" : haManual && source != SRC_PREVIEW ? "set by Home Assistant" : mode == MODE_MUSIC && source != SRC_PREVIEW ? "music" : ST_NAMES[state];
  d["progress"] = (int)lroundf(fill);
  /* Rounded to 5 dB, so a signal that wobbles by one does not send a message every second. */
  d["rssi"] = (int)(lroundf(WiFi.RSSI() / 5.0f) * 5);
  d["mode"] = MODE_NAMES[mode];
  String text;
  serializeJson(d, text);
  return text;
}

static void haWait(uint32_t ms) {
  const uint32_t from = millis();
  while (!haRestart && millis() - from < ms) vTaskDelay(pdMS_TO_TICKS(200));
}

static void haTask(void *) {
  WiFiClient net;
  PubSubClient mq(net);
  /* Discovery records with the effect list run past a kilobyte; the library's default buffer is 256 bytes and drops anything larger without a word. */
  mq.setBufferSize(1600);
  mq.setCallback(haMessage);
  mq.setKeepAlive(30);
  mq.setSocketTimeout(5);
  static String address;   // the library keeps the pointer, not a copy
  String lastLight, lastInfo;
  uint32_t sentAt = 0;
  for (;;) {
    if (haRestart) {
      haRestart = false;
      if (mq.connected()) { mq.publish((haBase + "/status").c_str(), "offline", true); mq.disconnect(); }
      haConnected = false;
    }
    xSemaphoreTake(haLock, portMAX_DELAY);
    const bool on = haOn;
    const String host = haHost, user = haUser, pass = haPass;
    const uint16_t port = haPort;
    if (!mq.connected()) haLivePrefix = haPrefix;
    xSemaphoreGive(haLock);
    if (!on || !host.length()) { haConnected = false; haError[0] = 0; haWait(1000); continue; }
    if (prPaused) { if (mq.connected()) { mq.publish((haBase + "/status").c_str(), "offline", true); mq.disconnect(); } haConnected = false; strlcpy(haError, "Paused while a firmware update downloads", sizeof haError); haWait(1000); continue; }
    if (WiFi.status() != WL_CONNECTED) { haConnected = false; strlcpy(haError, "Waiting for Wi-Fi", sizeof haError); haWait(2000); continue; }

    if (!mq.connected()) {
      haConnected = false;
      address = host;
      if (endsWithLocal(host)) {
        IPAddress found = MDNS.queryHost(host.substring(0, host.length() - 6), 2000);
        if (found == IPAddress()) { strlcpy(haError, "Could not find that .local name", sizeof haError); haWait(15000); continue; }
        address = found.toString();
      }
      mq.setServer(address.c_str(), port);
      const String will = haBase + "/status";
      const bool ok = mq.connect(haId.c_str(), user.length() ? user.c_str() : nullptr, user.length() ? pass.c_str() : nullptr, will.c_str(), 0, true, "offline");
      if (!ok) {
        const int why = mq.state();
        strlcpy(haError, why == 4 ? "Wrong user name or password" : why == 5 ? "The broker refused the sign-in" : why == -2 ? "Broker not reachable" : "The broker did not answer", sizeof haError);
        haWait(15000);
        continue;
      }
      mq.publish(will.c_str(), "online", true);
      haDiscovery(mq);
      mq.subscribe((haBase + "/light/set").c_str());
      mq.subscribe((haBase + "/state/set").c_str());
      mq.subscribe((haBase + "/mode/set").c_str());
      mq.subscribe((haLivePrefix + "/status").c_str());
      lastLight = ""; lastInfo = "";
      haError[0] = 0;
      haConnected = true;
    }
    mq.loop();
    if (haResend) { haResend = false; haDiscovery(mq); lastLight = ""; lastInfo = ""; }
    if (millis() - sentAt >= 1000) {
      sentAt = millis();
      const String light = haLightState(), info = haInfo();
      if (light != lastLight) { mq.publish((haBase + "/light/state").c_str(), light.c_str(), true); lastLight = light; }
      if (info != lastInfo) { mq.publish((haBase + "/info").c_str(), info.c_str(), true); lastInfo = info; }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

/* Started only once Home Assistant is turned on: its stack and the MQTT buffer
   are memory a second TLS session (the add-ons' HTTPS beside the printer link)
   needs, and with them reserved anyway an add-on fetch failed "SSL - Memory
   allocation failed" (measured on the P2, 0.7.0 test build). */
static bool haStarted = false;
static void haStart() {
  if (haStarted) return;
  haStarted = xTaskCreate(haTask, "ha", 6144, nullptr, 1, nullptr) == pdPASS;
  if (!haStarted) strlcpy(haError, "No memory to start it; save again or restart", sizeof haError);
}

static void haToJson(JsonDocument &doc) {
  JsonObject ha = doc["homeAssistant"].to<JsonObject>();
  xSemaphoreTake(haLock, portMAX_DELAY);
  ha["on"] = haOn;
  ha["host"] = haHost;
  ha["port"] = haPort;
  ha["user"] = haUser;
  ha["hasPassword"] = haPass.length() > 0;
  ha["prefix"] = haPrefix;
  xSemaphoreGive(haLock);
  ha["connected"] = (bool)haConnected;
  ha["error"] = haError;
  ha["topic"] = haBase;
  ha["manual"] = (bool)haManual;
}

/* {"on":true,"host":"192.168.1.10","port":1883,"user":"panda","password":"…","prefix":"homeassistant"}; anything left out is kept. */
static void handleHa() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  logLine("Home Assistant settings changed");
  xSemaphoreTake(haLock, portMAX_DELAY);
  bool on = haOn; String host = haHost, user = haUser, pass = haPass, prefix = haPrefix; uint16_t port = haPort;
  xSemaphoreGive(haLock);
  if (!doc["on"].isNull()) on = doc["on"].as<bool>();
  if (!doc["host"].isNull()) { host = doc["host"] | ""; host.trim(); }
  if (!doc["user"].isNull()) { user = doc["user"] | ""; user.trim(); }
  if (!doc["password"].isNull()) pass = doc["password"] | "";
  if (!doc["prefix"].isNull()) {
    prefix = doc["prefix"] | "";
    prefix.trim();
    while (prefix.endsWith("/")) prefix.remove(prefix.length() - 1);   // "homeassistant/" would put an empty level in every topic, which Home Assistant ignores
  }
  if (!doc["port"].isNull()) {
    const int p = doc["port"].as<int>();
    if (p < 1 || p > 65535) return sendResult(false, "The port is 1-65535");
    port = p;
  }
  if (host.length() > 63 || host.indexOf(' ') >= 0 || host.indexOf('/') >= 0 || host.indexOf(':') >= 0)
    return sendResult(false, "Give the broker's address, e.g. 192.168.1.10 or homeassistant.local, without mqtt:// or a port");
  if (on && !host.length()) return sendResult(false, "Give the broker's address first");
  if (user.length() > 64 || pass.length() > 64) return sendResult(false, "User name and password are up to 64 characters");
  if (!prefix.length() || prefix.length() > 40 || prefix.indexOf('#') >= 0 || prefix.indexOf('+') >= 0 || prefix.startsWith("/") || prefix.indexOf("//") >= 0) return sendResult(false, "The discovery prefix is usually homeassistant");
  /* Each write checked, as the main settings are: a full store took them without a word, and they were gone at the next restart. putString answers the length it wrote, which is 0 for an empty one whether it worked or not. */
  bool saved = prefs.putBool("haon", on) && prefs.putUShort("haport", port);
  saved = prefs.putString("hahost", host) == host.length() && saved;
  saved = prefs.putString("hauser", user) == user.length() && saved;
  saved = prefs.putString("hapass", pass) == pass.length() && saved;
  saved = prefs.putString("haprefix", prefix) == prefix.length() && saved;
  xSemaphoreTake(haLock, portMAX_DELAY);
  haOn = on; haHost = host; haUser = user; haPass = pass; haPrefix = prefix; haPort = port;
  xSemaphoreGive(haLock);
  haRestart = true;
  if (on) haStart();
  if (!saved) return sendResult(false, "Changed, but the bar could not save it: it will be lost on a restart");
  sendResult(true, on ? "Saved. Connecting to the broker." : "Saved. Home Assistant is off.");
}

/* ------------------------------------------------------------ main -- */

void setup() {
  Serial.begin(115200);
  logBegin();
  {
    const esp_partition_t *running = esp_ota_get_running_partition();
    logLine("---- Start %lu: Panda State %s (%s), %s, slot %s, free memory %u", (unsigned long)logStore.starts, BUILD_ID, BOARD_ID,
            resetText(esp_reset_reason()), running ? running->label : "?", (unsigned)ESP.getFreeHeap());
  }
  prefs.begin("panda", false);
  loadConfig();
  outBegin();

  printSeconds = printSecondsSaved = prefs.getUInt("printsec", 0);
  maintFrom = prefs.getUInt("maintfrom", 0);
  const bool markedGood = imageMarkedGood();
  onTrial = !markedGood && prefs.getString("okbuild", "") != BUILD_ID;
  /* A build that proved itself before but is not down as good in the boot record (it was switched back to) is put down again: a bootloader that rolls back would otherwise drop it at its next start. */
  if (!onTrial && !markedGood && runningIsBootSlot()) esp_ota_mark_app_valid_cancel_rollback();
  if (onTrial) {
    /* A build on trial that has just fallen over has failed it. Waiting out the two minutes again would be a loop wherever the fault comes sooner than that, with only the bootloader's rollback to break it, and whether stock's bootloader has one is not known. */
    const esp_reset_reason_t why = esp_reset_reason();
    if (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT) {
      logLine("Crashed while on trial");
#if defined(BOARD_P2)
      /* Except once, where the microphone's start on this build was still unconfirmed: that is what its guard, further down, is for. It leaves the microphone off this time, and the build goes on with its trial. A second fall is the build's own. The excuse is written down before it is used, and only used if the note took, so it cannot be had twice. */
      const bool blameMic = prefs.getString("audiotry", "") == BUILD_ID && prefs.getString("micblame", "") != BUILD_ID;
      if (blameMic) prefs.putString("micblame", BUILD_ID);
      if (!blameMic || prefs.getString("micblame", "") != BUILD_ID) failTrial();
#else
      failTrial();
#endif
    }
  }
  startWifi();
  if (onTrial && !wifiSsid.length()) trialAllow = TRIAL_SETUP_MS;
  if (onTrial) logLine("On trial: %s", wifiSsid.length() ? "has 2 minutes to join Wi-Fi" : "no Wi-Fi details, setup hotspot up for 10 minutes");
  if (!wifiSsid.length()) logLine("Wi-Fi: no details saved here or by stock");
  applyTimeZone();

  prLock = xSemaphoreCreateMutex();
  addonLock = xSemaphoreCreateMutex();
  libBegin();
  strlcpy(libActive, prefs.getString("falib", "").c_str(), sizeof libActive);
  cfgLock = xSemaphoreCreateMutex();
  updAuto = prefs.getBool("updauto", true);
  if (prefs.isKey("filcol")) { filLast.tray = -2; filLast.colour = prefs.getUInt("filcol", 0); strlcpy(filLast.type, prefs.getString("filtype", "").c_str(), sizeof filLast.type); }
  if (prefs.getBool("pset", false)) {
    prIp = prefs.getString("pip", "");
    prSerial = prefs.getString("psn", "");
    prCode = prefs.getString("pcode", "");
    prKind = prefs.getUChar("ptype", PR_BAMBU);
    prPort = prefs.getUShort("pport", 7125);
  } else {
    readStockPairing(prIp, prSerial, prCode);
  }
  /* Its own task: a TLS handshake blocks for seconds, and the LEDs and the page must not freeze while it does. */
  xTaskCreate(printerTask, "printer", 12288, nullptr, 1, nullptr);
  /* The microphone runs all the time, not only in Music mode: it is a few percent of the processor, and the page can then show the level for setting the sensitivity. */
#if defined(BOARD_P2)
  /* An attempt that never got as far as running only counts against the microphone if the chip then fell over. The note alone could not tell that from a restart in the first 15 s, so a plug pulled and put back, or new Wi-Fi details saved straight away, left the microphone off until the next build (0.7.11). */
  const esp_reset_reason_t why = esp_reset_reason();
  const bool fellOver = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT || why == ESP_RST_BROWNOUT;
  const bool tried = prefs.getString("audiotry", "") == BUILD_ID;
  if (tried && fellOver) {
    audioBlocked = true;
  } else {
    if (!tried) prefs.putString("audiotry", BUILD_ID);
    xTaskCreate(audioTask, "audio", 6144, nullptr, 1, nullptr);
  }
#endif
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  /* GitHub and file add-ons. Its own task for the same reason as the printer's: HTTPS blocks for seconds. */
  xTaskCreate(onlineTask, "online", 14336, nullptr, 1, nullptr);
  haLock = xSemaphoreCreateMutex();
  haLoad();
  if (haOn) haStart();

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/state", HTTP_POST, handleState);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/preview", HTTP_POST, handlePreview);
  server.on("/api/addon", HTTP_POST, handleAddon);
  server.on("/api", HTTP_GET, []() { server.send_P(200, "application/json", API_DOC); });
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/printer", HTTP_POST, handlePrinter);
  server.on("/api/slot", HTTP_POST, handleSlot);
  server.on("/api/otherslot.bin", HTTP_GET, handleOtherSlotDownload);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/factory", HTTP_POST, handleFactory);
  /* Just the 25 colours, for the page's live picture: a few hundred bytes, so it can be asked five times a second where the full status is asked once. */
  server.on("/api/frame", HTTP_GET, []() {
    char hex[LED_COUNT * 6 + 1];
    for (int i = 0; i < LED_COUNT; i++) snprintf(hex + i * 6, 7, "%02x%02x%02x", frame[i][0], frame[i][1], frame[i][2]);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain", hex);
  });
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/addonfile", HTTP_POST, handleAddonFile);
  server.on("/api/ap", HTTP_POST, handleAp);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/update", HTTP_POST, handleUpdateApi);
  server.on("/api/ha", HTTP_POST, handleHa);
  server.on("/api/hostname", HTTP_POST, handleHostName);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateChunk);
  server.onNotFound([]() { sendResult(false, "Not found"); });
  server.begin();
  {
    const String saved = prefs.getString("host", "");
    if (saved.length() && hostNameValid(saved.c_str())) strlcpy(hostName, saved.c_str(), sizeof hostName);
  }
  MDNS.begin(hostName);
  mdns_instance_name_set(hostName);
  MDNS.addService("http", "tcp", 80);
}

/* The other names (stock's, and "pandastatus") are delegated hosts, which
   mDNS answers with a fixed address, so they are set again whenever the
   bar's address changes (DHCP, another network) or the owner renames it. */
static void keepOtherNames(bool force = false) {
  static uint32_t had = 0;
  static char added[2][33] = {"", ""};
  const IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress();
  if ((uint32_t)ip == had && !force) return;
  for (auto &name : added) if (name[0]) { mdns_delegate_hostname_remove(name); name[0] = 0; }
  had = (uint32_t)ip;
  if (!had) return;
  mdns_ip_addr_t addr = {};
  addr.addr.type = ESP_IPADDR_TYPE_V4;
  addr.addr.u_addr.ip4.addr = had;
  addr.next = nullptr;
  const char *others[2] = {STOCK_HOST, "pandastatus"};
  for (int i = 0; i < 2; i++) {
    // mDNS names ignore case, so the owner's own name standing in for one of these is not added twice.
    if (strcasecmp(others[i], hostName) == 0) continue;
    if (mdns_delegate_hostname_add(others[i], &addr) == ESP_OK) strlcpy(added[i], others[i], sizeof added[i]);
  }
}

/* The owner's own .local name. Empty puts back the stock name. Applied at
   once: mDNS takes a new host name while running, and the old names keep
   answering, so a page open on any of them is not stranded. */
static void handleHostName() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  String name = doc["name"] | "";
  name.trim();
  if (endsWithLocal(name)) name.remove(name.length() - 6);
  if (name.length() && !hostNameValid(name.c_str()))
    return sendResult(false, "A name is 1-32 letters, digits and hyphens, not starting or ending with a hyphen");
  bool saved;
  if (name.length()) saved = prefs.putString("host", name) == name.length();
  else { prefs.remove("host"); saved = !prefs.isKey("host"); }
  strlcpy(hostName, name.length() ? name.c_str() : STOCK_HOST, sizeof hostName);
  mdns_hostname_set(hostName);
  mdns_instance_name_set(hostName);
  keepOtherNames(true);
  const String now = String("Now at http://") + hostName + ".local";
  if (!saved) return sendResult(false, (now + ", but the bar could not save the name: it goes back at a restart").c_str());
  sendResult(true, now.c_str());
}

/* Changes in what the other tasks report, written to the log once a second from here, so that only this task writes it. The same fields the page shows; a line only when one changes. */
static void logWatch() {
  static bool timedOutWas = false;
  const uint8_t told = stateNow;
  const bool timedOut = told < ST_OFF && stateOverLimit((State)told);
  if (timedOut != timedOutWas) {
    if (timedOut) logLine("%s shown for %u min: %s until it changes", ST_NAMES[told], (unsigned)looks[told].limit, told == ST_IDLE || told == ST_OFFLINE ? "dark" : "idle look");
    timedOutWas = timedOut;
  }
  static int wifiWas = -1;
  const int wifi = WiFi.status() == WL_CONNECTED;
  if (wifi != wifiWas) {
    if (wifi) logLine("Wi-Fi: joined, %s, %d dBm", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    else if (wifiWas == 1) logLine("Wi-Fi: lost");
    wifiWas = wifi;
  }
  static int apWas = 0;
  if ((int)apUp != apWas) { logLine("Setup hotspot: %s", apUp ? "on" : "off"); apWas = apUp; }

  static char prErrorWas[sizeof prError] = "";
  static int prLinkedWas = 0;
  if ((int)prConnected != prLinkedWas) { logLine("Printer: %s", prConnected ? "linked" : "not linked"); prLinkedWas = prConnected; }
  char now[sizeof prError];
  strlcpy(now, prError, sizeof now);
  if (strcmp(now, prErrorWas)) { if (now[0]) logLine("Printer: %s", now); strlcpy(prErrorWas, now, sizeof prErrorWas); }
  static char gcodeWas[sizeof prGcode] = "";
  char gcode[sizeof prGcode];
  strlcpy(gcode, prGcode, sizeof gcode);
  if (gcode[0] && strcmp(gcode, gcodeWas)) { logLine("Printer says %s", gcode); strlcpy(gcodeWas, gcode, sizeof gcodeWas); }

  static int haWas = 0;
  if ((int)haConnected != haWas) { logLine("Home Assistant: %s", haConnected ? "connected" : "not connected"); haWas = haConnected; }
  static char haErrorWas[sizeof haError] = "";
  char haNow[sizeof haError];
  strlcpy(haNow, haError, sizeof haNow);
  if (strcmp(haNow, haErrorWas)) { if (haNow[0]) logLine("Home Assistant: %s", haNow); strlcpy(haErrorWas, haNow, sizeof haErrorWas); }

  static String faErrorWas;
  const String faNow = faError;
  if (faNow != faErrorWas) { if (faNow.length()) logLine("Add-on: %s", faNow.c_str()); faErrorWas = faNow; }

  static int updWas = -1;
  if (updPhase != updWas) {
    updWas = updPhase;
    if (updPhase == UPD_FAILED) logLine("Update: failed: %s", updMessage.c_str());
    else if (updPhase == UPD_AVAILABLE) logLine("Update: %s is available", updLatest);
    else if (updPhase == UPD_DOWNLOADING) logLine("Update: downloading %s", updLatest);
    else if (updPhase == UPD_DONE) logLine("Update: installed, restarting");
  }

  /* Memory running low is behind most of the failures the secure connections have shown. */
  static uint32_t lowestLogged = UINT32_MAX;
  const uint32_t lowest = ESP.getMinFreeHeap();
  if (lowest + 4096 < lowestLogged) { logLine("Memory: lowest free so far %u bytes", (unsigned)lowest); lowestLogged = lowest; }
}

/* A report to attach to a GitHub issue: what the bar is and what it is doing, its settings, and the log. Nothing secret, as above. */
static void handleLog() {
  String out;
  out.reserve(LOG_SIZE + 3072);
  out += "Panda State report\n";
  out += String("Firmware: ") + BUILD_ID + " for the " BOARD_NAME " (" BOARD_ID ")\n";
  const esp_partition_t *running = esp_ota_get_running_partition();
  out += String("Slot: ") + (running ? running->label : "?") + (onTrial ? ", on trial" : "") + "\n";
  const uint32_t up = millis() / 1000;
  out += String("Up: ") + (up / 86400) + " d " + (up / 3600 % 24) + " h " + (up / 60 % 60) + " min; last start: " + resetText(esp_reset_reason()) + "; starts in this log: " + logStore.starts + "\n";
  out += String("Memory: free ") + ESP.getFreeHeap() + ", lowest " + ESP.getMinFreeHeap() + ", largest block " + ESP.getMaxAllocHeap() + "\n";
  out += String("Wi-Fi: ") + (WiFi.status() == WL_CONNECTED ? String("joined, ") + WiFi.RSSI() + " dBm" : String("not joined")) + (apUp ? "; setup hotspot on" : "") + "\n";
  out += String("Printer: ") + (prKind == PR_KLIPPER ? "Klipper" : "Bambu Lab") + (prConnected ? ", linked" : ", not linked") + (prGcode[0] ? String(", says ") + prGcode : String("")) + (prError[0] ? String(", ") + prError : String("")) + "\n";
  out += String("Home Assistant: ") + (haOn ? (haConnected ? "connected" : "not connected") : "off") + (haError[0] ? String(", ") + haError : String("")) + "\n";
  out += String("Add-on file: ") + FA_NAMES[faKind] + (faId[0] ? String(" ") + faId : String("")) + (faError.length() ? String(", ") + faError : String("")) + "\n";
  out += String("Update check: ") + UPD_NAMES[updPhase] + (updMessage.length() ? String(", ") + updMessage : String("")) + "\n";
  {
    JsonDocument doc;
    configToJson(doc);
    out += "Settings: ";
    serializeJson(doc, out);
    out += "\n";
  }
  out += "\n---- What happened, oldest first (kept across restarts, not across a power cut)\n";
  logAppendTo(out);
  server.sendHeader("Content-Disposition", "attachment; filename=\"panda-state-report.txt\"");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", out);
}

void loop() {
  server.handleClient();
  static uint32_t lastFrameAt = 0;
  /* Slower while an update is being written: the LED signal is bit-timed and shares the core with the flash writes, so a frame now and then may come out wrong. At this rate that is a flicker on a progress bar, nothing worse. */
  if (millis() - lastFrameAt >= (Update.isRunning() ? 150u : 25u)) {
    lastFrameAt = millis();
    render();
  }
  pollButton();
  static uint32_t lastWatchAt = 0;
  if (millis() - lastWatchAt >= 1000) {
    lastWatchAt = millis();
    trackState();
    logWatch();
    watchTrial();
    keepOtherNames();
    trackPrinter();
    /* The filament in use, remembered (and kept across restarts) so it can still be shown once the printer has unloaded it. */
    if (printerFresh()) {
      const int tray = filLoadedTray();
      const FilamentTray *found = tray >= 0 ? filFind(tray) : nullptr;
      if (found && (found->colour != filLast.colour || strcmp(found->type, filLast.type))) {
        filLast = *found;
        prefs.putUInt("filcol", filLast.colour);
        prefs.putString("filtype", filLast.type);
      } else if (found) filLast.tray = found->tray;
    }
    /* Ten seconds of the microphone running without taking the chip down is proof enough to stop guarding it. */
    if (audioRunning && !audioConfirmed && millis() > 15000) { prefs.remove("audiotry"); audioConfirmed = true; }
  }
}
