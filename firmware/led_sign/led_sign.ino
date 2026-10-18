// led_sign.ino - ESP32 controller for an Adaptive Micro Systems 80x7 transit
// sign (PN 1105-2126) over RS-485 using the Alpha sign protocol.
//
// Two display modes:
//   * Weather (default, Ames, Iowa): joins your home Wi-Fi, pulls today's
//     forecast from Open-Meteo every 5 minutes, and scrolls it right to left:
//     city, conditions, now/high/low, then precipitation type, amount and
//     chance on one screen when the chance is above 5%, otherwise how
//     cloudy the sky is.
//   * Message: shows text you type.
//
// Control it from:
//   * USB Serial Monitor (115200 baud): type a message, or /help for commands
//   * Web page: http://ledsign.local on your home network, or join the
//     "LED-Sign" Wi-Fi network and open http://192.168.4.1 when the ESP32
//     can't reach your home Wi-Fi
//
// Settings (including Wi-Fi and location) are saved in flash. Wiring and
// setup: see README.md.

#include <Arduino.h>
#include <algorithm>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "AlphaSign.h"
#include "Geocode.h"
#include "J1587Sign.h"
#include "JsonStream.h"
#include "Transit.h"
#include "Weather.h"

// ---------------------------------------------------------------- configuration

// RS-485 transceiver pins.
//
// Default: ESP32-S3 RS485/CAN board (Waveshare ESP32-S3-RS485-CAN, also sold
// as KLAYERS). The transceiver is on the board: TX = GPIO17, RX = GPIO18,
// and GPIO21 enables the driver (HIGH = send, LOW = receive).
//
// Separate module on a plain ESP32 instead:
//   * Auto-direction module (DIYables RS485-TTL etc., pins VCC GND RXD TXD):
//     RX = 16 (module TXD), TX = 17 (module RXD), DE = -1.
//   * MAX3485-style module: RX = RO, TX = DI, DE = the GPIO wired to DE+RE.
constexpr int RS485_RX_PIN = 18;
constexpr int RS485_TX_PIN = 17;
constexpr int RS485_DE_PIN = 21;  // -1 if the transceiver switches direction itself
// Default driver-enable polarity; both can be changed at runtime with /de.
constexpr bool RS485_DE_ACTIVE_HIGH = true;

// Home Wi-Fi. You can leave these blank and set them later with /ssid and
// /pass (or from the web page); saved values take priority over these.
const char* DEFAULT_WIFI_SSID = "";
const char* DEFAULT_WIFI_PASSWORD = "";

// Fallback access point, started when home Wi-Fi isn't reachable so you can
// still configure the sign. Password must be at least 8 characters.
const char* AP_SSID = "LED-Sign";
const char* AP_PASSWORD = "ledsign123";

// Web page address on your home network: http://<MDNS_NAME>.local
const char* MDNS_NAME = "ledsign";

constexpr uint32_t BOOT_SEND_DELAY_MS = 3000;             // let the sign boot first
constexpr uint32_t WIFI_FALLBACK_MS = 20000;              // start the AP after this long offline
constexpr uint32_t WEATHER_INTERVAL_MS = 5UL * 60 * 1000;   // fetch the forecast every 5 min
constexpr uint32_t WEATHER_RETRY_MS = 60UL * 1000;        // first retry after a failed fetch
constexpr uint32_t WEATHER_STALE_MS = 3UL * 60 * 60 * 1000;  // stop showing data older than this
constexpr uint32_t WEATHER_RESEND_MS = 60UL * 60 * 1000;     // re-send hourly in case the sign lost power

// Sign address used until you set another one with /addr. The sign shows
// its address at power-up ("ADDRESS = 01"). "00" is the Alpha broadcast
// address, but not every sign firmware listens to it.
const char* DEFAULT_SIGN_ADDRESS = "01";

// Weather location used until you set another one (/location or the web
// page). Ames, Iowa.
const char* DEFAULT_PLACE = "AMES";
const char* DEFAULT_PLACE_QUERY = "Ames, Iowa, US";
const char* DEFAULT_PLACE_DETAIL = "IOWA, UNITED STATES";
constexpr double DEFAULT_LATITUDE = 42.0308;
constexpr double DEFAULT_LONGITUDE = -93.6319;

// ---- Bus times (Transitland, https://www.transit.land) ----
// Get a free API key at transit.land and enter it on the web page (it's saved
// on the ESP32). You can also put it here in your own copy, but never commit
// a real key to a public repository.
const char* DEFAULT_TRANSIT_API_KEY = "";
// Time zone for timetable times, as a POSIX TZ string (US Central for Ames).
const char* BUS_TIMEZONE = "CST6CDT,M3.2.0,M11.1.0";
constexpr uint8_t BUS_DEFAULT_EVERY_MIN = 5;          // how often to fetch bus times
constexpr uint8_t BUS_EVERY_CHOICES[] = {2, 3, 5, 10, 15};
constexpr long BUS_FREE_QUOTA = 10000;                // free Transitland plan: requests per month
constexpr long BUS_WINDOW_S = 2L * 60 * 60;           // look this far ahead for buses
constexpr int BUS_DEPARTURE_LIMIT = 60;
constexpr long BUS_LINES_WINDOW_S = 24L * 60 * 60;    // when finding a stop's lines
constexpr int BUS_LINES_LIMIT = 100;
constexpr int BUS_SEARCH_RADIUS_M = 10000;            // stop search around the weather location
constexpr uint32_t BUS_RETRY_MS = 60UL * 1000;
constexpr uint32_t BUS_STALE_MIN_MS = 30UL * 60 * 1000;

// How weather screens move: "rotate" scrolls each one right to left.
// How weather screens appear: "rotate" scrolls each one right to left, one
// after another; "hold" shows each screen still and switches to the next.
const char* WEATHER_SCREEN_MODE = "rotate";

// About 13 characters fit across the 80-column display. With "hold", wider
// screens are split into several held screens so nothing is cut off.
constexpr size_t SIGN_WIDTH_CHARS = 13;

constexpr size_t MAX_MESSAGE_LEN = 240;

// ---------------------------------------------------------------- state

struct SerialFormat {
  const char* name;
  uint32_t config;
};

// Alpha 1.0 signs use 7E2; Alpha 2.0+ also accept 8N1 (protocol manual, Table 3).
static const SerialFormat FORMATS[] = {{"7E2", SERIAL_7E2}, {"8N1", SERIAL_8N1}};
constexpr size_t FORMAT_COUNT = sizeof(FORMATS) / sizeof(FORMATS[0]);

// How text reaches the sign:
//   J1587: J1708/J1587 transport (RTS -> CTS -> data) at 9600 8N1. This is
//          what the TDR80x7 transit sign (PN 1105-2126) accepts on its J1708
//          wires; byte layout from a known-good Linux program, see J1587Sign.h.
//   ALPHA: plain Alpha protocol packets, for regular Alpha signs.
enum Protocol : uint8_t { PROTO_J1587 = 0, PROTO_ALPHA = 1 };

constexpr uint32_t J1587_IDLE_MS = 50;         // bus quiet before an RTS
constexpr uint32_t J1587_CTS_TIMEOUT_MS = 1500;
constexpr uint32_t J1587_TURNAROUND_MS = 80;   // after CTS, before data
constexpr uint32_t J1587_SEGMENT_GAP_MS = 20;  // between data segments
constexpr uint32_t J1587_ACK_WAIT_MS = 500;

struct Settings {
  uint8_t protocol = PROTO_J1587;
  // Send each screen with its Alpha mode code (weather: scroll right to left,
  // one screen after another). Off = plain text joined into one line, the
  // exact format of the known-good example, as a fallback.
  bool j1587Mode = true;
  uint32_t baud = 9600;
  uint8_t format = 0;  // index into FORMATS
  alpha::TextOptions text;
  bool usePriority = false;
  String message = "HELLO FROM ESP32";

  String wifiSsid;
  String wifiPassword;

  bool weatherOn = true;
  double latitude = DEFAULT_LATITUDE;
  double longitude = DEFAULT_LONGITUDE;
  String placeName = DEFAULT_PLACE;           // label shown on the sign
  String placeQuery = DEFAULT_PLACE_QUERY;    // what was typed, e.g. "Ames, IA, USA"
  String placeDetail = DEFAULT_PLACE_DETAIL;  // matched state and country
  bool showPlace = true;
  bool fahrenheit = true;

  bool busOn = false;     // show bus times (with weatherOn too: bus times, then weather)
  String transitKey;      // Transitland API key (never printed in full)
  String busStop;         // stop onestop_id, e.g. "s-9zmm1qqzhd-lincolnway~beachave"
  String busStopName;     // e.g. "Lincoln Way & Beach Ave (1092)"
  uint8_t busEvery = BUS_DEFAULT_EVERY_MIN;  // minutes between fetches
};

HardwareSerial SignSerial(1);
WebServer server(80);
Preferences prefs;
Settings settings;

bool sniffing = false;
bool bootMessagePending = true;
String consoleLine;

// Wi-Fi
uint32_t wifiOkAt = 0;       // last time home Wi-Fi was connected (or a connect began)
uint32_t reconnectAt = 0;    // non-zero: reconnect to home Wi-Fi at this time
bool apRunning = false;
bool mdnsStarted = false;

// Weather
weather::Forecast forecast;
bool haveForecast = false;
uint32_t lastFetchOkAt = 0;
uint32_t nextFetchAt = 0;
uint32_t retryDelay = WEATHER_RETRY_MS;
String lastWeatherError;
String lastSentWeather;  // what's on the sign now, so we only write on change
uint32_t lastWeatherSentAt = 0;

bool hasLocation() { return !isnan(settings.latitude) && !isnan(settings.longitude); }

// Bus times
transit::RouteList busKnown;  // lines that stop at the chosen stop
transit::RouteList busLines;  // lines picked to show (none = all of busKnown)
transit::Board busBoard;      // upcoming departures from the last fetch
bool haveBusBoard = false;
uint32_t busFetchOkAt = 0;
uint32_t busNextFetchAt = 0;
uint32_t busRetryDelay = BUS_RETRY_MS;
String lastBusError;
int64_t lastBusMinute = -1;
bool clockStarted = false;
transit::Stop busResults[transit::MAX_STOPS];  // last stop search
size_t busResultCount = 0;
String busSearchQuery;

bool clockSet() { return time(nullptr) > 1700000000; }  // NTP has answered
void showMessage(const String& text);
char routeBuf[transit::MAX_ROUTES * (transit::ID_LEN + transit::SHORT_LEN + transit::NAME_LEN + 3) + 1];

// ---------------------------------------------------------------- RS-485 I/O

// Driver-enable pin and polarity (runtime-configurable with /de, saved).
int dePin = RS485_DE_PIN;
bool deActiveHigh = RS485_DE_ACTIVE_HIGH;

void driverEnable(bool on) {
  if (dePin >= 0) digitalWrite(dePin, on == deActiveHigh ? HIGH : LOW);
}

// Sets up the driver-enable pin, releasing the previous one if it changed.
void applyDePin(int oldPin) {
  if (oldPin >= 0 && oldPin != dePin) pinMode(oldPin, INPUT);
  if (dePin >= 0) {
    pinMode(dePin, OUTPUT);
    driverEnable(false);  // listen by default
  }
}

void applySerialSettings() {
  SignSerial.end();
  if (settings.protocol == PROTO_J1587) {
    SignSerial.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);  // J1708 is fixed at 9600 8N1
  } else {
    SignSerial.begin(settings.baud, FORMATS[settings.format].config, RS485_RX_PIN, RS485_TX_PIN);
  }
}

void sendBytes(const uint8_t* data, size_t len) {
  driverEnable(true);
  SignSerial.write(data, len);
  SignSerial.flush();  // block until the last stop bit has left the UART
  driverEnable(false);
}

// Holds the RS-485 driver on and streams 0x00 bytes so a multimeter across
// A and B shows whether the board is really driving the bus.
void runTxTest(uint32_t seconds) {
  Serial.printf("Driving the bus for %lu s: measure DC volts between A and B now.\n",
                (unsigned long)seconds);
  static const uint8_t zeros[64] = {0};
  driverEnable(true);
  uint32_t start = millis();
  while (millis() - start < seconds * 1000UL) {
    SignSerial.write(zeros, sizeof(zeros));
    SignSerial.flush();
  }
  driverEnable(false);
  Serial.println("TX test done (driver off). Measure A-B again to compare.");
}

// Slowly toggles a GPIO (default: the RS-485 TX pin) with the driver on, so a
// multimeter across A and B shows whether that pin really reaches the
// transceiver's data input, and with which polarity.
void runTxFlip(int pin) {
  Serial.printf("Toggling GPIO%d every 2 s with the driver on. Watch DC volts between A and B.\n", pin);
  SignSerial.end();
  pinMode(pin, OUTPUT);
  driverEnable(true);
  for (int i = 0; i < 6; ++i) {
    bool high = (i % 2) == 0;
    digitalWrite(pin, high ? HIGH : LOW);
    Serial.printf("  GPIO%d %s -> note the A-B reading\n", pin, high ? "HIGH" : "LOW ");
    delay(2000);
  }
  driverEnable(false);
  pinMode(pin, INPUT);
  applySerialSettings();
  Serial.println("Flip test done.");
}

// ---------------------------------------------------------------- J1587

void printHex(const char* label, const uint8_t* data, size_t len) {
  Serial.printf("   [%s: %u bytes]", label, (unsigned)len);
  for (size_t i = 0; i < len; ++i) Serial.printf(" %02X", data[i]);
  Serial.println();
}

void clearRx() {
  while (SignSerial.available()) SignSerial.read();
}

// Collects bytes from the bus for up to `timeoutMs`. Stops early once a CTS
// frame has arrived (when `stopOnCts`), or after `quietMs` of silence once
// something has been received.
size_t readBus(uint8_t* buf, size_t cap, uint32_t timeoutMs, bool stopOnCts, uint32_t quietMs) {
  size_t n = 0;
  uint32_t start = millis();
  uint32_t lastByte = start;
  while (millis() - start < timeoutMs) {
    while (SignSerial.available() && n < cap) {
      buf[n++] = SignSerial.read();
      lastByte = millis();
    }
    if (stopOnCts && j1587::findCts(buf, n)) break;
    if (n > 0 && millis() - lastByte > quietMs) break;
    delay(2);
  }
  return n;
}

// RTS layout the sign answered for multi-segment messages (saved). Single-
// segment messages always use the exact known-good RTS.
uint8_t rtsLayout = j1587::RTS_FIXED;

void saveSettings();

// Sends an RTS in `layout` and waits for the sign's CTS. Returns true if the
// sign answered.
bool requestToSend(uint8_t layout, size_t segments, size_t plen) {
  uint8_t frame[j1587::MAX_FRAME];
  uint8_t rx[96];
  clearRx();
  delay(J1587_IDLE_MS);
  size_t n = j1587::buildRts(frame, layout, segments, plen);
  sendBytes(frame, n);
  clearRx();  // drop any echo of our own RTS

  size_t got = readBus(rx, sizeof(rx), J1587_CTS_TIMEOUT_MS, true, 60);
  printHex("sign CTS", rx, got);
  // The reference program only checks that ~10 bytes came back, so accept an
  // unrecognised reply of that size too, as long as it isn't our own echo.
  return j1587::findCts(rx, got) || (got >= 10 && rx[0] != j1587::MID_CONTROLLER);
}

// Sends pages to the sign with the J1587 transport: RTS, wait for the sign's
// CTS, then the data segments. Mirrors the known-good Linux program.
bool sendJ1587Pages(const j1587::Page* pages, size_t count) {
  uint8_t payload[j1587::MAX_PAYLOAD];
  size_t plen = j1587::buildPayloadPages(payload, sizeof(payload), pages, count);
  if (plen == 0) {
    Serial.printf("! message too long for the sign (max %u bytes)\n", (unsigned)j1587::MAX_PAYLOAD);
    return false;
  }
  size_t segments = j1587::segmentsFor(plen);

  // One segment: the exact known-good RTS. Several: the saved layout first,
  // then the others. A long message counts as delivered only when the sign
  // confirms it with an end-of-message frame; the layout that gets one is
  // remembered.
  uint8_t order[j1587::RTS_LAYOUT_COUNT];
  size_t tries = 0;
  if (segments == 1) {
    order[tries++] = j1587::RTS_FIXED;
  } else {
    order[tries++] = rtsLayout;
    for (uint8_t l = 0; l < j1587::RTS_LAYOUT_COUNT; ++l)
      if (l != rtsLayout) order[tries++] = l;
  }

  uint8_t frame[j1587::MAX_FRAME];
  uint8_t rx[96];
  bool anyCts = false;
  for (size_t t = 0; t < tries; ++t) {
    if (segments > 1) {
      Serial.printf("   RTS for %u segments, layout \"%s\"\n", (unsigned)segments, j1587::rtsLayoutName(order[t]));
    }
    if (!requestToSend(order[t], segments, plen)) continue;
    anyCts = true;

    // Turnaround pause, then the data segments.
    delay(J1587_TURNAROUND_MS);
    for (size_t i = 0; i < segments; ++i) {
      size_t n = j1587::buildSegment(frame, payload, plen, i);
      sendBytes(frame, n);
      if (i + 1 < segments) delay(J1587_SEGMENT_GAP_MS);
    }
    clearRx();

    size_t got = readBus(rx, sizeof(rx), J1587_ACK_WAIT_MS, false, 100);
    if (got) printHex("sign reply", rx, got);
    if (j1587::findEom(rx, got)) {
      Serial.printf("   sign confirmed the message (%u segment%s)\n", (unsigned)segments, segments == 1 ? "" : "s");
      if (segments > 1 && order[t] != rtsLayout) {
        rtsLayout = order[t];
        saveSettings();
        Serial.printf("   long messages use RTS layout \"%s\" from now on (saved)\n", j1587::rtsLayoutName(rtsLayout));
      }
      return true;
    }
    Serial.printf("   ! no end-of-message from the sign for %u segment%s\n", (unsigned)segments,
                  segments == 1 ? "" : "s");
    if (segments == 1) return true;  // matches the known-good program, which doesn't check
  }

  if (!anyCts) {
    Serial.println("! no CTS from the sign; check wiring, power and /protocol");
  } else {
    Serial.println("! the sign didn't confirm the long message with any RTS layout");
  }
  return false;
}

bool sendJ1587(const char* text, char mode) {
  j1587::Page page = {text, mode, 0, 0};
  return sendJ1587Pages(&page, 1);
}

bool sendPages(const alpha::Page* pages, size_t count, const alpha::TextOptions& opts) {
  if (settings.protocol == PROTO_J1587) {
    if (settings.j1587Mode) {
      // Same screens and modes as the Alpha path: each page keeps its own
      // mode field, so the weather screens each scroll right to left, one
      // after another.
      j1587::Page jp[weather::MAX_SCREENS];
      size_t n = count < weather::MAX_SCREENS ? count : weather::MAX_SCREENS;
      for (size_t i = 0; i < n; ++i) {
        int m = pages[i].mode;
        bool valid = m >= 0 && (size_t)m < alpha::MODE_COUNT;
        jp[i] = {pages[i].text, valid ? alpha::MODES[m].code : (char)0, valid ? alpha::MODES[m].special : (char)0,
                 opts.speed};
      }
      return sendJ1587Pages(jp, n);
    }
    // Fallback: plain text joined into one line, no mode codes.
    String joined;
    for (size_t i = 0; i < count; ++i) {
      if (i) joined += "    ";
      joined += pages[i].text;
    }
    j1587::Page plain = {joined.c_str(), 0, 0, opts.speed};
    return sendJ1587Pages(&plain, 1);
  }

  uint8_t packet[MAX_MESSAGE_LEN + 160];
  size_t len = alpha::buildWriteTextPages(packet, sizeof(packet), opts, pages, count);
  if (len == 0) {
    Serial.println("! message too long");
    return false;
  }
  sendBytes(packet, len);
  return true;
}

bool sendText(const char* text, const alpha::TextOptions& opts) {
  alpha::Page page = {text, opts.mode};
  return sendPages(&page, 1, opts);
}

alpha::TextOptions currentOptions() {
  alpha::TextOptions opts = settings.text;
  opts.fileLabel = settings.usePriority ? alpha::FILE_PRIORITY : alpha::FILE_DEFAULT;
  return opts;
}

// ---------------------------------------------------------------- persistence

void saveSettings() {
  prefs.putUInt("baud", settings.baud);
  prefs.putUChar("proto", settings.protocol);
  prefs.putBool("j1587mode", settings.j1587Mode);
  prefs.putUChar("rtslayout", rtsLayout);
  prefs.putInt("depin", dePin);
  prefs.putBool("dehigh", deActiveHigh);
  prefs.putString("addr", settings.text.address);
  prefs.putUChar("type", (uint8_t)settings.text.typeCode);
  prefs.putUChar("format", settings.format);
  prefs.putInt("mode", settings.text.mode);
  prefs.putInt("color", settings.text.color);
  prefs.putUChar("speed", settings.text.speed);
  prefs.putBool("checksum", settings.text.checksum);
  prefs.putBool("priority", settings.usePriority);
  prefs.putString("message", settings.message);
  prefs.putString("ssid", settings.wifiSsid);
  prefs.putString("wifipass", settings.wifiPassword);
  prefs.putBool("weather", settings.weatherOn);
  prefs.putDouble("lat", settings.latitude);
  prefs.putDouble("lon", settings.longitude);
  prefs.putString("place", settings.placeName);
  prefs.putString("placeq", settings.placeQuery);
  prefs.putString("placed", settings.placeDetail);
  prefs.putBool("showplace", settings.showPlace);
  prefs.putBool("fahrenheit", settings.fahrenheit);
  prefs.putBool("buson", settings.busOn);
  prefs.putString("buskey", settings.transitKey);
  prefs.putString("busstop", settings.busStop);
  prefs.putString("busstopn", settings.busStopName);
  prefs.putUChar("busevery", settings.busEvery);
  transit::serialize(busKnown, routeBuf, sizeof(routeBuf));
  prefs.putString("busknown", routeBuf);
  transit::serialize(busLines, routeBuf, sizeof(routeBuf));
  prefs.putString("buslines", routeBuf);
}

void loadSettings() {
  settings.baud = prefs.getUInt("baud", settings.baud);
  settings.format = prefs.getUChar("format", settings.format) % FORMAT_COUNT;
  settings.text.mode = prefs.getInt("mode", alpha::findMode("rotate"));
  settings.text.color = prefs.getInt("color", -1);
  settings.text.speed = prefs.getUChar("speed", 0);
  settings.text.checksum = prefs.getBool("checksum", false);
  settings.usePriority = prefs.getBool("priority", false);
  settings.message = prefs.getString("message", settings.message);
  settings.wifiSsid = prefs.getString("ssid", DEFAULT_WIFI_SSID);
  settings.wifiPassword = prefs.getString("wifipass", DEFAULT_WIFI_PASSWORD);
  settings.weatherOn = prefs.getBool("weather", settings.weatherOn);
  settings.latitude = prefs.getDouble("lat", settings.latitude);
  settings.longitude = prefs.getDouble("lon", settings.longitude);
  settings.placeName = prefs.getString("place", settings.placeName);
  if (prefs.isKey("placeq")) {
    settings.placeQuery = prefs.getString("placeq", settings.placeQuery);
    settings.placeDetail = prefs.getString("placed", settings.placeDetail);
  } else if (settings.placeName != DEFAULT_PLACE) {
    settings.placeQuery = settings.placeName;  // saved by an older version
    settings.placeDetail = "";
  }
  settings.showPlace = prefs.getBool("showplace", true);
  settings.fahrenheit = prefs.getBool("fahrenheit", true);
  settings.busOn = prefs.getBool("buson", false);
  settings.transitKey = prefs.getString("buskey", DEFAULT_TRANSIT_API_KEY);
  settings.busStop = prefs.getString("busstop", "");
  settings.busStopName = prefs.getString("busstopn", "");
  settings.busEvery = prefs.getUChar("busevery", BUS_DEFAULT_EVERY_MIN);
  if (settings.busEvery < 1) settings.busEvery = BUS_DEFAULT_EVERY_MIN;
  transit::deserialize(prefs.getString("busknown", "").c_str(), busKnown);
  transit::deserialize(prefs.getString("buslines", "").c_str(), busLines);
  settings.protocol = prefs.getUChar("proto", PROTO_J1587) == PROTO_ALPHA ? PROTO_ALPHA : PROTO_J1587;
  settings.j1587Mode = prefs.getBool("j1587mode", true);
  rtsLayout = prefs.getUChar("rtslayout", j1587::RTS_FIXED) % j1587::RTS_LAYOUT_COUNT;
  dePin = prefs.getInt("depin", RS485_DE_PIN);
  deActiveHigh = prefs.getBool("dehigh", RS485_DE_ACTIVE_HIGH);
  String addr = prefs.getString("addr", DEFAULT_SIGN_ADDRESS);
  if (addr.length() == 2) {
    settings.text.address[0] = addr[0];
    settings.text.address[1] = addr[1];
  }
  settings.text.typeCode = (char)prefs.getUChar("type", (uint8_t)alpha::TYPE_ALL);
  if (settings.text.mode >= (int)alpha::MODE_COUNT) settings.text.mode = 0;
  if (settings.text.color >= (int)alpha::COLOR_COUNT) settings.text.color = -1;
}

// ---------------------------------------------------------------- Wi-Fi

void connectHomeWifi() {
  WiFi.disconnect();
  wifiOkAt = millis();
  if (settings.wifiSsid.length()) {
    Serial.printf("Connecting to Wi-Fi \"%s\"...\n", settings.wifiSsid.c_str());
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
  }
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  apRunning = true;
  Serial.printf("Home Wi-Fi unavailable. Join \"%s\" (password \"%s\") and open http://%s\n",
                AP_SSID, AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

void stopAccessPoint() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apRunning = false;
}

// Starts the fallback AP when home Wi-Fi is missing, and turns it off again
// once home Wi-Fi is up and nobody is using the AP.
void pollWifi() {
  if (reconnectAt && (int32_t)(millis() - reconnectAt) >= 0) {
    reconnectAt = 0;
    connectHomeWifi();
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) wifiOkAt = millis();
  if (connected && !mdnsStarted) {
    mdnsStarted = MDNS.begin(MDNS_NAME);
    if (mdnsStarted) MDNS.addService("http", "tcp", 80);
    Serial.printf("Wi-Fi connected: http://%s.local or http://%s\n", MDNS_NAME,
                  WiFi.localIP().toString().c_str());
  }
  if (connected && !clockStarted) {
    // Internet time, for counting down to the next bus.
    clockStarted = true;
    configTzTime(BUS_TIMEZONE, "pool.ntp.org", "time.nist.gov");
  }
  if (!connected && !apRunning && millis() - wifiOkAt > WIFI_FALLBACK_MS) startAccessPoint();
  if (connected && apRunning && WiFi.softAPgetStationNum() == 0) stopAccessPoint();
}

void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  connectHomeWifi();
  if (!settings.wifiSsid.length()) startAccessPoint();
}

// ---------------------------------------------------------------- HTTP

// Fetches `url` into `body`. Open-Meteo data is public and we send nothing
// secret, so certificate checking is skipped to avoid maintaining a CA bundle.
// (Transitland requests skip it too; see transitGet.)
bool httpGet(const char* url, String& body, String& error) {
  if (WiFi.status() != WL_CONNECTED) {
    error = "no Wi-Fi";
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);  // plain (non-chunked) response body
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    error = "bad URL";
    return false;
  }
  int code = http.GET();
  bool ok = code == HTTP_CODE_OK;
  if (ok) body = http.getString();
  else error = code < 0 ? http.errorToString(code) : "HTTP " + String(code);
  http.end();
  return ok;
}

// Streams a Transitland response into `parser` a few hundred bytes at a time,
// so large responses never have to fit in RAM. The API key is sent in a
// header, not the URL. Certificates aren't checked (as for Open-Meteo); the
// key only gives read access to public timetables, but anyone able to
// intercept your Wi-Fi traffic could copy it.
bool transitGet(const char* url, jsonstream::Parser& parser, String& error) {
  if (!settings.transitKey.length()) {
    error = "no Transitland API key";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    error = "no Wi-Fi";
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);  // plain (non-chunked) response body
  http.setTimeout(15000);
  if (!http.begin(client, url)) {
    error = "bad URL";
    return false;
  }
  http.addHeader("apikey", settings.transitKey);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code == 401 || code == 403) error = "API key rejected (HTTP " + String(code) + ")";
    else if (code == 429) error = "too many requests (HTTP 429); try checking less often";
    else error = code < 0 ? http.errorToString(code) : "HTTP " + String(code);
    http.end();
    return false;
  }
  auto* stream = http.getStreamPtr();
  int remaining = http.getSize();  // -1 if the server didn't say
  char buf[256];
  uint32_t lastData = millis();
  while (stream && (http.connected() || stream->available()) && !parser.done() && !parser.failed()) {
    int avail = stream->available();
    if (avail <= 0) {
      if (millis() - lastData > 15000) {
        error = "timed out";
        break;
      }
      delay(2);
      continue;
    }
    int n = stream->readBytes(buf, std::min<int>(avail, (int)sizeof(buf)));
    if (n <= 0) continue;
    parser.feed(buf, (size_t)n);
    lastData = millis();
    if (remaining > 0 && (remaining -= n) <= 0) break;
  }
  http.end();
  if (parser.done()) return true;
  if (error.isEmpty()) error = parser.failed() ? "unexpected response" : "response cut off";
  return false;
}

// ---------------------------------------------------------------- weather

bool fetchWeather() {
  if (!hasLocation()) {
    lastWeatherError = "no location set";
    return false;
  }
  char url[512];
  if (!weather::forecastUrl(url, sizeof(url), settings.latitude, settings.longitude, settings.fahrenheit)) {
    lastWeatherError = "URL too long";
    return false;
  }
  String body;
  String error;
  weather::Forecast f;
  if (!httpGet(url, body, error)) {
    lastWeatherError = error;
  } else if (!weather::parseForecast(body.c_str(), f)) {
    lastWeatherError = "unexpected response";
  } else {
    forecast = f;
    haveForecast = true;
    lastFetchOkAt = millis();
    lastWeatherError = "";
    Serial.println("Weather updated");
    return true;
  }
  Serial.printf("! weather fetch failed: %s\n", lastWeatherError.c_str());
  return false;
}

// Looks up "City", "City, State", "City, Country" or "City, State, Country"
// with Open-Meteo geocoding and makes the best match the weather location.
// See Geocode.h for the accepted state and country spellings.
bool setLocationByName(const String& input, String& error) {
  geo::Query q;
  if (!geo::parseQuery(input.c_str(), q)) {
    error = "enter a city, e.g. Ames, Iowa, US";
    return false;
  }
  static geo::Place places[geo::MAX_RESULTS];  // static: keep it off the stack
  char url[320];
  String body;
  size_t n = 0;

  // With a recognised country, ask Open-Meteo to search only that country
  // first; fall back to a plain search if that fails or finds nothing.
  bool byCountry = geo::queryCountryCode(q) != nullptr;
  if (byCountry) {
    geo::searchUrl(url, sizeof(url), q, true);
    String err;
    if (httpGet(url, body, err)) n = geo::parsePlaces(body.c_str(), places, geo::MAX_RESULTS);
  }
  if (n == 0) {
    geo::searchUrl(url, sizeof(url), q, false);
    if (!httpGet(url, body, error)) return false;
    n = geo::parsePlaces(body.c_str(), places, geo::MAX_RESULTS);
  }
  if (n == 0) {
    error = String("no place called \"") + q.city + "\"";
    return false;
  }

  int i = geo::pick(q, places, n);
  if (i < 0) {
    error = String("found \"") + q.city + "\" but not in that state/country. Did you mean: ";
    char detail[2 * geo::FIELD_LEN + 4];
    for (size_t k = 0; k < n && k < 4; ++k) {
      geo::describePlace(places[k], detail, sizeof(detail));
      error += String(k ? "; " : "") + places[k].name + ", " + detail;
    }
    return false;
  }

  const geo::Place& pl = places[i];
  char detail[2 * geo::FIELD_LEN + 4];
  geo::describePlace(pl, detail, sizeof(detail));
  settings.latitude = pl.latitude;
  settings.longitude = pl.longitude;
  settings.placeName = pl.name;
  settings.placeDetail = detail;
  settings.placeQuery = input;
  settings.placeQuery.trim();
  Serial.printf("Location: %s, %s (%.4f, %.4f)\n", pl.name, detail, pl.latitude, pl.longitude);
  return true;
}

// Bus times for the sign (all chosen lines on one line), or what's missing.
void busText(char* out, size_t cap) {
  uint32_t staleMs = std::max<uint32_t>(BUS_STALE_MIN_MS, 3UL * settings.busEvery * 60 * 1000);
  const transit::RouteList& chosen = busLines.count ? busLines : busKnown;
  if (!settings.transitKey.length()) {
    snprintf(out, cap, "SET BUS API KEY");
  } else if (!settings.busStop.length()) {
    snprintf(out, cap, "PICK A BUS STOP");
  } else if (!haveBusBoard || !clockSet()) {
    snprintf(out, cap, "LOADING BUS TIMES");
  } else if (millis() - busFetchOkAt > staleMs) {
    snprintf(out, cap, "BUS TIMES OFFLINE");
  } else if (!chosen.count) {
    snprintf(out, cap, "NO BUSES AT THIS STOP");
  } else {
    int64_t now = (int64_t)time(nullptr);
    now -= now % 60;  // whole minutes, so the sign changes at most once a minute
    transit::formatArrivals(busBoard, chosen, now, out, cap);
  }
}

// Shows bus times and/or the weather, whichever are on. Bus times come first
// (they change every minute and the sign starts over on each update), then
// the weather screens.
void showData(bool force) {
  char lines[weather::MAX_PAGES][weather::PAGE_LEN];
  size_t count = 0;
  char busLine[j1587::MAX_PAYLOAD];
  busLine[0] = '\0';
  if (settings.busOn) busText(busLine, sizeof(busLine) - 16);

  bool stale = haveForecast && millis() - lastFetchOkAt > WEATHER_STALE_MS;
  if (!settings.weatherOn) {
    count = 0;
  } else if (!hasLocation()) {
    strcpy(lines[count++], "SET LOCATION");
  } else if (!haveForecast) {
    strcpy(lines[count++], "LOADING WEATHER");
  } else if (stale) {
    strcpy(lines[count++], "WEATHER OFFLINE");
  } else {
    String label = settings.showPlace ? settings.placeName : String();
    count = weather::formatPages(forecast, label.c_str(), settings.fahrenheit, lines,
                                 weather::MAX_PAGES);
  }

  // Each screen is shown in turn. Held screens must fit the display, so wide
  // ones are split into several screens first.
  int mode = alpha::findMode(WEATHER_SCREEN_MODE);
  bool held = mode != alpha::findMode("rotate") && mode != alpha::findMode("compressed");
  char screens[weather::MAX_SCREENS][weather::PAGE_LEN];
  size_t screenCount = 0;
  for (size_t i = 0; i < count; ++i) {
    if (held) {
      screenCount = weather::fitScreens(lines[i], SIGN_WIDTH_CHARS, screens, screenCount, weather::MAX_SCREENS);
    } else if (screenCount < weather::MAX_SCREENS) {
      strcpy(screens[screenCount++], lines[i]);
    }
  }
  alpha::Page pages[weather::MAX_SCREENS + 1];
  count = 0;
  if (busLine[0]) pages[count++] = {busLine, alpha::findMode("rotate")};
  for (size_t i = 0; i < screenCount; ++i) pages[count++] = {screens[i], mode};

  // Keep within what the sign takes in one message: drop weather screens
  // from the end if bus times plus weather would be too long.
  auto payloadSize = [&](size_t n) {
    size_t total = 3;  // command, file label, EOT
    for (size_t i = 0; i < n; ++i) total += 5 + strlen(pages[i].text);  // mode field + speed
    return total;
  };
  while (count > 1 && payloadSize(count) > j1587::MAX_PAYLOAD) --count;

  if (!count) return;
  String summary;
  for (size_t i = 0; i < count; ++i) summary += (i ? " | " : "") + String(pages[i].text);

  // Writing a TEXT file blanks the sign briefly, so only write on change
  // (plus an hourly refresh in case the sign was power cycled on its own).
  bool due = millis() - lastWeatherSentAt > WEATHER_RESEND_MS;
  if (!force && !due && summary == lastSentWeather) return;
  if (sendPages(pages, count, currentOptions())) {
    lastSentWeather = summary;
    lastWeatherSentAt = millis();
    Serial.printf("-> sign: %s\n", summary.c_str());
  }
}

void setWeatherMode(bool on) {
  settings.weatherOn = on;
  saveSettings();
  if (on) {
    nextFetchAt = millis();  // fetch right away
    showData(true);
  }
}

// Chooses what the sign shows: weather, bus times, both, or (neither) the message.
void setDisplay(bool weatherOn, bool busOn) {
  settings.weatherOn = weatherOn;
  settings.busOn = busOn;
  saveSettings();
  if (weatherOn) nextFetchAt = millis();
  if (busOn) {
    busNextFetchAt = millis();
    busRetryDelay = BUS_RETRY_MS;
  }
  if (weatherOn || busOn) showData(true);
  else showMessage(settings.message);
}

const char* displayName() {
  if (settings.busOn && settings.weatherOn) return "bus times, then weather";
  if (settings.busOn) return "bus times";
  if (settings.weatherOn) return "weather";
  return "message";
}

void pollWeather() {
  if (!settings.weatherOn || !hasLocation() || WiFi.status() != WL_CONNECTED) return;
  if ((int32_t)(millis() - nextFetchAt) < 0) return;

  if (fetchWeather()) {
    retryDelay = WEATHER_RETRY_MS;
    nextFetchAt = millis() + WEATHER_INTERVAL_MS;
  } else {
    nextFetchAt = millis() + retryDelay;
    retryDelay = std::min<uint32_t>(retryDelay * 2, WEATHER_INTERVAL_MS);
  }
  showData(false);
}

// ---------------------------------------------------------------- bus times

// Local wall-clock seconds (as if UTC) to Unix time, using BUS_TIMEZONE. Only
// needed when a timetable gives local times without a UTC offset.
int64_t wallToEpoch(int64_t wall) {
  time_t w = (time_t)wall;
  struct tm tm;
  gmtime_r(&w, &tm);
  tm.tm_isdst = -1;  // let the time zone rules decide
  return (int64_t)mktime(&tm);
}

uint32_t busIntervalMs() { return (uint32_t)settings.busEvery * 60UL * 1000; }
long busRequestsPerMonth(uint8_t every) { return 30L * 24 * 60 / (every ? every : 1); }

// Keeps only the picked lines that still stop at the stop.
void prunePickedLines() {
  static transit::RouteList kept;
  kept.clear();
  for (size_t i = 0; i < busLines.count; ++i) {
    int k = busKnown.find(busLines.items[i].id);
    if (k >= 0) kept.add(busKnown.items[k]);
  }
  busLines = kept;
}

// Downloads departures from `stopId` over `windowS` into `board`.
bool fetchDepartures(const char* stopId, long windowS, int limit, transit::Board& board, String& error) {
  char url[320];
  if (!transit::departuresUrl(url, sizeof(url), stopId, windowS, limit)) {
    error = "stop id too long";
    return false;
  }
  transit::DepartureReader reader(board);
  jsonstream::Parser parser(reader);
  if (!transitGet(url, parser, error)) return false;
  if (!board.stopFound) {
    error = "Transitland doesn't know that stop";
    return false;
  }
  transit::resolveLocalTimes(board, wallToEpoch);
  return true;
}

bool fetchBus() {
  static transit::Board fresh;  // static: a few KB, keep it off the stack
  String error;
  if (!fetchDepartures(settings.busStop.c_str(), BUS_WINDOW_S, BUS_DEPARTURE_LIMIT, fresh, error)) {
    lastBusError = error;
    Serial.printf("! bus times fetch failed: %s\n", lastBusError.c_str());
    return false;
  }
  busBoard = fresh;
  haveBusBoard = true;
  busFetchOkAt = millis();
  lastBusError = "";
  // Lines seen now (e.g. a new route) are added to the stop's list.
  size_t before = busKnown.count;
  for (size_t i = 0; i < fresh.routes.count; ++i) busKnown.add(fresh.routes.items[i]);
  if (busKnown.count != before) {
    transit::sortRoutes(busKnown);
    saveSettings();
  }
  Serial.printf("Bus times updated (%u departures in the next %ld min)\n", (unsigned)fresh.count,
                BUS_WINDOW_S / 60);
  return true;
}

// Makes `stopId` the bus stop and finds the lines that serve it (from its
// departures over the next 24 hours).
bool selectStop(const String& stopId, String& error) {
  String id = stopId;
  id.trim();
  if (id.isEmpty()) {
    error = "no stop chosen";
    return false;
  }
  static transit::Board day;
  if (!fetchDepartures(id.c_str(), BUS_LINES_WINDOW_S, BUS_LINES_LIMIT, day, error)) return false;
  bool sameStop = id == settings.busStop;
  settings.busStop = id;
  settings.busStopName = day.stopName[0] ? String(day.stopName) : id;
  if (day.stopCode[0]) settings.busStopName += " (" + String(day.stopCode) + ")";
  busKnown = day.routes;
  transit::sortRoutes(busKnown);
  if (sameStop) prunePickedLines();
  else busLines.clear();
  busBoard = day;
  haveBusBoard = true;
  busFetchOkAt = millis();
  busNextFetchAt = millis() + busIntervalMs();
  lastBusError = "";
  busResultCount = 0;
  saveSettings();
  Serial.printf("Bus stop: %s, %u line%s\n", settings.busStopName.c_str(), (unsigned)busKnown.count,
                busKnown.count == 1 ? "" : "s");
  return true;
}

// Searches Transitland for stops named like `query` near the weather location.
bool searchStops(const String& query, String& error) {
  char url[400];
  double lat = hasLocation() ? settings.latitude : NAN;
  double lon = hasLocation() ? settings.longitude : NAN;
  if (!transit::stopSearchUrl(url, sizeof(url), query.c_str(), lat, lon, BUS_SEARCH_RADIUS_M,
                              (int)transit::MAX_STOPS)) {
    error = "search text too long";
    return false;
  }
  transit::StopReader reader(busResults, transit::MAX_STOPS);
  jsonstream::Parser parser(reader);
  busResultCount = 0;
  if (!transitGet(url, parser, error)) return false;
  busResultCount = reader.count;
  busSearchQuery = query;
  return true;
}

String routeName(const transit::Route& r) {
  char label[transit::LABEL_LEN + 1];
  transit::routeLabel(r, label, sizeof(label));
  return label;
}

String busLinesSummary() {
  if (!busKnown.count) return "none found";
  const transit::RouteList& list = busLines.count ? busLines : busKnown;
  String s;
  for (size_t i = 0; i < list.count; ++i) s += (i ? ", " : "") + routeName(list.items[i]);
  if (!busLines.count) s += " (all)";
  return s;
}

String busStatus() {
  if (!settings.transitKey.length()) return "no API key yet";
  if (!settings.busStop.length()) return "no stop chosen";
  String s = settings.busStopName + "; lines: " + busLinesSummary();
  s += "; checks every " + String((unsigned)settings.busEvery) + " min";
  if (haveBusBoard) s += ", updated " + String((millis() - busFetchOkAt) / 60000) + " min ago";
  if (!clockSet()) s += ", waiting for the clock";
  if (lastBusError.length()) s += ", last error: " + lastBusError;
  return s;
}

void pollBus() {
  if (!settings.busOn || bootMessagePending) return;
  bool canFetch = WiFi.status() == WL_CONNECTED && settings.transitKey.length() && settings.busStop.length();
  if (canFetch && (int32_t)(millis() - busNextFetchAt) >= 0) {
    if (fetchBus()) {
      busRetryDelay = BUS_RETRY_MS;
      busNextFetchAt = millis() + busIntervalMs();
    } else {
      busNextFetchAt = millis() + busRetryDelay;
      busRetryDelay = std::min<uint32_t>(busRetryDelay * 2, busIntervalMs());
    }
    showData(false);
  }
  // Count down once a minute between fetches.
  if (clockSet()) {
    int64_t minute = (int64_t)time(nullptr) / 60;
    if (minute != lastBusMinute) {
      lastBusMinute = minute;
      showData(false);
    }
  }
}

// ---------------------------------------------------------------- display API

// Shows a message with the current mode/colour/speed, remembers it, and
// switches out of weather mode.
void showMessage(const String& text) {
  settings.message = text.substring(0, MAX_MESSAGE_LEN);
  settings.weatherOn = false;
  settings.busOn = false;
  if (sendText(settings.message.c_str(), currentOptions())) {
    saveSettings();
    Serial.printf("-> sign: \"%s\"\n", settings.message.c_str());
  }
}

// Re-sends whatever the sign should be showing (after a setting change).
void refreshDisplay() {
  if (settings.weatherOn || settings.busOn) showData(true);
  else showMessage(settings.message);
}

// Writing an empty Priority TEXT file returns the sign to its normal files
// (protocol manual, section 6.1.3).
void clearPriority() {
  alpha::TextOptions opts = settings.text;
  opts.fileLabel = alpha::FILE_PRIORITY;
  opts.mode = -1;
  opts.color = -1;
  opts.speed = 0;
  sendText("", opts);
  Serial.println("-> sign: priority message cleared");
}

// ---------------------------------------------------------------- probe & sniff

// Cycles through baud rates and serial formats, showing the settings on the
// sign. Watch the sign: whichever text appears tells you what works.
void runProbe() {
  static const uint32_t BAUDS[] = {9600, 4800, 2400, 1200, 19200, 38400};
  static const char* ADDRESSES[] = {"01", "00"};
  uint32_t savedBaud = settings.baud;
  uint8_t savedFormat = settings.format;

  alpha::TextOptions opts;
  opts.mode = alpha::findMode("hold");
  opts.typeCode = settings.text.typeCode;

  Serial.println("Probing; watch the sign. Press Enter in this monitor to stop.");
  for (size_t f = 0; f < FORMAT_COUNT; ++f) {
    for (uint32_t baud : BAUDS) {
      for (const char* addr : ADDRESSES) {
        settings.baud = baud;
        settings.format = f;
        applySerialSettings();
        opts.address[0] = addr[0];
        opts.address[1] = addr[1];
        char text[32];
        snprintf(text, sizeof(text), "%lu %s A%s", (unsigned long)baud, FORMATS[f].name, addr);
        Serial.printf("  trying %s\n", text);
        sendText(text, opts);

        uint32_t start = millis();
        while (millis() - start < 4000) {
          server.handleClient();
          if (Serial.available()) {
            while (Serial.available()) Serial.read();
            Serial.printf("Stopped at %s. Keep it with /baud %lu, /fmt %s and /addr %s.\n", text,
                          (unsigned long)baud, FORMATS[f].name, addr);
            settings.baud = savedBaud;
            settings.format = savedFormat;
            applySerialSettings();
            return;
          }
          delay(10);
        }
      }
    }
  }
  settings.baud = savedBaud;
  settings.format = savedFormat;
  applySerialSettings();
  Serial.println("Probe finished. Nothing shown? Swap the A/B wires and run /probe again.");
}

// Prints anything received on the RS-485 bus as hex + ASCII. Useful for
// capturing what the bus's original controller sends.
void pollSniffer() {
  static uint8_t line[16];
  static size_t n = 0;
  static uint32_t lastByte = 0;

  while (SignSerial.available()) {
    line[n++] = SignSerial.read();
    lastByte = millis();
    if (n == sizeof(line)) break;
  }
  bool idle = n > 0 && millis() - lastByte > 50;
  if (n == sizeof(line) || idle) {
    for (size_t i = 0; i < sizeof(line); ++i) {
      if (i < n) Serial.printf("%02X ", line[i]);
      else Serial.print("   ");
    }
    Serial.print(" |");
    for (size_t i = 0; i < n; ++i) Serial.print(isprint(line[i]) ? (char)line[i] : '.');
    Serial.println("|");
    n = 0;
  }
}

// ---------------------------------------------------------------- console

String weatherStatus() {
  if (!hasLocation()) return "no location set";
  String s = settings.placeName.length() ? settings.placeName : String("custom");
  if (settings.placeDetail.length()) s += ", " + settings.placeDetail;
  s += " (" + String(settings.latitude, 4) + ", " + String(settings.longitude, 4) + "), ";
  s += settings.fahrenheit ? "F" : "C";
  if (haveForecast) s += ", updated " + String((millis() - lastFetchOkAt) / 60000) + " min ago";
  if (lastWeatherError.length()) s += ", last error: " + lastWeatherError;
  return s;
}

void printStatus() {
  const alpha::TextOptions& t = settings.text;
  Serial.printf("display  : %s\n", displayName());
  Serial.printf("weather  : %s\n", weatherStatus().c_str());
  Serial.printf("bus      : %s\n", busStatus().c_str());
  Serial.printf("message  : \"%s\"\n", settings.message.c_str());
  Serial.printf("mode     : %s\n", t.mode >= 0 ? alpha::MODES[t.mode].name : "(none)");
  Serial.printf("color    : %s\n", t.color >= 0 ? alpha::COLORS[t.color].name : "(sign default)");
  Serial.printf("speed    : %s\n", t.speed ? String(t.speed).c_str() : "(sign default)");
  Serial.printf("priority : %s\n", settings.usePriority ? "on" : "off");
  if (settings.protocol == PROTO_J1587) {
    Serial.printf("protocol : J1587 (9600 8N1, MID %02X -> %02X), screen modes %s, long-message RTS %s\n",
                  j1587::MID_CONTROLLER, j1587::MID_SIGN,
                  settings.j1587Mode ? "on (scrolling screens)" : "off (plain text)",
                  j1587::rtsLayoutName(rtsLayout));
  } else {
    Serial.printf("protocol : Alpha, serial %lu %s, checksum %s\n", (unsigned long)settings.baud,
                  FORMATS[settings.format].name, t.checksum ? "on" : "off");
  }
  Serial.printf("address  : %c%c (sign type %c)\n", t.address[0], t.address[1], t.typeCode);
  if (dePin >= 0) {
    Serial.printf("rs485    : RX %d, TX %d, DE %d active %s\n", RS485_RX_PIN, RS485_TX_PIN, dePin,
                  deActiveHigh ? "HIGH" : "LOW");
  } else {
    Serial.printf("rs485    : RX %d, TX %d, no DE pin\n", RS485_RX_PIN, RS485_TX_PIN);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi     : \"%s\" at %s (http://%s.local)\n", settings.wifiSsid.c_str(),
                  WiFi.localIP().toString().c_str(), MDNS_NAME);
  } else {
    Serial.printf("wifi     : not connected%s\n",
                  settings.wifiSsid.length() ? (" to \"" + settings.wifiSsid + "\"").c_str()
                                             : " (set with /ssid and /pass)");
  }
  if (apRunning) {
    Serial.printf("setup AP : \"%s\" at http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
}

void printHelp() {
  Serial.println(
      "Type text and press Enter to show it on the sign (turns weather off), or:\n"
      "\n"
      " Wi-Fi\n"
      "  /ssid <network name>  home Wi-Fi network\n"
      "  /pass <password>      home Wi-Fi password, then connects\n"
      "\n"
      " Weather\n"
      "  /location <city>[, <state>][, <country>]  e.g. /location Ames, Iowa, US\n"
      "                     (states: name or IA/ON/NSW; countries: US, USA, United States...)\n"
      "  /latlon <lat> <lon> [label]  set coordinates directly\n"
      "  /units f|c            Fahrenheit or Celsius\n"
      "  /showplace on|off     include the place name in the cycle\n"
      "  /weather on|off       switch between weather and your message\n"
      "  /update               fetch the weather now\n"
      "\n"
      " Bus times (Transitland)\n"
      "  /buskey <key>         save your Transitland API key\n"
      "  /bussearch <name>     find stops near the weather location, e.g. /bussearch Beach\n"
      "  /busstop <n|id>       use stop n from the search (or a stop onestop_id)\n"
      "  /buslines [all|n n..] list the stop's lines, or pick which to show\n"
      "  /bus on|both|off      bus times only / bus times then weather / off\n"
      "  /busevery <minutes>   how often to check (5 = about 8,600 requests a month)\n"
      "  /busupdate            fetch bus times now\n"
      "\n"
      " Display\n"
      "  /mode <name>       rotate hold flash rollup rolldown rollleft rollright\n"
      "                     wipeup wipedown wipeleft wiperight scroll auto rollin\n"
      "                     rollout wipein wipeout compressed twinkle sparkle snow\n"
      "                     interlock switch slide spray starburst\n"
      "  /color <name|none> red green amber dimred dimgreen brown orange yellow\n"
      "                     rainbow1 rainbow2 mix auto (red-only signs ignore this)\n"
      "  /speed <0-5>       1 = slowest, 5 = fastest, 0 = sign default\n"
      "  /priority on|off   use the priority file (max 125 bytes, overrides all)\n"
      "  /clear             clear the priority message\n"
      "\n"
      " Sign link\n"
      "  /protocol j1587|alpha  how text is sent (j1587 = TDR80x7 transit sign)\n"
      "  /j1587test         send \"HELLO\" exactly like the known-good Linux program\n"
      "  /j1587mode on|off  on: each screen keeps its mode (weather scrolls right to\n"
      "                     left, one screen after another); off: plain text fallback\n"
      "  (the commands below apply to Alpha protocol only, except /de, /txtest, /txflip, /sniff)\n"
      "  /baud <rate>       1200 2400 4800 9600 19200 38400\n"
      "  /fmt 7E2|8N1       serial framing\n"
      "  /checksum on|off   append a checksum to each packet\n"
      "  /addr <00-FF>      sign address (shown at power-up; 00 = broadcast)\n"
      "  /type <char>       sign type code (Z = all signs, 1 = one-line signs)\n"
      "  /probe             try every baud/format/address combination\n"
      "  /de <pin|-1> [high|low]  RS-485 driver-enable pin and polarity\n"
      "  /txtest [seconds]  drive the bus for a multimeter check (default 10 s)\n"
      "  /txflip [gpio]     toggle the TX pin slowly for a multimeter check\n"
      "  /sniff             toggle printing bytes received from the bus\n"
      "\n"
      "  /resend  /status  /help\n");
}

void handleCommand(String line) {
  line.trim();
  if (line.isEmpty()) return;
  if (line[0] != '/') {
    showMessage(line);
    return;
  }

  int space = line.indexOf(' ');
  String cmd = space < 0 ? line : line.substring(0, space);
  String arg = space < 0 ? "" : line.substring(space + 1);
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "/help") {
    printHelp();
  } else if (cmd == "/status") {
    printStatus();

  } else if (cmd == "/ssid") {
    settings.wifiSsid = arg;
    saveSettings();
    connectHomeWifi();
  } else if (cmd == "/pass") {
    settings.wifiPassword = arg;
    saveSettings();
    connectHomeWifi();

  } else if (cmd == "/location") {
    if (arg.isEmpty()) return (void)Serial.println("! usage: /location <city>, <state>, <country>");
    String error;
    if (!setLocationByName(arg, error)) return (void)Serial.printf("! %s\n", error.c_str());
    haveForecast = false;
    setWeatherMode(true);
  } else if (cmd == "/latlon") {
    char label[48] = "";
    double lat, lon;
    int got = sscanf(arg.c_str(), "%lf %lf %47[^\n]", &lat, &lon, label);
    if (got < 2 || fabs(lat) > 90 || fabs(lon) > 180)
      return (void)Serial.println("! usage: /latlon <lat> <lon> [label], e.g. /latlon 43.04 -87.91 HOME");
    settings.latitude = lat;
    settings.longitude = lon;
    settings.placeName = label;
    settings.placeName.toUpperCase();
    settings.placeQuery = "";
    settings.placeDetail = "";
    haveForecast = false;
    setWeatherMode(true);
  } else if (cmd == "/units") {
    settings.fahrenheit = !arg.equalsIgnoreCase("c");
    haveForecast = false;  // values are in the old unit until the next fetch
    setWeatherMode(settings.weatherOn);
  } else if (cmd == "/showplace") {
    settings.showPlace = arg.equalsIgnoreCase("on");
    saveSettings();
    if (settings.weatherOn) showData(true);
  } else if (cmd == "/weather") {
    setDisplay(arg.equalsIgnoreCase("on"), settings.busOn);
  } else if (cmd == "/buskey") {
    if (arg.isEmpty()) return (void)Serial.println("! usage: /buskey <Transitland API key>");
    settings.transitKey = arg;
    saveSettings();
    busNextFetchAt = millis();
    Serial.println("API key saved.");
  } else if (cmd == "/bussearch") {
    if (arg.isEmpty()) return (void)Serial.println("! usage: /bussearch <stop name>");
    String error;
    if (!searchStops(arg, error)) return (void)Serial.printf("! %s\n", error.c_str());
    if (!busResultCount) return (void)Serial.println("No stops found. Try a shorter name or a street.");
    for (size_t i = 0; i < busResultCount; ++i) {
      const transit::Stop& st = busResults[i];
      Serial.printf("  %2u  %s%s%s%s%s  [%s]\n", (unsigned)(i + 1), st.name, st.code[0] ? " (" : "", st.code,
                    st.code[0] ? ")" : "", st.routes[0] ? (String("  lines ") + st.routes).c_str() : "", st.id);
    }
    Serial.println("Pick one with /busstop <number>.");
  } else if (cmd == "/busstop") {
    String id = arg;
    long n = arg.toInt();
    if (n >= 1 && (size_t)n <= busResultCount && String(n) == arg) id = busResults[n - 1].id;
    String error;
    if (!selectStop(id, error)) return (void)Serial.printf("! %s\n", error.c_str());
    handleCommand("/buslines");
  } else if (cmd == "/buslines") {
    if (arg.equalsIgnoreCase("all")) {
      busLines.clear();
      saveSettings();
    } else if (arg.length()) {
      busLines.clear();
      for (const char* p = arg.c_str(); *p;) {
        char* end;
        long n = strtol(p, &end, 10);
        if (end == p) {
          ++p;
          continue;
        }
        if (n >= 1 && (size_t)n <= busKnown.count) busLines.add(busKnown.items[n - 1]);
        p = end;
      }
      saveSettings();
    }
    if (!busKnown.count) return (void)Serial.println("No lines known yet; pick a stop with /bussearch and /busstop.");
    for (size_t i = 0; i < busKnown.count; ++i) {
      bool picked = busLines.find(busKnown.items[i].id) >= 0;
      Serial.printf("  %2u %s %s\n", (unsigned)(i + 1), picked ? "[x]" : "[ ]", routeName(busKnown.items[i]).c_str());
    }
    Serial.printf("Showing: %s. Change with /buslines 1 3 (or all).\n", busLinesSummary().c_str());
    if (settings.busOn) showData(true);
  } else if (cmd == "/bus") {
    if (arg.equalsIgnoreCase("on")) setDisplay(false, true);
    else if (arg.equalsIgnoreCase("both")) setDisplay(true, true);
    else if (arg.equalsIgnoreCase("off")) setDisplay(settings.weatherOn, false);
    else return (void)Serial.println("! usage: /bus on|both|off");
  } else if (cmd == "/busevery") {
    long m = arg.toInt();
    if (m < 1 || m > 60) return (void)Serial.println("! usage: /busevery <1-60 minutes>");
    settings.busEvery = (uint8_t)m;
    saveSettings();
    Serial.printf("Checking every %ld min, about %ld requests a month%s.\n", m, busRequestsPerMonth((uint8_t)m),
                  busRequestsPerMonth((uint8_t)m) > BUS_FREE_QUOTA ? " (more than the free plan allows)" : "");
  } else if (cmd == "/busupdate") {
    busNextFetchAt = millis();
    busRetryDelay = BUS_RETRY_MS;
    if (!settings.busOn) Serial.println("(bus times are off; /bus on to show them)");
  } else if (cmd == "/update") {
    nextFetchAt = millis();
    retryDelay = WEATHER_RETRY_MS;
    if (!settings.weatherOn) Serial.println("(weather is off; /weather on to show it)");
    else if (WiFi.status() != WL_CONNECTED) Serial.println("! no Wi-Fi");

  } else if (cmd == "/mode") {
    int m = alpha::findMode(arg.c_str());
    if (m < 0) return (void)Serial.println("! unknown mode, see /help");
    settings.text.mode = m;
    refreshDisplay();
  } else if (cmd == "/color") {
    int c = arg.equalsIgnoreCase("none") ? -1 : alpha::findColor(arg.c_str());
    if (c < 0 && !arg.equalsIgnoreCase("none")) return (void)Serial.println("! unknown color, see /help");
    settings.text.color = c;
    saveSettings();
    refreshDisplay();
  } else if (cmd == "/speed") {
    long s = arg.toInt();
    if (s < 0 || s > 5) return (void)Serial.println("! speed must be 0-5");
    settings.text.speed = s;
    saveSettings();
    refreshDisplay();
  } else if (cmd == "/priority") {
    settings.usePriority = arg.equalsIgnoreCase("on");
    saveSettings();
    if (!settings.usePriority) clearPriority();
    refreshDisplay();
  } else if (cmd == "/clear") {
    clearPriority();

  } else if (cmd == "/baud") {
    long b = arg.toInt();
    if (b != 1200 && b != 2400 && b != 4800 && b != 9600 && b != 19200 && b != 38400)
      return (void)Serial.println("! baud must be 1200, 2400, 4800, 9600, 19200 or 38400");
    settings.baud = b;
    saveSettings();
    applySerialSettings();
    refreshDisplay();
  } else if (cmd == "/fmt") {
    for (size_t i = 0; i < FORMAT_COUNT; ++i) {
      if (arg.equalsIgnoreCase(FORMATS[i].name)) {
        settings.format = i;
        saveSettings();
        applySerialSettings();
        refreshDisplay();
        return;
      }
    }
    Serial.println("! format must be 7E2 or 8N1");
  } else if (cmd == "/addr") {
    arg.toUpperCase();
    bool ok = arg.length() == 2 && isxdigit((unsigned char)arg[0]) && isxdigit((unsigned char)arg[1]);
    if (!ok) return (void)Serial.println("! usage: /addr 01 (two hex digits, 00 = broadcast)");
    settings.text.address[0] = arg[0];
    settings.text.address[1] = arg[1];
    saveSettings();
    refreshDisplay();
  } else if (cmd == "/type") {
    if (arg.length() != 1 || arg[0] < 0x20 || arg[0] > 0x7E)
      return (void)Serial.println("! usage: /type Z (one character, e.g. Z = all signs, 1 = one-line signs)");
    settings.text.typeCode = arg[0];
    saveSettings();
    refreshDisplay();
  } else if (cmd == "/de") {
    int sp = arg.indexOf(' ');
    String pinArg = sp < 0 ? arg : arg.substring(0, sp);
    String polArg = sp < 0 ? "" : arg.substring(sp + 1);
    polArg.trim();
    bool numeric = pinArg.length() > 0;
    for (size_t i = 0; i < pinArg.length(); ++i)
      if (!isdigit((unsigned char)pinArg[i]) && !(i == 0 && pinArg[i] == '-')) numeric = false;
    if (!numeric || (polArg.length() && !polArg.equalsIgnoreCase("high") && !polArg.equalsIgnoreCase("low")))
      return (void)Serial.println("! usage: /de 21 high, /de 21 low, or /de -1 (no pin)");
    long pin = pinArg.toInt();
    if (pin < -1 || pin > 48) return (void)Serial.println("! pin must be -1 to 48");
    int oldPin = dePin;
    dePin = (int)pin;
    if (polArg.length()) deActiveHigh = polArg.equalsIgnoreCase("high");
    applyDePin(oldPin);
    saveSettings();
    printStatus();
  } else if (cmd == "/txtest") {
    long secs = arg.length() ? arg.toInt() : 10;
    runTxTest(secs > 0 && secs <= 60 ? secs : 10);
  } else if (cmd == "/txflip") {
    long pin = arg.length() ? arg.toInt() : RS485_TX_PIN;
    bool numeric = true;
    for (size_t i = 0; i < arg.length(); ++i)
      if (!isdigit((unsigned char)arg[i])) numeric = false;
    // GPIO19/20 are the USB port; toggling them would cut this monitor off.
    if (!numeric || pin < 0 || pin > 48 || pin == 19 || pin == 20 || pin == dePin)
      return (void)Serial.println("! usage: /txflip or /txflip <gpio> (not 19, 20 or the DE pin)");
    runTxFlip((int)pin);
  } else if (cmd == "/checksum") {
    settings.text.checksum = arg.equalsIgnoreCase("on");
    saveSettings();
    refreshDisplay();
  } else if (cmd == "/protocol") {
    if (arg.equalsIgnoreCase("j1587")) settings.protocol = PROTO_J1587;
    else if (arg.equalsIgnoreCase("alpha")) settings.protocol = PROTO_ALPHA;
    else return (void)Serial.println("! usage: /protocol j1587 or /protocol alpha");
    saveSettings();
    applySerialSettings();
    printStatus();
  } else if (cmd == "/j1587test") {
    if (settings.protocol != PROTO_J1587) {
      settings.protocol = PROTO_J1587;
      saveSettings();
      applySerialSettings();
    }
    Serial.println("Sending HELLO with the J1587 handshake...");
    Serial.println(sendJ1587("HELLO", 0) ? "Sent. Is HELLO on the sign?" : "Failed (see above).");
  } else if (cmd == "/j1587mode") {
    settings.j1587Mode = arg.equalsIgnoreCase("on");
    saveSettings();
    refreshDisplay();
  } else if (cmd == "/probe") {
    if (settings.protocol == PROTO_J1587)
      return (void)Serial.println("! /probe tests Alpha settings; switch with /protocol alpha first");
    runProbe();
  } else if (cmd == "/sniff") {
    sniffing = !sniffing;
    Serial.printf("sniffing %s\n", sniffing ? "on" : "off");
  } else if (cmd == "/resend") {
    refreshDisplay();
  } else {
    Serial.println("! unknown command, try /help");
  }
}

void pollConsole() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (consoleLine.length()) handleCommand(consoleLine);
      consoleLine = "";
    } else if (consoleLine.length() < MAX_MESSAGE_LEN + 16) {
      consoleLine += c;
    }
  }
}

// ---------------------------------------------------------------- web UI

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

String option(const String& value, const String& label, bool selected) {
  return "<option value='" + value + "'" + (selected ? " selected" : "") + ">" + label + "</option>";
}

String webNotice;  // one-shot message shown at the top of the page

void redirectHome(const String& notice) {
  webNotice = notice;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRoot() {
  const alpha::TextOptions& t = settings.text;
  String html;
  html.reserve(9216);
  html += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>LED Sign</title><style>"
            "body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px;background:#111;color:#eee}"
            "h1{color:#f33;font-size:1.4em}h2{font-size:1.1em;margin:28px 0 4px;color:#f88}"
            "label{display:block;margin-top:12px}"
            "input,select,button{width:100%;padding:10px;margin-top:4px;font-size:1em;box-sizing:border-box;"
            "background:#222;color:#eee;border:1px solid #444;border-radius:6px}"
            "input[type=checkbox],input[type=radio]{width:auto;margin:0}"
            "label.check{display:flex;gap:10px;align-items:center;margin-top:10px}"
            "button{background:#c22;border:0;margin-top:16px;font-weight:bold}"
            "button.alt{background:#444}.row{display:flex;gap:8px}.row>*{flex:1}"
            "small,.status{color:#999}.notice{background:#332;padding:10px;border-radius:6px}"
            "</style></head><body><h1>LED Sign</h1>");

  if (webNotice.length()) {
    html += "<p class='notice'>" + htmlEscape(webNotice) + "</p>";
    webNotice = "";
  }
  bool showingData = settings.weatherOn || settings.busOn;
  html += "<p class='status'>Showing: <b>" + String(displayName()) + "</b><br>" +
          htmlEscape(showingData ? lastSentWeather : settings.message) + "</p>";

  // Scroll speed (applies to the weather and messages)
  html += F("<h2>Scroll speed</h2><form method='post' action='/scrollspeed'>"
            "<label>How fast text moves across the sign<select name='speed'>");
  html += option("0", "Sign default", t.speed == 0);
  for (int sp = 1; sp <= 5; ++sp) {
    String label = String(sp) + (sp == 1 ? " (slowest)" : sp == 5 ? " (fastest)" : "");
    html += option(String(sp), label, sp == t.speed);
  }
  html += F("</select></label><button type='submit'>Set speed</button></form>");

  // Weather
  html += F("<h2>Weather</h2><form method='post' action='/weather'>"
            "<label>City, state, country <small>(e.g. Ames, Iowa, US or Paris, France)</small>"
            "<input name='location' value='");
  html += htmlEscape(settings.placeQuery.length() ? settings.placeQuery : settings.placeName);
  html += F("'></label><div class='row'><label>Units<select name='units'>");
  html += option("f", "&deg;F", settings.fahrenheit) + option("c", "&deg;C", !settings.fahrenheit);
  html += F("</select></label><label><br><input type='checkbox' name='showplace'");
  if (settings.showPlace) html += F(" checked");
  html += F("> Show city name</label></div><button type='submit'>Show weather</button></form>"
            "<form method='post' action='/update'><button class='alt'>Update weather now</button></form>"
            "<p class='status'>");
  html += htmlEscape(weatherStatus());
  html += F("</p>");

  // Bus times
  html += F("<h2>Bus times</h2>");
  if (!settings.transitKey.length())
    html += F("<p class='status'>First save your Transitland API key at the bottom of this section.</p>");
  if (settings.busStop.length()) {
    html += F("<form method='post' action='/bus'><p>Stop: <b>");
    html += htmlEscape(settings.busStopName);
    html += F("</b></p><label>Lines to show <small>(several scroll by one after another; none ticked = all)"
              "</small></label>");
    for (size_t i = 0; i < busKnown.count; ++i) {
      const transit::Route& r = busKnown.items[i];
      html += F("<label class='check'><input type='checkbox' name='line' value='");
      html += htmlEscape(r.id);
      html += "'";
      if (busLines.find(r.id) >= 0) html += F(" checked");
      html += ">" + htmlEscape(routeName(r));
      if (r.shortName[0] && r.longName[0]) {
        html += F(" <small>");
        html += htmlEscape(r.longName);
        html += F("</small>");
      }
      html += F("</label>");
    }
    if (!busKnown.count) html += F("<p class='status'>No buses found at this stop in the next 24 hours.</p>");
    html += F("<div class='row'><label>Show<select name='show'>");
    html += option("bus", "Bus times only", !settings.weatherOn || !settings.busOn);
    html += option("both", "Bus times, then weather", settings.weatherOn && settings.busOn);
    html += F("</select></label><label>Check every<select name='every'>");
    for (uint8_t every : BUS_EVERY_CHOICES) {
      long perMonth = busRequestsPerMonth(every);
      String label = String((unsigned)every) + " min (" + String(perMonth) + "/month" +
                     (perMonth > BUS_FREE_QUOTA ? ", over free plan" : "") + ")";
      html += option(String((unsigned)every), label, every == settings.busEvery);
    }
    html += F("</select></label></div><button type='submit'>Show bus times</button></form>"
              "<form method='post' action='/busupdate'><button class='alt'>Update bus times now</button></form>");
  }
  html += "<p class='status'>" + htmlEscape(busStatus()) + "</p>";

  html += F("<form method='post' action='/bussearch'><label>Find a stop <small>(stop name or street, e.g. "
            "Beach or Lincoln Way)</small><input name='q' value='");
  html += htmlEscape(busSearchQuery);
  html += F("'></label><button class='alt' type='submit'>Search stops</button></form>");
  if (busResultCount) {
    html += F("<form method='post' action='/busstop'>");
    for (size_t i = 0; i < busResultCount; ++i) {
      const transit::Stop& st = busResults[i];
      html += F("<label class='check'><input type='radio' name='id' value='");
      html += htmlEscape(st.id);
      html += "'";
      if (i == 0) html += F(" checked");
      html += "><span>" + htmlEscape(st.name);
      if (st.code[0]) html += " (" + htmlEscape(st.code) + ")";
      if (st.routes[0]) html += " <small>lines " + htmlEscape(st.routes) + "</small>";
      html += F("</span></label>");
    }
    html += F("<button type='submit'>Use this stop</button></form>");
  }

  html += F("<form method='post' action='/buskey'><label>Transitland API key <small>(");
  if (settings.transitKey.length() > 4)
    html += "saved, ends in " + htmlEscape(settings.transitKey.substring(settings.transitKey.length() - 4));
  else
    html += F("not set; get one free at transit.land");
  html += F(")</small><input name='key' type='password' autocomplete='off'></label>"
            "<button class='alt' type='submit'>Save key</button></form>");

  // Message
  html += F("<h2>Message</h2><form method='post' action='/send'>"
            "<label>Text<input name='text' maxlength='240' value='");
  html += htmlEscape(settings.message);
  html += F("'></label><div class='row'><label>Mode<select name='mode'>");
  for (size_t i = 0; i < alpha::MODE_COUNT; ++i)
    html += option(String(i), alpha::MODES[i].name, (int)i == t.mode);
  html += F("</select></label></div><label>Color <small>(tricolor signs only)</small><select name='color'>");
  html += option("-1", "default", t.color < 0);
  for (size_t i = 0; i < alpha::COLOR_COUNT; ++i)
    html += option(String(i), alpha::COLORS[i].name, (int)i == t.color);
  html += F("</select></label><label><input type='checkbox' name='priority'");
  if (settings.usePriority) html += F(" checked");
  html += F("> Priority message <small>(overrides everything, 125 bytes max)</small></label>"
            "<button type='submit'>Show message</button></form>"
            "<form method='post' action='/clear'><button class='alt'>Clear priority message</button></form>");

  // Wi-Fi
  html += F("<h2>Home Wi-Fi</h2><form method='post' action='/wifi'>"
            "<label>Network<input name='ssid' value='");
  html += htmlEscape(settings.wifiSsid);
  html += F("'></label><label>Password <small>(leave blank to keep the saved one)</small>"
            "<input name='pass' type='password'></label>"
            "<button class='alt' type='submit'>Save and connect</button></form><p class='status'>");
  html += WiFi.status() == WL_CONNECTED ? "Connected, " + WiFi.localIP().toString() : String("Not connected");
  html += " &middot; Sign serial " + String(settings.baud) + " " + FORMATS[settings.format].name;
  html += F("</p></body></html>");
  server.send(200, "text/html", html);
}

void handleWeather() {
  settings.fahrenheit = server.arg("units") != "c";
  settings.showPlace = server.hasArg("showplace");
  String location = server.arg("location");
  location.trim();
  String notice;
  if (location.length() && !location.equalsIgnoreCase(settings.placeQuery) &&
      !location.equalsIgnoreCase(settings.placeName)) {
    String error;
    if (!setLocationByName(location, error)) return redirectHome("Couldn't set location: " + error);
    notice = "Location set to " + settings.placeName;
    if (settings.placeDetail.length()) notice += ", " + settings.placeDetail;
    notice += ".";
  }
  if (!hasLocation()) return redirectHome("Enter a city first.");
  haveForecast = false;
  setDisplay(true, false);
  redirectHome(notice);
}

void handleBus() {
  if (!settings.busStop.length()) return redirectHome("Find and pick a bus stop first.");
  busLines.clear();
  for (int i = 0; i < server.args(); ++i) {
    if (server.argName(i) != "line") continue;
    int k = busKnown.find(server.arg(i).c_str());
    if (k >= 0) busLines.add(busKnown.items[k]);
  }
  long every = server.arg("every").toInt();
  for (uint8_t choice : BUS_EVERY_CHOICES)
    if (every == choice) settings.busEvery = choice;
  setDisplay(server.arg("show") == "both", true);
  redirectHome("Showing bus times for " + busLinesSummary() + ".");
}

void handleBusUpdate() {
  busNextFetchAt = millis();
  busRetryDelay = BUS_RETRY_MS;
  redirectHome(settings.busOn ? "Updating bus times..." : "Bus times are off. Press \"Show bus times\" first.");
}

void handleBusSearch() {
  String q = server.arg("q");
  q.trim();
  if (q.isEmpty()) return redirectHome("Type part of a stop name to search.");
  String error;
  if (!searchStops(q, error)) return redirectHome("Couldn't search stops: " + error);
  if (!busResultCount) {
    String near = hasLocation() && settings.placeName.length() ? " near " + settings.placeName : String();
    return redirectHome("No stops matching \"" + q + "\"" + near + ". Try a shorter name or a street.");
  }
  redirectHome("Found " + String((unsigned)busResultCount) + " stop" + (busResultCount == 1 ? "" : "s") +
               ". Pick yours below.");
}

void handleBusStop() {
  String error;
  if (!selectStop(server.arg("id"), error)) return redirectHome("Couldn't use that stop: " + error);
  redirectHome("Stop set to " + settings.busStopName + ". Tick the lines you want and press \"Show bus times\".");
}

void handleBusKey() {
  String key = server.arg("key");
  key.trim();
  if (key.isEmpty()) return redirectHome("Paste your Transitland API key first.");
  settings.transitKey = key;
  saveSettings();
  busNextFetchAt = millis();
  redirectHome("Transitland API key saved.");
}

void handleUpdate() {
  nextFetchAt = millis();
  retryDelay = WEATHER_RETRY_MS;
  redirectHome(settings.weatherOn ? "Updating..." : "Weather is off. Press \"Show weather\" first.");
}

void handleSend() {
  int m = server.arg("mode").toInt();
  if (m >= 0 && m < (int)alpha::MODE_COUNT) settings.text.mode = m;
  int c = server.arg("color").toInt();
  settings.text.color = (c >= 0 && c < (int)alpha::COLOR_COUNT) ? c : -1;
  bool wasPriority = settings.usePriority;
  settings.usePriority = server.hasArg("priority");
  if (wasPriority && !settings.usePriority) clearPriority();

  showMessage(server.hasArg("text") ? server.arg("text") : settings.message);
  redirectHome("");
}

void handleScrollSpeed() {
  long sp = server.arg("speed").toInt();
  if (sp < 0 || sp > 5) return redirectHome("Speed must be 1-5 or sign default.");
  settings.text.speed = (uint8_t)sp;
  saveSettings();
  refreshDisplay();  // re-send what's showing at the new speed
  String notice = "Scroll speed set to ";
  notice += sp ? String(sp) : String("sign default");
  notice += ".";
  redirectHome(notice);
}

void handleClear() {
  settings.usePriority = false;
  saveSettings();
  clearPriority();
  redirectHome("Priority message cleared.");
}

void handleWifi() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (ssid.isEmpty()) return redirectHome("Enter a network name.");
  settings.wifiSsid = ssid;
  if (server.arg("pass").length()) settings.wifiPassword = server.arg("pass");
  saveSettings();
  redirectHome("Connecting to " + ssid + ". If this page stops responding, reopen it at http://" +
               MDNS_NAME + ".local on that network.");
  reconnectAt = millis() + 500;  // after the redirect has gone out
}

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/weather", HTTP_POST, handleWeather);
  server.on("/update", HTTP_POST, handleUpdate);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/scrollspeed", HTTP_POST, handleScrollSpeed);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/bus", HTTP_POST, handleBus);
  server.on("/busupdate", HTTP_POST, handleBusUpdate);
  server.on("/bussearch", HTTP_POST, handleBusSearch);
  server.on("/busstop", HTTP_POST, handleBusStop);
  server.on("/buskey", HTTP_POST, handleBusKey);
  server.begin();
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);

  prefs.begin("ledsign", false);
  loadSettings();
  applyDePin(-1);
  applySerialSettings();
  startWifi();
  startWebServer();

  Serial.println("\nLED sign controller ready. Type /help for commands.");
  printStatus();
}

void loop() {
  if (bootMessagePending && millis() > BOOT_SEND_DELAY_MS) {
    bootMessagePending = false;
    refreshDisplay();
  }
  pollConsole();
  pollWifi();
  pollWeather();
  pollBus();
  server.handleClient();
  if (sniffing) pollSniffer();
  else while (SignSerial.available()) SignSerial.read();  // discard echoes/noise
}
