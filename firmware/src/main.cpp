/**
 * Custom firmware for the BIQU Panda Status P2, the light bar for Bambu Lab
 * P2S and X2D printers.
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
#include <Preferences.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <nvs.h>
#include <esp_ota_ops.h>
#include "page.h"

/* What the firmware calls itself on its page and in /api/status. Plain and generic, so it says what it is on anyone's network. */
#define FW_NAME "panda"
#define FW_VERSION "0.5.2"
static const char *BUILD_ID = FW_VERSION " " __DATE__ " " __TIME__;

/* Measured, not assumed: see HARDWARE.md. */
static const uint8_t LED_PIN = 5;
static const uint8_t LED_COUNT = 25;
/* The ES8311 audio chip (the microphone) and the button, as the stock firmware's board file sets them. */
static const uint8_t PIN_SDA = 2, PIN_SCL = 3;
static const uint8_t PIN_MCLK = 0, PIN_BCK = 1, PIN_WS = 4, PIN_DIN = 10;   // stock also names DOUT 7; nothing audible is on it
static const uint8_t PIN_BUTTON = 9;   // to ground when pressed; stock reads it with the pull-up on

/* Sent as raw bytes in wire order; the colour order is a setting because it
   has not been confirmed on the real LEDs yet (GRB is only the usual one). */
static Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);
static WebServer server(80);
static Preferences prefs;

/* ------------------------------------------------------------- looks -- */

/* The first six are the bar's own. The rest are lamp effects in the spirit of WLED's, redrawn for 25 LEDs. They take two colours: the state's own and a second one. */
enum Effect : uint8_t {
  FX_SOLID, FX_BREATHE, FX_FILL, FX_FLASH, FX_CHASE, FX_RAINBOW,
  FX_GRADIENT, FX_WAVES, FX_SCROLL, FX_THEATRE, FX_SCANNER, FX_METEOR, FX_FIRE, FX_TWINKLE, FX_SPARKLE, FX_RAIN, FX_AURORA,
  FX_COUNT
};
static const char *FX_NAMES[FX_COUNT] = {
  "solid", "breathe", "fill", "flash", "chase", "rainbow",
  "gradient", "waves", "scroll", "theatre", "scanner", "meteor", "fire", "twinkle", "sparkle", "rain", "aurora",
};

enum State : uint8_t { ST_IDLE, ST_PRINTING, ST_PAUSED, ST_FINISHED, ST_COOLING, ST_READY, ST_ERROR, ST_OFFLINE, ST_OFF, ST_COUNT };
static const char *ST_NAMES[ST_COUNT] = {"idle", "printing", "paused", "finished", "cooling", "ready", "error", "offline", "off"};

/* colour2 is the second colour the two-colour effects use. AUTO means "work one out from the first", so a look set before 0.5.0 (or with only one colour given) still has a sensible partner. */
static const uint32_t AUTO = 0xFFFFFFFF;
/* A constructor rather than a default on the member: this toolchain's C++11 will not brace-initialise a struct that has one. */
struct Look {
  uint32_t colour; uint8_t effect; uint8_t speed; uint32_t colour2;
  Look(uint32_t c = 0, uint8_t e = 0, uint8_t s = 50, uint32_t c2 = AUTO) : colour(c), effect(e), speed(s), colour2(c2) {}
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
  /* off      */ {0x000000, FX_SOLID, 50},
};

static uint8_t brightness = 100;     // 0-100, the same scale the stock bar uses
static char order[4] = "GRB";
static bool reversed = false;
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
  if (strlen(text) < 6) return false;
  char hex[7];
  memcpy(hex, text, 6);
  hex[6] = 0;
  char *end = nullptr;
  uint32_t value = strtoul(hex, &end, 16);
  if (end != hex + 6) return false;
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
static const char *applyExtraConfig(JsonVariantConst doc);

static void configToJson(JsonDocument &doc) {
  extraConfigToJson(doc);
  doc["brightness"] = brightness;
  doc["order"] = order;
  doc["reversed"] = reversed;
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
  }
}

/* Applies whatever is present and valid, leaves the rest alone. Returns an error message or nullptr. */
static const char *applyConfig(JsonVariantConst doc) {
  if (const char *error = applyExtraConfig(doc)) return error;
  if (!doc["brightness"].isNull()) {
    int value = doc["brightness"].as<int>();
    if (value < 0 || value > 100) return "Brightness is 0-100";
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
  if (!doc["cool"]["minutes"].isNull()) {
    int value = doc["cool"]["minutes"].as<int>();
    if (value < 0 || value > 120) return "Cool-down is 0-120 minutes";
    coolMinutes = value;
  }
  if (!doc["cool"]["target"].isNull()) {
    int value = doc["cool"]["target"].as<int>();
    if (value < 20 || value > 60) return "Bed target is 20-60";
    coolTarget = value;
  }
  if (!doc["mode"].isNull()) {
    int at = findName(MODE_NAMES, MODE_COUNT, doc["mode"].as<const char *>());
    if (at < 0) return "Mode is status or music";
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
    if (!one["colour2"].isNull()) {
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
      int speed = one["speed"].as<int>();
      if (speed < 1 || speed > 100) return "Speed is 1-100";
      looks[at].speed = speed;
    }
  }
  return nullptr;
}

static void saveConfig() {
  JsonDocument doc;
  configToJson(doc);
  String text;
  serializeJson(doc, text);
  prefs.putString("cfg", text);
}

static void loadConfig() {
  String text = prefs.getString("cfg", "");
  if (!text.length()) return;
  JsonDocument doc;
  if (deserializeJson(doc, text)) return;
  applyConfig(doc.as<JsonVariantConst>());
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
static SemaphoreHandle_t prLock;              // the three strings above are read by the link's task and written by the web handler
static volatile bool prRestart = false;       // settings changed: drop the link and start again
static volatile bool prConnected = false;
static char prError[48] = "";
static char prGcode[12] = "";                 // RUNNING, PAUSE, FINISH, IDLE, FAILED, PREPARE ...
static volatile int prPercent = -1;
static volatile float prBed = NAN;
static volatile uint32_t prLastMsgAt = 0;
static const uint32_t PRINTER_STALE_MS = 30000;

static void prSetError(const char *text) { strlcpy(prError, text, sizeof prError); }

/* Stream scanner: finds "key": value pairs for a handful of keys at any depth, one character at a time. */
static const char *SCAN_KEYS[] = {"gcode_state", "mc_percent", "bed_temper"};
static struct {
  bool inString, escaped, tooLong, hadString, awaiting, capturing, valueIsString;
  char text[20]; uint8_t length;
  char value[16]; uint8_t valueLength;
  int8_t key;
  char gcode[12]; int percent; float bed; bool gotGcode, gotPercent, gotBed;
} scan;

static void scanReset() { memset(&scan, 0, sizeof scan); scan.key = -1; }

static void scanFinish() {
  scan.value[scan.valueLength] = 0;
  if (scan.key == 0) { strlcpy(scan.gcode, scan.value, sizeof scan.gcode); scan.gotGcode = true; }
  else if (scan.key == 1) { scan.percent = atoi(scan.value); scan.gotPercent = true; }
  else if (scan.key == 2) { scan.bed = atof(scan.value); scan.gotBed = true; }
  scan.capturing = false;
  scan.key = -1;
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
    if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\r') { scanFinish(); return; }
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
    if (scan.hadString && !scan.tooLong) {
      scan.key = -1;
      for (int i = 0; i < 3; i++) if (strcmp(scan.text, SCAN_KEYS[i]) == 0) scan.key = i;
      scan.awaiting = scan.key >= 0;
    }
    scan.hadString = false;
    return;
  }
  scan.hadString = false;
  if (scan.awaiting) {
    scan.awaiting = false;
    if (c == '{' || c == '[') { scan.key = -1; return; }
    scan.capturing = true; scan.valueIsString = false; scan.valueLength = 0;
    scan.value[scan.valueLength++] = c;
  }
}

/* Printers that send only what changed leave fields out, so each value is kept until a newer one arrives. */
static void scanCommit() {
  if (scan.gotGcode) strlcpy(prGcode, scan.gcode, sizeof prGcode);
  if (scan.gotPercent) prPercent = scan.percent;
  if (scan.gotBed) prBed = scan.bed;
  prLastMsgAt = millis();
}

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

static void printerTask(void *) {
  static uint8_t buf[512];
  static uint8_t packet[200];
  for (;;) {
    prConnected = false;
    xSemaphoreTake(prLock, portMAX_DELAY);
    String ip = prIp, serial = prSerial, code = prCode;
    xSemaphoreGive(prLock);
    prRestart = false;
    if (!ip.length() || !serial.length()) { prSetError(""); prWait(2000); continue; }
    if (WiFi.status() != WL_CONNECTED) { prSetError("Waiting for Wi-Fi"); prWait(2000); continue; }

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
      prSetError(buf[3] == 4 || buf[3] == 5 ? "Wrong access code or serial" : "Printer refused the sign-in");
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
    while (!prRestart) {
      if (millis() - lastPing > 30000) { const uint8_t ping[2] = {0xC0, 0}; net.write(ping, 2); lastPing = millis(); }
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
static float finishedHot = NAN;
static char lastGcode[12] = "";

static bool gcodeIs(const char *what) { return strcmp(prGcode, what) == 0; }
static bool printerFresh() { return prConnected && prLastMsgAt && millis() - prLastMsgAt < PRINTER_STALE_MS; }

static void trackPrinter() {
  if (!printerFresh()) return;
  const bool wasBusy = !strcmp(lastGcode, "RUNNING") || !strcmp(lastGcode, "PAUSE") || !strcmp(lastGcode, "PREPARE");
  if (gcodeIs("FINISH")) {
    if (wasBusy) { finishedAt = millis() | 1; finishedHot = prBed; }
    if (!isnan(prBed) && (isnan(finishedHot) || prBed > finishedHot)) finishedHot = prBed;
  } else {
    finishedAt = 0;
    finishedHot = NAN;
  }
  strlcpy(lastGcode, prGcode, sizeof lastGcode);
}

static State printerState(float &fill) {
  fill = prPercent < 0 ? 0 : prPercent;
  if (gcodeIs("PAUSE")) return ST_PAUSED;
  if (gcodeIs("RUNNING") || gcodeIs("PREPARE") || gcodeIs("SLICING")) return ST_PRINTING;
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
  if (finishedAt && millis() - finishedAt > 2UL * 3600000UL) return ST_IDLE;
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
  if (addonWhen == WHEN_ALWAYS) return a;
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
    int value = schedule["level"].as<int>();
    if (value < 0 || value > 100) return "Brightness is 0-100";
    schedLevel = value;
  }
  if (!doc["time"]["tz"].isNull()) {
    const char *tz = doc["time"]["tz"].as<const char *>();
    if (!tz || !*tz || strlen(tz) >= sizeof timeTz) return "The time zone rule is 1-47 characters";
    strlcpy(timeTz, tz, sizeof timeTz);
    const char *zone = doc["time"]["zone"].as<const char *>();
    strlcpy(timeZone, zone && *zone ? zone : tz, sizeof timeZone);
    applyTimeZone();
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

/* What to show, how full "fill" is, and who decided. A controller that is speaking wins over the bar's own printer link: it is assumed to know more. */
static State shown(float &fill, Source &source) {
  if (previewUntil && millis() < previewUntil) { fill = previewProgress; source = SRC_PREVIEW; return previewState; }
  previewUntil = 0;
  if (everPushed && millis() - lastPushAt <= LINK_TIMEOUT_MS) { fill = progress; source = SRC_CONTROLLER; return current; }
  if (printerFresh()) { source = SRC_PRINTER; return printerState(fill); }
  fill = 0;
  source = SRC_NONE;
  return ST_OFFLINE;
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

static void paintEffect(uint8_t effect, uint32_t colour, uint32_t colour2, uint8_t speed, float t, float dt) {
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

  for (int i = 0; i < N; i++) {
    float *c = fxColour[i];
    float &k = fxLevel[i];
    k = 1;
    switch (effect) {
      case FX_GRADIENT: mix(pal[0], pal[1], (float)i / (N - 1), c); break;
      case FX_WAVES:
        along(pal, (float)i / N + t * 0.05f * s + 0.12f * sinf(t * 0.6f * s + i / 3.0f), c);
        k = 0.55f + 0.45f * sinf(i / 2.5f - t * 0.9f * s);
        break;
      case FX_SCROLL: along(pal, (float)i / N + t * 0.07f * s, c); break;
      case FX_THEATRE: {
        const bool on = (i + (int)(t * s * 3)) % 3 == 0;
        memcpy(c, on ? pal[0] : pal[1], sizeof(float) * 3);
        k = on ? 1 : 0.25f;
        break;
      }
      case FX_SCANNER: {
        const float pos = (N - 1) * (0.5f - 0.5f * cosf(t * s * 1.5f));
        const float m = constrain(1 - fabsf(i - pos) / 3, 0.0f, 1.0f);
        mix(pal[1], pal[0], m, c);
        k = 0.25f + 0.75f * m;
        break;
      }
      case FX_METEOR: {
        /* A soft wide head and a long tail behind it; placed between LEDs so it glides rather than hops (a hop between LEDs reads as flicker). */
        const float pos = fmodf(t * s * 2.6f, N + 14);
        const float behind = pos - i;
        const float glowAt = behind < 0 ? expf(-(behind * behind) / 2.2f) : expf(-behind / 4.5f);
        along(pal, 1 - (float)i / N * 0.6f, c);
        k = sqrtf(constrain(glowAt, 0.0f, 1.0f));   // the bar squares levels later; this keeps the lamp's fall-off
        break;
      }
      case FX_AURORA: {
        const float v = constrain(0.5f + 0.5f * sinf(i * 0.35f + t * s * 0.35f) * sinf(i * 0.17f - t * s * 0.25f + 1.7f) + 0.15f, 0.0f, 1.0f);
        along(pal, 0.5f * v + (float)i / N * 0.3f + t * 0.02f * s, c);
        k = 0.5f + 0.5f * v;
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

static void render() {
  float fill;
  Source source;
  const State state = shown(fill, source);
  /* Music mode takes the bar over, except while a look is being tried from the page. */
  const bool music = mode == MODE_MUSIC && source != SRC_PREVIEW;
  const bool notice = noticeUntil && millis() < noticeUntil;
  if (!notice) noticeUntil = 0;
  /* An add-on gets the bar only in the mode that follows the printer, and never over a look being tried. */
  const Addon *addon = music || source == SRC_PREVIEW ? nullptr : addonNow(state);
  /* An add-on that sends a "look" is drawn by the same effects as the bar's own states. */
  const Look look = addon && addon->kind == ADDON_LOOK ? addon->look : looks[state];
  if (addon && addon->kind == ADDON_LOOK) fill = addon->fill;
  const bool dark = state == ST_OFF && !addon;
  const float fillTo = fill / 100.0f * LED_COUNT;
  const float t = millis() / 1000.0f;
  /* 1-100 on the page is 0.1 to 2 cycles a second. */
  const float rate = 0.1f + look.speed / 100.0f * 1.9f;
  const float level = (schedActive() ? schedLevel : brightness) / 100.0f;
  const uint8_t baseR = (look.colour >> 16) & 255, baseG = (look.colour >> 8) & 255, baseB = look.colour & 255;
  /* Meters share the bar equally, with one dark LED between neighbours so two full meters do not read as one. */
  const int meters = addon && addon->kind == ADDON_METERS ? addon->meterCount : 0;
  const float meterSpan = meters ? (LED_COUNT - (meters - 1)) / (float)meters : 0;
  static uint32_t lastFrameMs = 0;
  const float dt = lastFrameMs ? min(0.1f, (millis() - lastFrameMs) / 1000.0f) : 0.025f;
  lastFrameMs = millis();
  const bool lampFx = !dark && look.effect >= FX_GRADIENT;
  if (lampFx) paintEffect(look.effect, look.colour, secondColour(look), look.speed, t, dt);

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
        v = lit >= 1 ? 1 : lit > 0 ? max(lit, 0.2f) : 0.2f;
      }
    } else switch (dark ? FX_SOLID : look.effect) {
      case FX_BREATHE: v = 0.10f + 0.90f * (0.5f - 0.5f * cosf(2 * PI * t * rate * 0.5f)); break;
      case FX_FILL: {
        /* The LED the edge is in is lit in proportion, so progress moves smoothly rather than a whole LED at a time. The rest stay faintly lit so the bar never looks switched off mid-print. */
        /* 0.2 is the floor because the level is squared below: 0.05 came out as nothing at all on the real LEDs (0.1.0). */
        float lit = fillTo - i;
        v = lit >= 1 ? 1 : lit > 0 ? max(lit, 0.2f) : 0.2f;
        break;
      }
      case FX_FLASH: v = fmodf(t * rate * 2, 1) < 0.5f ? 1 : 0.15f; break;
      case FX_CHASE: {
        float head = fmodf(t * rate * LED_COUNT * 0.5f, LED_COUNT);
        float behind = fmodf(head - i + LED_COUNT, LED_COUNT);
        v = behind < 6 ? 1 - behind / 6 : 0.2f;
        if (v < 0.2f) v = 0.2f;
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
    if (dark && !music && !notice) v = 0;
    /* Squared: LEDs are far from linear, and without it "breathe" spends most of its time looking fully on. */
    const float scale = v * v * level;
    r = r * scale; g = g * scale; b = b * scale;
    frame[i][0] = r; frame[i][1] = g; frame[i][2] = b;

    uint8_t wire[3];
    for (int c = 0; c < 3; c++) wire[c] = order[c] == 'R' ? r : order[c] == 'G' ? g : b;
    strip.setPixelColor(reversed ? LED_COUNT - 1 - i : i, wire[0], wire[1], wire[2]);
  }
  strip.show();
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

static void startWifi() {
  WiFi.persistent(false);   // our settings live in our own namespace, not the driver's
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("PandaStatusP2");   // the name stock used, so the router sees the same device
  WiFi.setAutoReconnect(true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  if (!wifiSsid.length()) readStockWifi(wifiSsid, wifiPass);
  if (wifiSsid.length()) WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  lastTryAt = millis();
}

/* ------------------------------------------------- staying recoverable -- */

static bool onTrial = false;
static const uint32_t TRIAL_GIVE_UP_MS = 120000;
static const uint32_t TRIAL_PROVEN_MS = 20000;
static const uint32_t AP_AFTER_MS = 180000;

static bool otherSlot(const esp_partition_t *&part, esp_app_desc_t &desc) {
  part = esp_ota_get_next_update_partition(nullptr);
  return part && esp_ota_get_partition_description(part, &desc) == ESP_OK;
}

/* Hand the bar to the firmware in the other slot. Returns only if there is nothing bootable there. */
static void bootOtherSlot() {
  const esp_partition_t *part;
  esp_app_desc_t desc;
  if (!otherSlot(part, desc)) return;
  if (esp_ota_set_boot_partition(part) != ESP_OK) return;
  strip.clear();
  strip.show();
  delay(200);
  ESP.restart();
}

/* The Arduino core would otherwise mark every new image good before setup() runs, which is exactly the decision this firmware wants to make itself. */
extern "C" bool verifyRollbackLater() { return true; }

static void watchTrial() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  /* When the network was last lost (or boot, if it has never been up). Kept apart from lastTryAt, which moves on every retry. */
  static uint32_t lostAt = 0;
  if (connected && !connectedSince) connectedSince = millis();
  if (!connected && (connectedSince || !lostAt)) lostAt = millis();
  if (!connected) connectedSince = 0;

  if (onTrial) {
    if (connectedSince && millis() - connectedSince > TRIAL_PROVEN_MS) {
      esp_ota_mark_app_valid_cancel_rollback();
      prefs.putString("okbuild", BUILD_ID);
      onTrial = false;
    } else if (!connectedSince && millis() > TRIAL_GIVE_UP_MS) {
      /* The bootloader's own rollback first, if stock's bootloader has it; our own switch of slots if it does not. */
      esp_ota_mark_app_invalid_rollback_and_reboot();
      bootOtherSlot();
      onTrial = false;   // nothing to go back to (flashed by USB): carry on and offer the setup network
    }
    return;
  }

  /* A proven build that has lost the network keeps trying, and after a while also offers its own network so the Wi-Fi details can be changed. */
  /* With no network name saved at all (a bar flashed from new) there is nothing to wait for: the setup network comes up at once. */
  if (!connected && !apUp && (millis() - lostAt > AP_AFTER_MS || !wifiSsid.length())) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("PandaP2-Setup", apPassword().c_str());
    apUp = true;
  }
  if (connected && apUp && !apByButton && millis() - connectedSince > 60000) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apUp = false;
  }
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
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("PandaP2-Setup", apPassword().c_str());
    apUp = true;
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
  if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain"))) {
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
  doc["heap"] = ESP.getFreeHeap();
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["setupNetwork"] = apUp;
  doc["onTrial"] = onTrial;
  doc["leds"] = LED_COUNT;

  float fill;
  Source source;
  const State state = shown(fill, source);
  doc["state"] = ST_NAMES[state];
  doc["source"] = SRC_NAMES[source];
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
  printer["paired"] = prIp.length() > 0 && prSerial.length() > 0;
  printer["ip"] = prIp;
  printer["serial"] = prSerial;
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

/* What a controller calls every few seconds: {"state":"printing","progress":42}. */
static void handleState() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  int at = findName(ST_NAMES, ST_COUNT, doc["state"].as<const char *>());
  if (at < 0 || at == ST_OFFLINE) return sendResult(false, "Unknown state");
  current = (State)at;
  if (!doc["progress"].isNull()) progress = constrain(doc["progress"].as<float>(), 0.0f, 100.0f);
  lastPushAt = millis();
  everPushed = true;
  sendResult(true);
}

static void handleConfig() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  const char *error = applyConfig(doc.as<JsonVariantConst>());
  if (error) return sendResult(false, error);
  saveConfig();
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
static void handleAddon() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  const char *id = doc["id"].as<const char *>();
  if (!id || !*id || strlen(id) > 15) return sendResult(false, "id is 1-15 characters");
  for (const char *c = id; *c; c++) if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '-' || *c == '_')) return sendResult(false, "id uses a-z, 0-9, - and _");
  Addon *slot = addonById(id);
  if (doc["remove"].as<bool>()) {
    if (slot) slot->used = false;
    return sendResult(true);
  }
  if (!slot) {
    /* A free place, or failing that one whose add-on has stopped posting. The add-on the owner picked is never pushed out by a stranger. */
    for (int i = 0; i < ADDON_SLOTS && !slot; i++) if (!addons[i].used) slot = &addons[i];
    for (int i = 0; i < ADDON_SLOTS && !slot; i++) if (!addonFresh(addons[i]) && strcmp(addons[i].id, addonShow) != 0) slot = &addons[i];
    if (!slot) return sendResult(false, "Four add-ons are already posting; remove one first");
  }

  Addon next = {};
  next.used = true;
  strlcpy(next.id, id, sizeof next.id);
  const char *name = doc["name"].as<const char *>();
  strlcpy(next.name, name && *name ? name : id, sizeof next.name);
  const int ttl = doc["ttl"].isNull() ? 120 : doc["ttl"].as<int>();
  if (ttl < 1 || ttl > 3600) return sendResult(false, "ttl is 1-3600 seconds");
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
      if (!ok) return sendResult(false, "frame is 25 colours: a list of #RRGGBB, or 150 hex digits");
      next.pixels[i][0] = (colour >> 16) & 255; next.pixels[i][1] = (colour >> 8) & 255; next.pixels[i][2] = colour & 255;
    }
  } else if (!doc["meters"].isNull()) {
    next.kind = ADDON_METERS;
    JsonArrayConst list = doc["meters"].as<JsonArrayConst>();
    if (list.size() < 1 || list.size() > 4) return sendResult(false, "meters is a list of 1-4");
    for (JsonVariantConst one : list) {
      uint32_t colour = 0xFFFFFF;
      if (!one["colour"].isNull() && !parseColour(one["colour"].as<const char *>(), colour)) return sendResult(false, "Colour must be #RRGGBB");
      next.meterColour[next.meterCount] = colour;
      next.meterValue[next.meterCount] = constrain(one["value"].as<float>(), 0.0f, 100.0f);
      next.meterCount++;
    }
  } else if (!doc["look"].isNull()) {
    next.kind = ADDON_LOOK;
    JsonVariantConst look = doc["look"];
    next.look = {0xFFFFFF, FX_SOLID, 50};
    if (!look["colour"].isNull() && !parseColour(look["colour"].as<const char *>(), next.look.colour)) return sendResult(false, "Colour must be #RRGGBB");
    if (!look["colour2"].isNull() && !parseColour(look["colour2"].as<const char *>(), next.look.colour2)) return sendResult(false, "Second colour must be #RRGGBB");
    if (!look["effect"].isNull()) {
      int fx = findName(FX_NAMES, FX_COUNT, look["effect"].as<const char *>());
      if (fx < 0) return sendResult(false, "Unknown effect");
      next.look.effect = fx;
    }
    if (!look["speed"].isNull()) next.look.speed = constrain(look["speed"].as<int>(), 1, 100);
    next.fill = look["progress"].isNull() ? 100 : constrain(look["progress"].as<float>(), 0.0f, 100.0f);
  } else {
    return sendResult(false, "Send one of look, meters or frame");
  }
  *slot = next;

  JsonDocument answer;
  answer["ok"] = true;
  /* So a script can tell whether it is the one on the bar right now, and say so to whoever runs it. */
  answer["picked"] = strcmp(addonShow, id) == 0;
  sendJson(200, answer);
}

/* What the bar can be asked to do, for anyone writing something to talk to it. */
static const char API_DOC[] PROGMEM = R"JSON({"about":"Panda Status P2 custom firmware. JSON in, JSON out, no sign-in; every POST answers {ok, message?}.",
"endpoints":[
{"call":"GET /api/status","does":"Everything: what is showing and why, printer link, microphone, clock, add-ons, settings, the 25 LEDs as hex"},
{"call":"POST /api/state","body":{"state":"idle|printing|paused|finished|cooling|ready|error|off","progress":"0-100"},"does":"A controller saying what the printer is doing. Repeat at least every 45 s; when it stops, the bar's own printer link takes over"},
{"call":"POST /api/addon","body":{"id":"a-z0-9-_ up to 15","name":"shown on the page","ttl":"seconds this post stays valid, default 120","look":{"colour":"#RRGGBB","colour2":"#RRGGBB","effect":"any effect listed under /api/config","speed":"1-100","progress":"0-100"},"meters":[{"value":"0-100","colour":"#RRGGBB"}],"frame":"25 colours as a list of #RRGGBB or 150 hex digits"},"does":"An add-on saying what it wants shown: send ONE of look, meters (1-4) or frame, and keep posting. Shown only if picked under Add-ons. {id, remove:true} withdraws it"},
{"call":"POST /api/config","body":{"brightness":"0-100","mode":"status|music","music":{"sensitivity":"low|medium|high|auto"},"states":{"<state>":{"colour":"#RRGGBB","colour2":"#RRGGBB or auto","effect":"solid|breathe|fill|flash|chase|rainbow|gradient|waves|scroll|theatre|scanner|meteor|fire|twinkle|sparkle|rain|aurora","speed":"1-100"}},"cool":{"minutes":"0-120","target":"20-60"},"schedule":{"enabled":true,"from":"HH:MM","to":"HH:MM","level":"0-100"},"time":{"tz":"POSIX rule","zone":"label"},"addon":{"show":"<id> or empty","when":"idle|always"},"order":"GRB|RGB|...","reversed":false},"does":"Change any subset of the settings; saved"},
{"call":"POST /api/preview","body":{"state":"<state>","progress":"0-100","seconds":"1-120"},"does":"Show a state briefly without it counting as the printer's"},
{"call":"POST /api/printer","body":{"ip":"...","serial":"...","code":"..."},"does":"Pair with a Bambu printer; {forget:true} unpairs"},
{"call":"POST /api/wifi","body":{"ssid":"...","password":"..."},"does":"Save Wi-Fi and restart"},
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
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass ? pass : "");
  sendResult(true, "Saved. Restarting to join it.");
  delay(500);
  ESP.restart();
}

/* Pair with a printer, or {"forget":true} to unpair. The link's task picks the change up by itself. */
static void handlePrinter() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  String ip, serial, code;
  if (!doc["forget"].as<bool>()) {
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
  /* Remembered, so an unpaired bar does not quietly pair itself again from the stock firmware's old record. */
  prefs.putBool("pset", true);
  xSemaphoreTake(prLock, portMAX_DELAY);
  prIp = ip; prSerial = serial; prCode = code;
  xSemaphoreGive(prLock);
  prGcode[0] = 0; prPercent = -1; prBed = NAN; prLastMsgAt = 0;
  prRestart = true;
  sendResult(true, ip.length() ? "Saved. Connecting to the printer." : "Unpaired.");
}

static void handleSlot() {
  const esp_partition_t *part;
  esp_app_desc_t desc;
  if (!otherSlot(part, desc)) return sendResult(false, "There is no firmware in the other slot");
  sendResult(true, "Restarting into the other slot");
  delay(500);
  bootOtherSlot();
}

/* Forget everything this firmware has saved (looks, mode, pairing, Wi-Fi) and start again. Stock's own settings are a separate namespace and are not touched, so the bar comes back on the Wi-Fi and pairing stock had. */
static void handleFactory() {
  prefs.clear();
  sendResult(true, "Settings cleared. Restarting.");
  delay(500);
  ESP.restart();
}

static void handleReboot() {
  sendResult(true, "Restarting");
  delay(500);
  ESP.restart();
}

static void handleUpdateDone() {
  if (Update.hasError()) return sendResult(false, Update.errorString());
  sendResult(true, "Installed. Restarting into it.");
  delay(800);
  ESP.restart();
}

static void handleUpdateChunk() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!Update.hasError()) Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    Update.end(true);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

/* ------------------------------------------------------------ main -- */

void setup() {
  Serial.begin(115200);
  prefs.begin("panda", false);
  loadConfig();
  strip.begin();
  strip.clear();
  strip.show();

  onTrial = prefs.getString("okbuild", "") != BUILD_ID;
  startWifi();
  applyTimeZone();

  prLock = xSemaphoreCreateMutex();
  if (prefs.getBool("pset", false)) {
    prIp = prefs.getString("pip", "");
    prSerial = prefs.getString("psn", "");
    prCode = prefs.getString("pcode", "");
  } else {
    readStockPairing(prIp, prSerial, prCode);
  }
  /* Its own task: a TLS handshake blocks for seconds, and the LEDs and the page must not freeze while it does. */
  xTaskCreate(printerTask, "printer", 12288, nullptr, 1, nullptr);
  /* The microphone runs all the time, not only in Music mode: it is a few percent of the processor, and the page can then show the level for setting the sensitivity. */
  if (prefs.getString("audiotry", "") == BUILD_ID) {
    audioBlocked = true;
  } else {
    prefs.putString("audiotry", BUILD_ID);
    xTaskCreate(audioTask, "audio", 6144, nullptr, 1, nullptr);
  }
  pinMode(PIN_BUTTON, INPUT_PULLUP);

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
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/factory", HTTP_POST, handleFactory);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateChunk);
  server.onNotFound([]() { sendResult(false, "Not found"); });
  server.begin();
  MDNS.begin("pandastatus");
  MDNS.addService("http", "tcp", 80);
}

void loop() {
  server.handleClient();
  static uint32_t lastFrameAt = 0;
  /* Not while an update is being written: the LED signal is bit-timed and the two fight over the same core. */
  if (!Update.isRunning() && millis() - lastFrameAt >= 25) {
    lastFrameAt = millis();
    render();
  }
  pollButton();
  static uint32_t lastWatchAt = 0;
  if (millis() - lastWatchAt >= 1000) {
    lastWatchAt = millis();
    watchTrial();
    trackPrinter();
    /* Ten seconds of the microphone running without taking the chip down is proof enough to stop guarding it. */
    if (audioRunning && !audioConfirmed && millis() > 15000) { prefs.remove("audiotry"); audioConfirmed = true; }
  }
}
