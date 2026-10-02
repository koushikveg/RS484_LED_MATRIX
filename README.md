# RS-485 LED Matrix Sign Controller (ESP32)

ESP32 firmware for driving an **Adaptive Micro Systems 80×7 transit sign**
(PN 1105-2126) over RS-485 using the **Alpha sign protocol**.

It has two display modes:

- **Weather:** the ESP32 joins your home Wi-Fi, pulls today's forecast for your
  city from [Open-Meteo](https://open-meteo.com) (free, no API key) every 15
  minutes, and the sign cycles through it:
  `MILWAUKEE` → `PARTLY CLOUDY` → `NOW 62F` → `HIGH 66F` → `LOW 51F` → `HUMIDITY 72%` → `RAIN 20%`
- **Message:** shows any text you type.

You can control it from the USB Serial Monitor, from a web page
(`http://ledsign.local` on your home network), or from your own code. Settings,
including Wi-Fi and location, are saved in flash and survive power cycles.

| Document | What it covers |
|---|---|
| [`docs/80x7_Transit_Sign_1105-2111.pdf`](docs/80x7_Transit_Sign_1105-2111.pdf) | Sign specs, power, connector pinout (sister model, see below) |
| [`docs/Alpha_Sign_Communications_Protocol_9708-8061F.pdf`](docs/Alpha_Sign_Communications_Protocol_9708-8061F.pdf) | Alpha protocol reference (packet format, modes, colours) |

## About the part number

This sign is **PN 1105-2126**. The only datasheet available is for
**PN 1105-2111**, another variant in the same 80×7 transit family (same case,
matrix, 12 V supply and RS-485/J1708 interface). Adaptive doesn't publish what
the last digits change. Typical differences between variants are LED colour,
connector/cable, and the protocol firmware loaded at the factory.

The firmware is written so those differences don't matter:

- **Address and sign type:** every packet uses type `Z` (all signs) and address `00` (broadcast), so the sign responds whatever address it was programmed with.
- **Baud rate and framing:** the sign autobauds on the leading NULs, and `/probe` tries every baud rate in both 7E2 and 8N1.
- **Red vs. tricolor:** colour codes are only sent if you pick a colour. A red-only sign ignores them.
- **Protocol:** if the factory loaded a J1708/J1587 transit protocol instead of Alpha, `/sniff` lets you capture what the original controller sends.

**Check before powering up:** the pinout below comes from the 1105-2111
datasheet. Before connecting 12 V, confirm your pigtail matches it: a
**6-pin** Mate-N-Lok with heavy (16 AWG) red, orange and black wires and two
thin (20 AWG) black and white wires. If your wires differ, don't guess.
Look for a label on the sign or cable, or tell me what you see.

## Parts

| Part | Notes |
|---|---|
| ESP32 dev board | Any ESP32 / ESP32-S3 board. On an ESP32-C3, change the RX/TX pins. |
| RS-485 module | **DIYables RS485-TTL (auto flow control)** or any similar auto-direction module (pins VCC/GND/RXD/TXD). Power it from **3.3 V**. A MAX3485/SP3485 module with DE/RE pins also works (see below). Avoid bare 5 V MAX485 boards: their RO output would put 5 V on an ESP32 pin. |
| 12 V power supply, ≥ 5 A | The sign draws about 36 W (3 A at 12 V). It accepts 9–26 V DC. |
| 12 V → 5 V buck converter | Powers the ESP32 from the same supply (or use USB while testing). |
| Mating connector | The sign's 1 ft pigtail ends in an AMP Mate-N-Lok 6-pin cap (350781-1). It mates with a 6-pin Mate-N-Lok plug (e.g. TE 350715-1 + socket contacts). Check this against your cable, or cut the pigtail and splice it. |
| 5 A inline fuse | On the +12 V feed. |

## Wiring

Sign connector pinout (from the sign datasheet):

| Pin | Wire | Function | Connect to |
|---|---|---|---|
| 1 | Black 16 AWG | Power ground | Supply **−** and ESP32/transceiver **GND** |
| 2 | — | No connect | — |
| 3 | Black 20 AWG | J1708A (RS-485 **A / +**) | Transceiver **A** |
| 4 | Red 16 AWG | +12 V unswitched | Supply **+** (via fuse) |
| 5 | Orange 16 AWG | +12 V switched (ignition) | Supply **+** (via fuse). The sign stays off without it. |
| 6 | White 20 AWG | J1708B (RS-485 **B / −**) | Transceiver **B** |

```
                 +12V ──[5A fuse]──┬──────────────► sign pin 4 (red)
                                   ├──────────────► sign pin 5 (orange)
                                   └──► buck 12V→5V ──► ESP32 5V/VIN
                  GND ─────────────┬──────────────► sign pin 1 (black, 16 AWG)
                                   ├──► buck GND
                                   └──► ESP32 GND ─── RS-485 module GND

   ESP32                 DIYables RS485 module        Sign
   3V3      ───────────► VCC
   GND      ───────────► GND
   GPIO17 (TX) ────────► RXD
   GPIO16 (RX) ◄──────── TXD
                         A+ ───────────────────────► pin 3 (black, 20 AWG)
                         B- ───────────────────────► pin 6 (white)
                         GND ──────────────────────► pin 1 (ground)
```

The module's **RXD/TXD are labelled from the module's side**, so they cross
over: ESP32 TX → module RXD, module TXD → ESP32 RX. The firmware's default
`RS485_DE_PIN = -1` is right for this module because it switches between
sending and receiving by itself.

**Using a MAX3485/SP3485 module with DE/RE pins instead:** wire GPIO17 → DI,
GPIO16 ← RO, tie DE and RE together to GPIO4, and set `RS485_DE_PIN = 4` in
`led_sign.ino`.

- **Grounds must be common.** The ESP32/transceiver ground has to connect to sign pin 1, or the RS-485 lines float.
- Auto-direction modules usually have a 120 Ω termination resistor fitted. That's fine on this short cable; leave it in place.
- A 120 Ω terminating resistor isn't needed for the 1–2 m cable used here.
- Vendors label A/B inconsistently. If nothing shows up, **swap A and B** first.

## Building and flashing

**Arduino IDE**

1. Install the ESP32 board package: *Boards Manager → "esp32" by Espressif*.
2. Open `firmware/led_sign/led_sign.ino`.
3. Select your board (e.g. *ESP32 Dev Module*) and port, then upload.
4. Open the Serial Monitor at **115200** baud with line ending **Newline**.

**PlatformIO**

```sh
pio run -t upload && pio device monitor
```

Pins, the fallback access point and timing settings are at the top of
`led_sign.ino`. You don't need to put your Wi-Fi password in the code (see
below).

## First power-up: finding the serial settings

The sign datasheet only says "RS485". The protocol manual allows 1200–38400
baud in either **7E2** (Alpha 1.0, older signs like this 2004 unit) or **8N1**
(Alpha 2.0+). Alpha signs also *autobaud* on the five NUL bytes that start
every packet, so the default **9600 7E2** will probably just work.

If it doesn't:

1. Type `/probe` in the Serial Monitor. The ESP32 tries every baud/format
   combination, showing text like `9600 7E2` on the sign for 5 seconds each.
2. When you see text appear, press Enter to stop the probe. Then set that combination,
   e.g. `/baud 9600` and `/fmt 7E2`. Settings are saved.
3. If nothing appears at all, swap the A/B wires, check pin 5 has 12 V, check
   that the grounds are common, and run `/probe` again.

If the sign still shows nothing, it may have been configured for a transit
J1708/J1587 protocol instead of Alpha. If you have the vehicle's original head
unit, connect it to the same A/B wires, type `/sniff`, and capture what it sends.

## Setting up weather

1. **Connect to your home Wi-Fi.** In the Serial Monitor type:
   ```
   /ssid MyHomeNetwork
   /pass my-wifi-password
   ```
   The ESP32 prints its address once connected, e.g.
   `Wi-Fi connected: http://ledsign.local or http://192.168.1.42`.
   The ESP32 only supports **2.4 GHz** Wi-Fi.

   *No USB handy?* With no saved network (or if it can't connect for 20 s), the
   ESP32 starts its own Wi-Fi network **`LED-Sign`** (password `ledsign123`).
   Join it, open **http://192.168.4.1**, and fill in *Home Wi-Fi*.

2. **Set your location:**
   ```
   /location Milwaukee
   ```
   Use only the city name. Open-Meteo's search doesn't understand "Milwaukee, WI". If it
   picks the wrong place, use coordinates instead (right-click a spot in Google
   Maps to copy them). The optional label is what the sign shows:
   ```
   /latlon 43.0389 -87.9065 HOME
   ```
   Setting a location switches the sign to weather mode and fetches right away.

3. **Optional:** `/units c` for Celsius, `/showplace off` to drop the city
   name from the cycle.

The ESP32 sends the whole cycle to the sign as one multi-page message, so the
sign does the cycling itself. Pages that fit (about 13 characters) hold still;
longer ones scroll. The sign is only rewritten when the numbers change, plus
once an hour in case the sign was power cycled on its own. If updates fail for
3 hours the sign shows `WEATHER OFFLINE` instead of old data.

Typing a message switches to message mode. `/weather on` switches back.

## Using it

### Serial Monitor

Type any text and press Enter to display it (this turns weather off). Commands:

| Command | Effect |
|---|---|
| `/ssid <name>`, `/pass <password>` | Home Wi-Fi network and password (saved; reconnects) |
| `/location <city>` | Look up a city and show its weather |
| `/latlon <lat> <lon> [label]` | Set the weather location by coordinates |
| `/units f\|c` | Fahrenheit or Celsius |
| `/showplace on\|off` | Include the city name in the cycle |
| `/weather on\|off` | Switch between weather and your message |
| `/update` | Fetch the weather now |
| `/mode <name>` | `rotate` `hold` `flash` `rollup` `rolldown` `rollleft` `rollright` `wipeup` `wipedown` `wipeleft` `wiperight` `scroll` `auto` `rollin` `rollout` `wipein` `wipeout` `compressed` `twinkle` `sparkle` `snow` `interlock` `switch` `slide` `spray` `starburst` |
| `/speed <0-5>` | 1 = slowest, 5 = fastest, 0 = sign default |
| `/color <name\|none>` | `red` `green` `amber` `orange` `yellow` `rainbow1` … (tricolor signs only) |
| `/priority on\|off` | Use the priority file. It overrides all other messages and holds at most 125 bytes. |
| `/clear` | Clear the priority message |
| `/baud <rate>`, `/fmt 7E2\|8N1` | Serial settings |
| `/checksum on\|off` | Append a checksum so the sign rejects corrupted packets |
| `/probe` | Try every baud/format combination |
| `/sniff` | Print bytes received on the RS-485 bus (hex + ASCII) |
| `/resend`, `/status`, `/help` | |

At 80 columns with a 7-row font, about 13 characters fit on screen. Longer
messages should use `rotate` (scrolling).

### Web page

On your home network, browse to **http://ledsign.local**, or to the IP address
shown by `/status` (some Android phones don't resolve `.local` names). The page
lets you:

- set the city and units and switch to weather;
- send a message with a mode, speed, colour and priority;
- change the home Wi-Fi network.

The page has no login, so anyone on your home network can change the sign.
The fallback `LED-Sign` network only runs while home Wi-Fi is unavailable.
**Change its password** (`AP_PASSWORD` in `led_sign.ino`).

Your Wi-Fi password is stored in the ESP32's flash (not encrypted) and is never
shown on the web page or in `/status`.

### From your own code

```cpp
showMessage("LAP 3  1:23.4");   // uses the current mode/speed/colour
settings.text.mode = alpha::findMode("flash");
showMessage("BOX BOX");
setWeatherMode(true);           // back to the weather cycle
```

`AlphaSign.h` is a standalone encoder with no Arduino dependencies:

```cpp
alpha::TextOptions opts;
opts.mode = alpha::findMode("hold");
uint8_t buf[300];
size_t len = alpha::buildWriteText(buf, sizeof(buf), opts, "HELLO");
// write buf[0..len) to the RS-485 UART

// Several pages that the sign cycles through on its own:
alpha::Page pages[] = {{"HIGH 74F", alpha::findMode("hold")},
                       {"PARTLY CLOUDY", alpha::findMode("rotate")}};
len = alpha::buildWriteTextPages(buf, sizeof(buf), opts, pages, 2);
```

`Weather.h` builds the Open-Meteo request URLs and parses the responses
(no JSON library needed).

## How the protocol works

Each message is a single packet (manual section 5.1 / 6.1.1):

```
00 00 00 00 00   01   'Z'   '0' '0'   02   'A'   'A'   1B 20 'a'   HELLO   04
└── 5×NUL ────┘  SOH  type  address   STX  write file  ESC pos mode  text   EOT
   (autobaud)        (all) (broadcast)     TEXT   'A'
```

- A TEXT file can hold several pages. Each page is another `ESC pos mode text` group, and the sign shows them in turn and loops. The weather cycle uses this.
- The sign has no degree symbol (the extended character set is only accented letters and currency), so temperatures show as `74F`.
- File `A` exists from power-up, so no memory configuration is needed.
- File `0` is the priority file. It overrides everything until an empty priority message is sent.
- The ESP32 strips control characters from user text so typed input can't break the framing.

## Tests

The packet encoder is unit tested on a PC against the examples in the
protocol manual (Appendix F). The weather parser is tested against
Open-Meteo-shaped responses, including `null` values and accented place names:

```sh
g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_alpha.cpp -o test_alpha && ./test_alpha
g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_weather.cpp -o test_weather && ./test_weather
```
