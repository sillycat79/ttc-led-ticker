// ttc led ticker ✿
// live ttc arrivals scrolling right → left on two chained 8x32 WS2812B panels (64x8)
// shows only trips heading to Union Station by default (see DIRECTION_MATCH)
//
// board: "ESP32 Dev Module" (ELEGOO ESP32 / ESP32-WROOM-32)
// libraries (Arduino Library Manager):
//   - FastLED                 (by Daniel Garcia)
//   - Adafruit GFX Library    (by Adafruit)
//   - ArduinoJson (v7)        (by Benoit Blanchon)
//
// wifi lives in secrets.h (copy secrets.example.h → secrets.h). it's gitignored.
//
// how it works:
//   - a little background task (core 0) asks the ttc feed for predictions every 30 s
//   - the main loop (core 1) scrolls the text 1 pixel at a time, never waiting on wifi
//   - minutes are recounted from each bus's real arrival timestamp every pass,
//     so "3 MIN" turns into "2 MIN" on time even between fetches

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <Adafruit_GFX.h>
#include <time.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS

// ================= settings ✿ =================

// stop ids are on the stop pole / google maps (under the stop name: "Stop ID: 12345")
const char* STOP_IDS[]   = { "10210" };     // example stop id, swap in yours. add more like { "12345", "67890" }
// routes in the order you want them shown. anything else at the stop goes after
const char* ROUTE_ORDER[] = { "509", "511" };
// only show trips whose direction mentions this, e.g. the ttc labels them like
// "East - 509 Harbourfront towards Union Station". set to "" to show everything at the stop
const char* DIRECTION_MATCH = "Union";
const bool  ONLY_LISTED_ROUTES = false;     // true = hide routes not in ROUTE_ORDER
const int   ARRIVALS_PER_ROUTE = 2;         // "3 & 10 MIN"

const unsigned long FETCH_MS  = 30UL * 1000UL;  // ask the ttc every 30 s
const uint16_t      SCROLL_MS = 35;             // ms per pixel. lower = faster
const uint8_t       BRIGHTNESS       = 40;      // 0-255. red text at 40 is plenty indoors
const uint8_t       NIGHT_BRIGHTNESS = 6;
const int           NIGHT_START_H = 23;         // dim from 11 pm ...
const int           NIGHT_END_H   = 7;          // ... to 7 am

const CRGB COLOR_TEXT = CRGB(255, 0, 0);        // classic station red
const CRGB COLOR_ERR  = CRGB(255, 0, 0);

// ================= panel wiring =================
#define DATA_PIN   16          // esp32 gpio 16 → 330 Ω → panel 1 DIN
#define PANEL_W    32
#define PANEL_H    8
#define NUM_PANELS 2
#define W (PANEL_W * NUM_PANELS)   // 64
#define H PANEL_H                  // 8
#define NUM_LEDS (W * H)           // 512

// these flexible panels are wired in vertical zigzag columns.
// if the boot "hi" looks mirrored / upside down / scrambled, flip these
#define FLIP_X      false      // text mirrored left↔right → true
#define FLIP_Y      false      // text upside down → true
#define SERPENTINE  true       // every other column garbled → false

CRGB leds[NUM_LEDS];

uint16_t XY(int x, int y) {
  if (FLIP_X) x = W - 1 - x;
  if (FLIP_Y) y = H - 1 - y;
  int panel = x / PANEL_W;
  int lx    = x % PANEL_W;
  int ly    = (SERPENTINE && (lx & 1)) ? (H - 1 - y) : y;
  return panel * PANEL_W * PANEL_H + lx * PANEL_H + ly;
}

// ================= arrivals (shared between cores) =================
struct Arrival {
  char     route[6];       // "509"
  char     name[20];       // "HARBOURFRONT"
  int64_t  epochMs;        // when it gets here
  int      minutesAtFetch; // backup if the clock isn't synced yet
};

const int MAX_ARRIVALS = 24;
Arrival  arrivals[MAX_ARRIVALS];
int      arrivalCount = 0;
bool     lastFetchOk = false;
bool     everFetched = false;
unsigned long lastGoodFetch = 0;
SemaphoreHandle_t dataLock;

// ================= helpers =================
int routeRank(const char* r) {
  int n = sizeof(ROUTE_ORDER) / sizeof(ROUTE_ORDER[0]);
  for (int i = 0; i < n; i++) if (!strcmp(r, ROUTE_ORDER[i])) return i;
  return 100;
}

// "509-Harbourfront" → "HARBOURFRONT"
void shortName(const char* title, char* out, size_t len) {
  const char* dash = strchr(title, '-');
  const char* s = dash ? dash + 1 : title;
  size_t i = 0;
  for (; *s && i < len - 1; s++) out[i++] = toupper(*s);
  out[i] = 0;
}

// the feed gives an object when there's one thing and an array when there's more
template <typename F>
void eachOf(JsonVariant v, F fn) {
  if (v.is<JsonArray>()) { for (JsonVariant x : v.as<JsonArray>()) fn(x); }
  else if (v.is<JsonObject>()) fn(v);
}

bool clockSynced() { return time(nullptr) > 1700000000; }

int minutesUntil(const Arrival& a) {
  if (!clockSynced()) return a.minutesAtFetch;
  int64_t nowMs = (int64_t)time(nullptr) * 1000;
  return (int)((a.epochMs - nowMs) / 60000);
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
}

// ================= fetching (core 0) =================
bool fetchStop(const char* stopId, Arrival* out, int& n) {
  WiFiClientSecure client;
  client.setInsecure();  // skips the TLS cert check, fine for a wall toy

  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding so ArduinoJson can read the stream
  String url = String("https://retro.umoiq.com/service/publicJSONFeed?command=predictions&a=ttc&stopId=") + stopId;
  http.begin(client, url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) { Serial.printf("ttc HTTP %d\n", code); http.end(); return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { Serial.printf("json error: %s\n", err.c_str()); return false; }
  if (!doc["Error"].isNull()) { Serial.println("ttc says: bad stop id?"); return false; }

  eachOf(doc["predictions"], [&](JsonVariant p) {
    const char* tag   = p["routeTag"]   | "";
    const char* title = p["routeTitle"] | tag;
    if (ONLY_LISTED_ROUTES && routeRank(tag) == 100) return;
    eachOf(p["direction"], [&](JsonVariant d) {
      const char* dir = d["title"] | "";
      if (DIRECTION_MATCH[0] && !strstr(dir, DIRECTION_MATCH)) return;
      eachOf(d["prediction"], [&](JsonVariant pr) {
        if (n >= MAX_ARRIVALS) return;
        Arrival& a = out[n++];
        strlcpy(a.route, tag, sizeof(a.route));
        shortName(title, a.name, sizeof(a.name));
        a.epochMs = atoll(pr["epochTime"] | "0");
        a.minutesAtFetch = atoi(pr["minutes"] | "0");
      });
    });
  });
  return true;
}

void fetchTask(void*) {
  for (;;) {
    connectWiFi();
    Arrival fresh[MAX_ARRIVALS];
    int n = 0;
    bool ok = WiFi.status() == WL_CONNECTED;
    if (ok) {
      for (const char* id : STOP_IDS) ok = fetchStop(id, fresh, n) && ok;
    }

    // sort: route order first, then soonest
    for (int i = 1; i < n; i++)
      for (int j = i; j > 0; j--) {
        int ra = routeRank(fresh[j - 1].route), rb = routeRank(fresh[j].route);
        int c = ra != rb ? ra - rb : strcmp(fresh[j - 1].route, fresh[j].route);
        if (c > 0 || (c == 0 && fresh[j - 1].epochMs > fresh[j].epochMs)) {
          Arrival t = fresh[j]; fresh[j] = fresh[j - 1]; fresh[j - 1] = t;
        } else break;
      }

    xSemaphoreTake(dataLock, portMAX_DELAY);
    if (ok) {
      memcpy(arrivals, fresh, sizeof(Arrival) * n);
      arrivalCount = n;
      lastGoodFetch = millis();
      everFetched = true;
    }
    lastFetchOk = ok;
    xSemaphoreGive(dataLock);

    Serial.printf("fetch %s, %d arrivals\n", ok ? "ok" : "FAILED", n);
    vTaskDelay(pdMS_TO_TICKS(FETCH_MS));
  }
}

// ================= the message =================
// "509 HARBOURFRONT > 3 & 10 MIN"  (or with more routes: "... MIN  |  511 BATHURST > NOW & 7 MIN")
String buildMessage(bool& stale) {
  String msg;
  xSemaphoreTake(dataLock, portMAX_DELAY);
  stale = everFetched && !lastFetchOk;
  bool veryStale = everFetched && millis() - lastGoodFetch > 5UL * 60UL * 1000UL;

  if (WiFi.status() != WL_CONNECTED && !everFetched) msg = "NO WIFI :(";
  else if (!everFetched) msg = "LOADING TTC...";
  else if (veryStale) msg = "TTC FEED DOWN, CHECK THE APP :(";
  else {
    char cur[6] = "";
    int shown = 0;
    for (int i = 0; i < arrivalCount; i++) {
      const Arrival& a = arrivals[i];
      int m = minutesUntil(a);
      if (m < 0) continue;                        // already left
      if (strcmp(cur, a.route)) {                 // new route
        if (msg.length()) msg += " MIN  |  ";
        msg += a.route; msg += " "; msg += a.name; msg += " > ";
        strlcpy(cur, a.route, sizeof(cur));
        shown = 0;
      }
      if (shown >= ARRIVALS_PER_ROUTE) continue;
      if (shown) msg += " & ";
      msg += m == 0 ? String("NOW") : String(m);
      shown++;
    }
    if (msg.length()) msg += " MIN";
    else msg = "NOTHING COMING RIGHT NOW";
  }
  xSemaphoreGive(dataLock);
  msg.replace("NOW MIN", "NOW");                  // "> NOW MIN" reads weird
  return msg;
}

// ================= drawing (core 1) =================
// the text is drawn once into a 1-bit canvas, then we slide a 64 px window across it
void scrollOnce(const String& text, bool errorDot) {
  int textW = text.length() * 6;                 // default gfx font: 6 px per char
  int canvasW = W + textW + W;                   // blank → text → blank
  GFXcanvas1* canvas = new GFXcanvas1(canvasW, H);
  if (!canvas || !canvas->getBuffer()) { delete canvas; delay(1000); return; }
  canvas->fillScreen(0);
  canvas->setTextWrap(false);
  canvas->setTextSize(1);
  canvas->setCursor(W, 0);                       // rows 0-6, row 7 stays empty
  canvas->print(text);

  for (int off = 0; off <= W + textW; off++) {
    unsigned long t0 = millis();
    for (int x = 0; x < W; x++)
      for (int y = 0; y < H; y++)
        leds[XY(x, y)] = canvas->getPixel(off + x, y) ? COLOR_TEXT : CRGB(0, 0, 0);
    // tiny red dot bottom-right = last fetch failed (still showing the old times)
    if (errorDot) leds[XY(W - 1, H - 1)] = COLOR_ERR;
    FastLED.show();
    long wait = SCROLL_MS - (long)(millis() - t0);
    if (wait > 0) delay(wait);
  }
  delete canvas;
}

void applyNightMode() {
  struct tm t;
  if (!getLocalTime(&t, 10)) return;
  bool night = NIGHT_START_H > NIGHT_END_H
    ? (t.tm_hour >= NIGHT_START_H || t.tm_hour < NIGHT_END_H)
    : (t.tm_hour >= NIGHT_START_H && t.tm_hour < NIGHT_END_H);
  FastLED.setBrightness(night ? NIGHT_BRIGHTNESS : BRIGHTNESS);
}

// ================= setup + loop =================
void setup() {
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 3500);  // stays under the 4 A adapter
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);

  // boot check: "hi" on the left panel. if it's mirrored/upside down, see FLIP_X/FLIP_Y
  GFXcanvas1 hi(W, H);
  hi.setCursor(1, 0);
  hi.print("hi :)");
  for (int x = 0; x < W; x++)
    for (int y = 0; y < H; y++)
      if (hi.getPixel(x, y)) leds[XY(x, y)] = COLOR_TEXT;
  FastLED.show();

  dataLock = xSemaphoreCreateMutex();
  connectWiFi();
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org");  // toronto time
  delay(1500);
  xTaskCreatePinnedToCore(fetchTask, "ttc", 20480, nullptr, 1, nullptr, 0);
}

void loop() {
  applyNightMode();
  bool stale;
  String msg = buildMessage(stale);
  Serial.println(msg);
  scrollOnce(msg, stale);
}
