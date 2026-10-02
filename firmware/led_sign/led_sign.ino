// led_sign.ino - ESP32 controller for an Adaptive Micro Systems 80x7 transit
// sign (PN 1105-2126) over RS-485 using the Alpha sign protocol.
//
// Two display modes:
//   * Weather: joins your home Wi-Fi, pulls today's forecast from Open-Meteo
//     every 15 minutes and has the sign cycle through condition, current
//     temperature, high, low, humidity and chance of rain.
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

#include "AlphaSign.h"
#include "Weather.h"

// ---------------------------------------------------------------- configuration

// RS-485 transceiver pins. GPIO 16/17 are fine on ESP32 and ESP32-S3.
// On an ESP32-C3 pick other pins (16/17 are used by the flash there).
//
// Default: auto-direction module (e.g. DIYables RS485-TTL, pins VCC GND RXD TXD).
// Its pin names are from the module's point of view: module TXD -> ESP32 RX,
// module RXD <- ESP32 TX.
// For a MAX3485-style module with DE/RE pins, wire RO->RX, DI->TX, tie DE+RE
// to a GPIO and set RS485_DE_PIN to that GPIO (e.g. 4).
constexpr int RS485_RX_PIN = 16;  // module TXD (or RO)
constexpr int RS485_TX_PIN = 17;  // module RXD (or DI)
constexpr int RS485_DE_PIN = -1;  // -1 = module switches direction itself

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
constexpr uint32_t WEATHER_INTERVAL_MS = 15UL * 60 * 1000;  // Open-Meteo updates every 15 min
constexpr uint32_t WEATHER_RETRY_MS = 60UL * 1000;        // first retry after a failed fetch
constexpr uint32_t WEATHER_STALE_MS = 3UL * 60 * 60 * 1000;  // stop showing data older than this
constexpr uint32_t WEATHER_RESEND_MS = 60UL * 60 * 1000;     // re-send hourly in case the sign lost power

// About 13 characters fit across 80 columns. Longer pages scroll.
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

struct Settings {
  uint32_t baud = 9600;
  uint8_t format = 0;  // index into FORMATS
  alpha::TextOptions text;
  bool usePriority = false;
  String message = "HELLO FROM ESP32";

  String wifiSsid;
  String wifiPassword;

  bool weatherOn = false;
  double latitude = NAN;
  double longitude = NAN;
  String placeName;
  bool showPlace = true;
  bool fahrenheit = true;
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

// ---------------------------------------------------------------- RS-485 I/O

void applySerialSettings() {
  SignSerial.end();
  SignSerial.begin(settings.baud, FORMATS[settings.format].config, RS485_RX_PIN, RS485_TX_PIN);
}

void sendBytes(const uint8_t* data, size_t len) {
  if (RS485_DE_PIN >= 0) digitalWrite(RS485_DE_PIN, HIGH);
  SignSerial.write(data, len);
  SignSerial.flush();  // block until the last stop bit has left the UART
  if (RS485_DE_PIN >= 0) digitalWrite(RS485_DE_PIN, LOW);
}

bool sendPages(const alpha::Page* pages, size_t count, const alpha::TextOptions& opts) {
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
  prefs.putBool("showplace", settings.showPlace);
  prefs.putBool("fahrenheit", settings.fahrenheit);
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
  settings.weatherOn = prefs.getBool("weather", false);
  settings.latitude = prefs.getDouble("lat", NAN);
  settings.longitude = prefs.getDouble("lon", NAN);
  settings.placeName = prefs.getString("place", "");
  settings.showPlace = prefs.getBool("showplace", true);
  settings.fahrenheit = prefs.getBool("fahrenheit", true);
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

// ---------------------------------------------------------------- weather

bool fetchWeather() {
  if (!hasLocation()) {
    lastWeatherError = "no location set";
    return false;
  }
  char url[320];
  weather::forecastUrl(url, sizeof(url), settings.latitude, settings.longitude, settings.fahrenheit);
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

// Looks up a place name with Open-Meteo geocoding and makes it the weather
// location.
bool setLocationByName(const String& name, String& error) {
  char url[256];
  weather::geocodeUrl(url, sizeof(url), name.c_str());
  String body;
  weather::Place place;
  if (!httpGet(url, body, error)) return false;
  if (!weather::parsePlace(body.c_str(), place)) {
    error = "no place called \"" + name + "\" (use just the city name, or /latlon)";
    return false;
  }
  settings.latitude = place.latitude;
  settings.longitude = place.longitude;
  settings.placeName = place.name;
  Serial.printf("Location: %s (%.4f, %.4f)\n", place.name, place.latitude, place.longitude);
  return true;
}

void showWeather(bool force = false) {
  char lines[weather::MAX_PAGES][weather::PAGE_LEN];
  size_t count = 0;

  bool stale = haveForecast && millis() - lastFetchOkAt > WEATHER_STALE_MS;
  if (!hasLocation()) {
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

  // Short pages hold still; anything wider than the sign scrolls.
  alpha::Page pages[weather::MAX_PAGES];
  String summary;
  for (size_t i = 0; i < count; ++i) {
    bool fits = strlen(lines[i]) <= SIGN_WIDTH_CHARS;
    pages[i] = {lines[i], alpha::findMode(fits ? "hold" : "rotate")};
    summary += (i ? " | " : "") + String(lines[i]);
  }

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
    showWeather(true);
  }
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
  showWeather();
}

// ---------------------------------------------------------------- display API

// Shows a message with the current mode/colour/speed, remembers it, and
// switches out of weather mode.
void showMessage(const String& text) {
  settings.message = text.substring(0, MAX_MESSAGE_LEN);
  settings.weatherOn = false;
  if (sendText(settings.message.c_str(), currentOptions())) {
    saveSettings();
    Serial.printf("-> sign: \"%s\"\n", settings.message.c_str());
  }
}

// Re-sends whatever the sign should be showing (after a setting change).
void refreshDisplay() {
  if (settings.weatherOn) showWeather(true);
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
  uint32_t savedBaud = settings.baud;
  uint8_t savedFormat = settings.format;

  alpha::TextOptions opts;
  opts.mode = alpha::findMode("hold");

  Serial.println("Probing; watch the sign. Press Enter in this monitor to stop.");
  for (size_t f = 0; f < FORMAT_COUNT; ++f) {
    for (uint32_t baud : BAUDS) {
      settings.baud = baud;
      settings.format = f;
      applySerialSettings();
      char text[32];
      snprintf(text, sizeof(text), "%lu %s", (unsigned long)baud, FORMATS[f].name);
      Serial.printf("  trying %s\n", text);
      sendText(text, opts);

      uint32_t start = millis();
      while (millis() - start < 5000) {
        server.handleClient();
        if (Serial.available()) {
          while (Serial.available()) Serial.read();
          Serial.printf("Stopped at %s. Use /baud and /fmt to keep it.\n", text);
          settings.baud = savedBaud;
          settings.format = savedFormat;
          applySerialSettings();
          return;
        }
        delay(10);
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
  s += " (" + String(settings.latitude, 4) + ", " + String(settings.longitude, 4) + "), ";
  s += settings.fahrenheit ? "F" : "C";
  if (haveForecast) s += ", updated " + String((millis() - lastFetchOkAt) / 60000) + " min ago";
  if (lastWeatherError.length()) s += ", last error: " + lastWeatherError;
  return s;
}

void printStatus() {
  const alpha::TextOptions& t = settings.text;
  Serial.printf("display  : %s\n", settings.weatherOn ? "weather" : "message");
  Serial.printf("weather  : %s\n", weatherStatus().c_str());
  Serial.printf("message  : \"%s\"\n", settings.message.c_str());
  Serial.printf("mode     : %s\n", t.mode >= 0 ? alpha::MODES[t.mode].name : "(none)");
  Serial.printf("color    : %s\n", t.color >= 0 ? alpha::COLORS[t.color].name : "(sign default)");
  Serial.printf("speed    : %s\n", t.speed ? String(t.speed).c_str() : "(sign default)");
  Serial.printf("priority : %s\n", settings.usePriority ? "on" : "off");
  Serial.printf("serial   : %lu %s, checksum %s\n", (unsigned long)settings.baud,
                FORMATS[settings.format].name, t.checksum ? "on" : "off");
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
      "  /location <city>      look up a city by name, e.g. /location Milwaukee\n"
      "  /latlon <lat> <lon> [label]  set coordinates directly\n"
      "  /units f|c            Fahrenheit or Celsius\n"
      "  /showplace on|off     include the place name in the cycle\n"
      "  /weather on|off       switch between weather and your message\n"
      "  /update               fetch the weather now\n"
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
      "  /baud <rate>       1200 2400 4800 9600 19200 38400\n"
      "  /fmt 7E2|8N1       serial framing\n"
      "  /checksum on|off   append a checksum to each packet\n"
      "  /probe             try every baud/format combination\n"
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
    if (arg.isEmpty()) return (void)Serial.println("! usage: /location <city>");
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
    haveForecast = false;
    setWeatherMode(true);
  } else if (cmd == "/units") {
    settings.fahrenheit = !arg.equalsIgnoreCase("c");
    haveForecast = false;  // values are in the old unit until the next fetch
    setWeatherMode(settings.weatherOn);
  } else if (cmd == "/showplace") {
    settings.showPlace = arg.equalsIgnoreCase("on");
    saveSettings();
    if (settings.weatherOn) showWeather(true);
  } else if (cmd == "/weather") {
    setWeatherMode(arg.equalsIgnoreCase("on"));
    if (!settings.weatherOn) showMessage(settings.message);
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
  } else if (cmd == "/checksum") {
    settings.text.checksum = arg.equalsIgnoreCase("on");
    saveSettings();
    refreshDisplay();
  } else if (cmd == "/probe") {
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

void redirectHome(const String& notice = "") {
  webNotice = notice;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRoot() {
  const alpha::TextOptions& t = settings.text;
  String html;
  html.reserve(6144);
  html += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>LED Sign</title><style>"
            "body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px;background:#111;color:#eee}"
            "h1{color:#f33;font-size:1.4em}h2{font-size:1.1em;margin:28px 0 4px;color:#f88}"
            "label{display:block;margin-top:12px}"
            "input,select,button{width:100%;padding:10px;margin-top:4px;font-size:1em;box-sizing:border-box;"
            "background:#222;color:#eee;border:1px solid #444;border-radius:6px}"
            "input[type=checkbox]{width:auto}"
            "button{background:#c22;border:0;margin-top:16px;font-weight:bold}"
            "button.alt{background:#444}.row{display:flex;gap:8px}.row>*{flex:1}"
            "small,.status{color:#999}.notice{background:#332;padding:10px;border-radius:6px}"
            "</style></head><body><h1>LED Sign</h1>");

  if (webNotice.length()) {
    html += "<p class='notice'>" + htmlEscape(webNotice) + "</p>";
    webNotice = "";
  }
  html += "<p class='status'>Showing: <b>" + String(settings.weatherOn ? "weather" : "message") +
          "</b><br>" + htmlEscape(settings.weatherOn ? lastSentWeather : settings.message) + "</p>";

  // Weather
  html += F("<h2>Weather</h2><form method='post' action='/weather'>"
            "<label>City <small>(name only, e.g. Milwaukee)</small><input name='location' value='");
  html += htmlEscape(settings.placeName);
  html += F("'></label><div class='row'><label>Units<select name='units'>");
  html += option("f", "&deg;F", settings.fahrenheit) + option("c", "&deg;C", !settings.fahrenheit);
  html += F("</select></label><label><br><input type='checkbox' name='showplace'");
  if (settings.showPlace) html += F(" checked");
  html += F("> Show city name</label></div><button type='submit'>Show weather</button></form>"
            "<form method='post' action='/update'><button class='alt'>Update weather now</button></form>"
            "<p class='status'>");
  html += htmlEscape(weatherStatus());
  html += F("</p>");

  // Message
  html += F("<h2>Message</h2><form method='post' action='/send'>"
            "<label>Text<input name='text' maxlength='240' value='");
  html += htmlEscape(settings.message);
  html += F("'></label><div class='row'><label>Mode<select name='mode'>");
  for (size_t i = 0; i < alpha::MODE_COUNT; ++i)
    html += option(String(i), alpha::MODES[i].name, (int)i == t.mode);
  html += F("</select></label><label>Speed<select name='speed'>");
  for (int s = 0; s <= 5; ++s) html += option(String(s), s ? String(s) : String("default"), s == t.speed);
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
  if (location.length() && !location.equalsIgnoreCase(settings.placeName)) {
    String error;
    if (!setLocationByName(location, error)) return redirectHome("Couldn't set location: " + error);
    notice = "Location set to " + settings.placeName + ".";
  }
  if (!hasLocation()) return redirectHome("Enter a city first.");
  haveForecast = false;
  setWeatherMode(true);
  redirectHome(notice);
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
  settings.text.speed = constrain(server.arg("speed").toInt(), 0, 5);

  bool wasPriority = settings.usePriority;
  settings.usePriority = server.hasArg("priority");
  if (wasPriority && !settings.usePriority) clearPriority();

  showMessage(server.hasArg("text") ? server.arg("text") : settings.message);
  redirectHome();
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
  server.on("/wifi", HTTP_POST, handleWifi);
  server.begin();
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  if (RS485_DE_PIN >= 0) {
    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, LOW);  // listen by default
  }

  prefs.begin("ledsign", false);
  loadSettings();
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
  server.handleClient();
  if (sniffing) pollSniffer();
  else while (SignSerial.available()) SignSerial.read();  // discard echoes/noise
}
