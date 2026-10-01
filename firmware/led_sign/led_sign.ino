// led_sign.ino - ESP32 controller for an Adaptive Micro Systems 80x7 transit
// sign (PN 1105-2126) over RS-485 using the Alpha sign protocol.
//
// Control it three ways:
//   * USB Serial Monitor (115200 baud): type a message, or /help for commands
//   * Wi-Fi: join the "LED-Sign" network and browse to http://192.168.4.1
//   * From your own code: call showMessage("...")
//
// The last message and settings are saved in flash and re-sent at boot, so the
// sign comes back up showing the right thing after a power cycle.
//
// Wiring and setup: see README.md.

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "AlphaSign.h"

// ---------------------------------------------------------------- configuration

// RS-485 transceiver pins. GPIO 16/17 are fine on ESP32 and ESP32-S3.
// On an ESP32-C3 pick other pins (16/17 are used by the flash there).
constexpr int RS485_RX_PIN = 16;  // transceiver RO
constexpr int RS485_TX_PIN = 17;  // transceiver DI
constexpr int RS485_DE_PIN = 4;   // transceiver DE + /RE tied together; -1 for auto-direction modules

// Wi-Fi access point the ESP32 creates. Password must be at least 8 characters.
const char* AP_SSID = "LED-Sign";
const char* AP_PASSWORD = "ledsign123";

// Optional: also join an existing network (leave blank to skip).
const char* STA_SSID = "";
const char* STA_PASSWORD = "";

// Wait for the sign to finish booting before sending the saved message.
constexpr uint32_t BOOT_SEND_DELAY_MS = 3000;

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
};

HardwareSerial SignSerial(1);
WebServer server(80);
Preferences prefs;
Settings settings;

bool sniffing = false;
bool bootMessagePending = true;
String consoleLine;

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

// Sends `text` to the sign using the current settings. Returns false if the
// packet could not be built.
bool sendText(const char* text, const alpha::TextOptions& opts) {
  uint8_t packet[MAX_MESSAGE_LEN + 64];
  size_t len = alpha::buildWriteText(packet, sizeof(packet), opts, text);
  if (len == 0) {
    Serial.println("! message too long");
    return false;
  }
  sendBytes(packet, len);
  return true;
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
  if (settings.text.mode >= (int)alpha::MODE_COUNT) settings.text.mode = 0;
  if (settings.text.color >= (int)alpha::COLOR_COUNT) settings.text.color = -1;
}

// ---------------------------------------------------------------- public API

// Shows a message with the current mode/colour/speed and remembers it.
void showMessage(const String& text) {
  settings.message = text.substring(0, MAX_MESSAGE_LEN);
  if (sendText(settings.message.c_str(), currentOptions())) {
    saveSettings();
    Serial.printf("-> sign: \"%s\"\n", settings.message.c_str());
  }
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

  Serial.println("Probing; watch the sign. Any key in this monitor stops the probe.");
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

void printStatus() {
  const alpha::TextOptions& t = settings.text;
  Serial.printf("serial   : %lu %s\n", (unsigned long)settings.baud, FORMATS[settings.format].name);
  Serial.printf("mode     : %s\n", t.mode >= 0 ? alpha::MODES[t.mode].name : "(none)");
  Serial.printf("color    : %s\n", t.color >= 0 ? alpha::COLORS[t.color].name : "(sign default)");
  Serial.printf("speed    : %s\n", t.speed ? String(t.speed).c_str() : "(sign default)");
  Serial.printf("checksum : %s\n", t.checksum ? "on" : "off");
  Serial.printf("priority : %s\n", settings.usePriority ? "on" : "off");
  Serial.printf("message  : \"%s\"\n", settings.message.c_str());
  Serial.printf("wifi     : AP \"%s\" at %s", AP_SSID, WiFi.softAPIP().toString().c_str());
  if (WiFi.status() == WL_CONNECTED) Serial.printf(", LAN %s", WiFi.localIP().toString().c_str());
  Serial.println();
}

void printHelp() {
  Serial.println(
      "Type text and press Enter to show it on the sign, or use a command:\n"
      "  /mode <name>       rotate hold flash rollup rolldown rollleft rollright\n"
      "                     wipeup wipedown wipeleft wiperight scroll auto rollin\n"
      "                     rollout wipein wipeout compressed twinkle sparkle snow\n"
      "                     interlock switch slide spray starburst\n"
      "  /color <name|none> red green amber dimred dimgreen brown orange yellow\n"
      "                     rainbow1 rainbow2 mix auto (red-only signs ignore this)\n"
      "  /speed <0-5>       1 = slowest, 5 = fastest, 0 = sign default\n"
      "  /priority on|off   use the priority file (max 125 bytes, overrides all)\n"
      "  /clear             clear the priority message\n"
      "  /baud <rate>       1200 2400 4800 9600 19200 38400\n"
      "  /fmt 7E2|8N1       serial framing\n"
      "  /checksum on|off   append a checksum to each packet\n"
      "  /probe             try every baud/format combination\n"
      "  /sniff             toggle printing bytes received from the bus\n"
      "  /resend            send the current message again\n"
      "  /status            show settings\n");
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
  } else if (cmd == "/mode") {
    int m = alpha::findMode(arg.c_str());
    if (m < 0) return (void)Serial.println("! unknown mode, see /help");
    settings.text.mode = m;
    showMessage(settings.message);
  } else if (cmd == "/color") {
    int c = arg.equalsIgnoreCase("none") ? -1 : alpha::findColor(arg.c_str());
    if (c < 0 && !arg.equalsIgnoreCase("none")) return (void)Serial.println("! unknown color, see /help");
    settings.text.color = c;
    showMessage(settings.message);
  } else if (cmd == "/speed") {
    long s = arg.toInt();
    if (s < 0 || s > 5) return (void)Serial.println("! speed must be 0-5");
    settings.text.speed = s;
    showMessage(settings.message);
  } else if (cmd == "/priority") {
    settings.usePriority = arg.equalsIgnoreCase("on");
    if (!settings.usePriority) clearPriority();
    showMessage(settings.message);
  } else if (cmd == "/clear") {
    clearPriority();
  } else if (cmd == "/baud") {
    long b = arg.toInt();
    if (b != 1200 && b != 2400 && b != 4800 && b != 9600 && b != 19200 && b != 38400)
      return (void)Serial.println("! baud must be 1200, 2400, 4800, 9600, 19200 or 38400");
    settings.baud = b;
    applySerialSettings();
    showMessage(settings.message);
  } else if (cmd == "/fmt") {
    for (size_t i = 0; i < FORMAT_COUNT; ++i) {
      if (arg.equalsIgnoreCase(FORMATS[i].name)) {
        settings.format = i;
        applySerialSettings();
        showMessage(settings.message);
        return;
      }
    }
    Serial.println("! format must be 7E2 or 8N1");
  } else if (cmd == "/checksum") {
    settings.text.checksum = arg.equalsIgnoreCase("on");
    showMessage(settings.message);
  } else if (cmd == "/probe") {
    runProbe();
  } else if (cmd == "/sniff") {
    sniffing = !sniffing;
    Serial.printf("sniffing %s\n", sniffing ? "on" : "off");
  } else if (cmd == "/resend") {
    showMessage(settings.message);
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
      default: out += c;
    }
  }
  return out;
}

void handleRoot() {
  const alpha::TextOptions& t = settings.text;
  String html;
  html.reserve(4096);
  html += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>LED Sign</title><style>"
            "body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px;background:#111;color:#eee}"
            "h1{color:#f33;font-size:1.4em}label{display:block;margin-top:12px}"
            "input,select,button{width:100%;padding:10px;margin-top:4px;font-size:1em;box-sizing:border-box;"
            "background:#222;color:#eee;border:1px solid #444;border-radius:6px}"
            "button{background:#c22;border:0;margin-top:16px;font-weight:bold}"
            ".row{display:flex;gap:8px}.row>*{flex:1}small{color:#999}"
            "</style></head><body><h1>LED Sign</h1><form method='post' action='/send'>");

  html += F("<label>Message<input name='text' maxlength='240' value='");
  html += htmlEscape(settings.message);
  html += F("'></label><div class='row'><label>Mode<select name='mode'>");
  for (size_t i = 0; i < alpha::MODE_COUNT; ++i) {
    html += "<option value='" + String(i) + "'" + ((int)i == t.mode ? " selected" : "") + ">" +
            alpha::MODES[i].name + "</option>";
  }
  html += F("</select></label><label>Speed<select name='speed'>");
  for (int s = 0; s <= 5; ++s) {
    html += "<option value='" + String(s) + "'" + (s == t.speed ? " selected" : "") + ">" +
            (s ? String(s) : String("default")) + "</option>";
  }
  html += F("</select></label></div><label>Color <small>(tricolor signs only)</small><select name='color'>");
  html += String("<option value='-1'") + (t.color < 0 ? " selected" : "") + ">default</option>";
  for (size_t i = 0; i < alpha::COLOR_COUNT; ++i) {
    html += "<option value='" + String(i) + "'" + ((int)i == t.color ? " selected" : "") + ">" +
            alpha::COLORS[i].name + "</option>";
  }
  html += F("</select></label><label><input type='checkbox' name='priority' style='width:auto'");
  if (settings.usePriority) html += F(" checked");
  html += F("> Priority message <small>(overrides everything, 125 bytes max)</small></label>"
            "<button type='submit'>Send to sign</button></form>"
            "<form method='post' action='/clear'><button style='background:#444'>Clear priority message</button></form>"
            "<p><small>Serial: ");
  html += String(settings.baud) + " " + FORMATS[settings.format].name;
  html += F("</small></p></body></html>");
  server.send(200, "text/html", html);
}

void handleSend() {
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= 0 && m < (int)alpha::MODE_COUNT) settings.text.mode = m;
  }
  if (server.hasArg("color")) {
    int c = server.arg("color").toInt();
    settings.text.color = (c >= 0 && c < (int)alpha::COLOR_COUNT) ? c : -1;
  }
  if (server.hasArg("speed")) settings.text.speed = constrain(server.arg("speed").toInt(), 0, 5);

  bool wasPriority = settings.usePriority;
  settings.usePriority = server.hasArg("priority");
  if (wasPriority && !settings.usePriority) clearPriority();

  showMessage(server.hasArg("text") ? server.arg("text") : settings.message);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleClear() {
  settings.usePriority = false;
  saveSettings();
  clearPriority();
  server.sendHeader("Location", "/");
  server.send(303);
}

void startWifi() {
  WiFi.mode(strlen(STA_SSID) ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  if (strlen(STA_SSID)) WiFi.begin(STA_SSID, STA_PASSWORD);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/clear", HTTP_POST, handleClear);
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

  Serial.println("\nLED sign controller ready. Type /help for commands.");
  printStatus();
}

void loop() {
  if (bootMessagePending && millis() > BOOT_SEND_DELAY_MS) {
    bootMessagePending = false;
    showMessage(settings.message);
  }
  pollConsole();
  server.handleClient();
  if (sniffing) pollSniffer();
  else while (SignSerial.available()) SignSerial.read();  // discard echoes/noise
}
