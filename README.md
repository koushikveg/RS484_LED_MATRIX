# RS-485 LED Matrix Sign Controller (ESP32)

ESP32 firmware for driving an **Adaptive Micro Systems 80×7 transit sign**
(PN 1105-2126, model TDR80x7-.3A) over its RS-485 / J1708 wires.

This sign only accepts text through the **SAE J1587 transport protocol**
(request to send, then clear to send, then data) at 9600 8N1. It ignores
plain Alpha packets. The firmware uses J1587 by default; plain Alpha protocol
is still available for regular Alpha signs (`/protocol alpha`).

It has two display modes:

- **Weather (default, set to Ames, Iowa):** the ESP32 joins your home Wi-Fi,
  pulls today's forecast from [Open-Meteo](https://open-meteo.com) (free, no
  API key) every 5 minutes, and the sign scrolls it right to left, one screen
  after another:
  `AMES` → `PARTLY CLOUDY` → `NOW 62F  HIGH 66F  LOW 51F` → `SNOW 1.2 IN  40% CHANCE`
- **Message:** shows any text you type.

You can control it from the USB Serial Monitor, from a web page
(`http://ledsign.local` on your home network), or from your own code. Settings,
including Wi-Fi and location, are saved in flash and survive power cycles.

| Document | What it covers |
|---|---|
| [`docs/80x7_Transit_Sign_1105-2111.pdf`](docs/80x7_Transit_Sign_1105-2111.pdf) | Sign specs, power, connector pinout (sister model, see below) |
| [`docs/Alpha_Sign_Communications_Protocol_9708-8061F.pdf`](docs/Alpha_Sign_Communications_Protocol_9708-8061F.pdf) | Alpha protocol reference (packet format, modes, colours) |
| [`docs/j1587_data.c`](docs/j1587_data.c) | Known-good Linux program that shows "HELLO" on this sign over a USB-RS485 adapter; the J1587 byte layout comes from it |

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

This project was built and tested with the **Waveshare ESP32-S3-RS485-CAN**
module, bought on Amazon (sold as KLAYERS):
[amazon.com/dp/B0FWK6TL4L](https://www.amazon.com/dp/B0FWK6TL4L). See the
[Waveshare product page](https://www.waveshare.com/esp32-s3-rs485-can.htm) and the
[Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN) for its documentation.

| Part | Notes |
|---|---|
| **ESP32-S3 RS485/CAN board** | [Waveshare ESP32-S3-RS485-CAN](https://www.amazon.com/dp/B0FWK6TL4L), also sold as KLAYERS. It has an isolated RS-485 transceiver built in and runs on 7–36 V, so it can share the sign's 12 V supply. The firmware defaults are for this board. |
| 12 V power supply, ≥ 5 A | The sign draws about 36 W (3 A at 12 V). It accepts 9–26 V DC. |
| Mating connector | The sign's 1 ft pigtail ends in an AMP Mate-N-Lok 6-pin cap (350781-1). It mates with a 6-pin Mate-N-Lok plug (e.g. TE 350715-1 + socket contacts). Check this against your cable, or cut the pigtail and splice it. |
| 5 A inline fuse | On the +12 V feed. |

## Wiring

Sign connector pinout (from the sign datasheet):

| Pin | Wire | Function | Connect to |
|---|---|---|---|
| 1 | Black 16 AWG | Power ground | Supply **−**, board power **GND**, and board RS-485 **GND** |
| 2 | — | No connect | — |
| 3 | Black 20 AWG | J1708A (RS-485 **A / +**) | Board RS-485 **A** |
| 4 | Red 16 AWG | +12 V unswitched | Supply **+** (via fuse) |
| 5 | Orange 16 AWG | +12 V switched (ignition) | Supply **+** (via fuse). The sign stays off without it. |
| 6 | White 20 AWG | J1708B (RS-485 **B / −**) | Board RS-485 **B** |

```
   12 V supply                                 Sign
   + ──[5A fuse]──┬──────────────────────────► pin 4 (red)
                  ├──────────────────────────► pin 5 (orange)
                  └──► board power in (7-36V)
   − ─────────────┬──────────────────────────► pin 1 (black, 16 AWG)
                  └──► board power GND

   ESP32-S3 RS485/CAN board
   RS485 A   ────────────────────────────────► pin 3 (black, 20 AWG)
   RS485 B   ────────────────────────────────► pin 6 (white)
   RS485 GND ────────────────────────────────► pin 1 (ground)
```

- **Connect the RS-485 GND terminal too.** The board's RS-485 port is
  isolated from the rest of the board, so its ground is separate from the
  power ground. Without a ground reference to the sign, the A/B signals float.
- The board has a 120 Ω termination resistor that's **off by default**
  (jumper). Leave it off; the cable to the sign is short.
- Vendors label A/B inconsistently. If nothing shows up, **swap A and B** first.
- While testing on the bench you can power the board from USB-C alone. The
  sign still needs its own 12 V.

The board's RS-485 transceiver is wired to TX = GPIO17, RX = GPIO18, and a
driver-enable line on GPIO21, which the firmware raises while sending. These
come from community configs for the Waveshare board and haven't been checked
against a KLAYERS unit. If `/probe` shows nothing even after swapping A/B,
compare the pin table in the board's manual with the top of `led_sign.ino`.

### Using a plain ESP32 and a separate RS-485 module instead

Edit the pin settings at the top of `led_sign.ino`:

| Module | Wiring | Settings |
|---|---|---|
| Auto-direction (DIYables RS485-TTL etc.: VCC, GND, RXD, TXD) | ESP32 TX(17) → module **RXD**, module **TXD** → ESP32 RX(16), VCC to **3.3 V** | `RX=16, TX=17, DE=-1` |
| MAX3485 / SP3485 (DI, RO, DE, RE) | TX(17) → DI, RO → RX(16), DE+RE tied → GPIO4, VCC to 3.3 V | `RX=16, TX=17, DE=4` |

Power a separate module from 3.3 V (a 5 V module would send 5 V back into an
ESP32 pin). Its A/B/GND go to sign pins 3/6/1, and the ESP32 needs a 12 V → 5 V
buck converter or USB for power.

## Building and flashing

**Arduino IDE**

1. Install the ESP32 board package: *Boards Manager → "esp32" by Espressif*.
2. Open `firmware/led_sign/led_sign.ino`.
3. Board: **ESP32S3 Dev Module**. Under *Tools*, set **USB CDC On Boot → Enabled**.
   Without that, Serial Monitor output goes to the board's internal UART instead
   of the USB-C port.
4. Pick the port, upload, and open the Serial Monitor at **115200** baud with
   line ending **Newline**.

If the upload doesn't start, hold **BOOT**, tap **RESET**, release **BOOT**, and
upload again. Press **RESET** afterwards to run the new firmware.

**PlatformIO**

```sh
pio run -t upload && pio device monitor          # ESP32-S3 RS485/CAN board
pio run -e esp32dev -t upload                    # plain ESP32 + separate module
```

Pins, the fallback access point and timing settings are at the top of
`led_sign.ino`. You don't need to put your Wi-Fi password in the code (see
below).

## First power-up: testing the sign link

1. Wire the sign and power it. It shows its boot screens
   (`1105-6001a …`, `32K RAM`, the time, `ADDRESS = 01`).
2. In the Serial Monitor type `/j1587test`. The ESP32 sends exactly the same
   bytes as the known-good Linux program and prints what the sign answers:
   ```
   [sign CTS: 10 bytes] BD C5 …
   ```
   `HELLO` should appear on the sign.
3. Type any message, or `/weather on` for the weather.

If there's no CTS line (`! no CTS from the sign`):
- Swap the A/B wires.
- Check that the RS-485 **GND** terminal is connected to sign pin 1.
- Check `/status` shows `protocol : J1587` and `rs485 : … DE 21 active HIGH`.

`/txtest` and `/txflip` check the RS-485 output with a multimeter, and
`/sniff` prints any bytes on the bus.

**Alpha protocol signs:** `/protocol alpha` switches to plain Alpha packets.
`/probe` then tries every Alpha baud rate, format and address.

## Connecting to Wi-Fi

The ESP32 needs your home Wi-Fi to download the weather. You enter the
network name and password **once, on the ESP32 itself**. They're saved in its
flash memory and survive power cycles and firmware updates (unless you turn on
*Tools → Erase All Flash Before Sketch Upload*). They are never stored in this
repository.

The ESP32-S3 only connects to **2.4 GHz** Wi-Fi. If your router has
separate 2.4 GHz and 5 GHz networks, use the 2.4 GHz one. A single name for
both bands usually works.

### Option A: Serial Monitor (USB)

1. Plug the board into your computer with USB-C and open the Arduino IDE
   Serial Monitor.
2. Set the baud rate to **115200** and the line ending to **Newline**
   (the two drop-downs in the Serial Monitor).
3. Type each line and press Enter, using your network's name and password:
   ```
   /ssid YourNetworkName
   /pass YourWiFiPassword
   ```
   Both are case-sensitive and can contain spaces.
4. Within a few seconds it should print:
   ```
   Wi-Fi connected: http://ledsign.local or http://192.168.1.42
   ```
   (your IP address will differ). Weather starts downloading straight away.

### Option B: from your phone (no USB needed)

1. Power the board. If it has no saved network, or can't reach the saved one
   for 20 seconds, it starts its own Wi-Fi network.
2. On your phone, join **`LED-Sign`** (password **`ledsign123`**).
3. Open **http://192.168.4.1** in the browser.
4. Under **Home Wi-Fi**, enter your network name and password and tap
   **Save and connect**.
5. Rejoin your normal Wi-Fi. The `LED-Sign` network turns itself off once
   the ESP32 is connected to your home network.

### Checking it worked

- Type `/status` in the Serial Monitor. The `wifi` line shows the network
  and the ESP32's IP address, and `weather` shows when it last updated.
- On a phone or computer on the same network, open **http://ledsign.local**.
  Some Android phones don't support `.local` addresses; use the IP address
  from `/status` instead.
- Once the weather downloads, the sign starts scrolling the Ames forecast.

### Changing networks later

Run `/ssid` and `/pass` again, or use the **Home Wi-Fi** form on the web page.
The new values replace the old ones. On the web page, leave the password
blank to keep the saved one.

### Building your Wi-Fi into the firmware (optional)

You can also put the network name and password in `DEFAULT_WIFI_SSID` and
`DEFAULT_WIFI_PASSWORD` at the top of `led_sign.ino` before flashing. They're
used until a network is saved with `/ssid`/`/pass` or the web page. **Don't
commit or push that change:** anything committed ends up on GitHub, where a
public repository would expose your Wi-Fi password.

### If it won't connect

| Symptom | Fix |
|---|---|
| `/status` says `not connected` | Re-enter the name and password; check capitals and spaces. |
| Still not connecting | Make sure the network is 2.4 GHz and the board is in range. |
| Nothing prints in the Serial Monitor | In the Arduino IDE set **Tools → USB CDC On Boot → Enabled** and re-flash; check the baud rate is 115200. |
| Commands do nothing | Set the Serial Monitor line ending to **Newline**. |
| `ledsign.local` doesn't open | Use the IP address from `/status`. |
| `LED-Sign` network doesn't appear | It only appears when home Wi-Fi isn't connected; wait 20 s after power-up. |

The Wi-Fi password is stored unencrypted in the ESP32's flash. It's never
shown on the web page or in `/status`.

## Setting up weather

1. **Wi-Fi:** connect it first (see [Connecting to Wi-Fi](#connecting-to-wi-fi)).

2. **Location:** it's preset to **Ames, Iowa, US** (42.0308, −93.6319, shown as
   `AMES`), so there's nothing to do. To change it, type the city, then
   optionally the state/province and the country, separated by commas, in
   the **Weather** box on `http://ledsign.local` or in the Serial Monitor:
   ```
   /location Des Moines, Iowa, US
   /location Des Moines, IA, USA
   /location Springfield, Illinois, United States of America
   /location Paris, France
   /location Paris, Texas
   /location Toronto, ON, Canada
   ```
   - The city comes first and is required. The state and country are optional,
     but they pick the right place when several cities share a name.
   - States and provinces can be written in full or abbreviated (`Iowa` or `IA`).
     This covers US states, Canadian provinces and Australian states.
   - Countries can be written as a code or a name. For the United States, `US`,
     `USA`, `U.S.A.`, `United States` and `United States of America` all work.
     `UK`, `Great Britain` and `GB` all work for the United Kingdom.
   - Upper/lower case and accents don't matter (`zurich` finds Zürich).
   - With two parts, the second can be a state or a country. `Atlanta, Georgia`
     finds the US state and `Tbilisi, Georgia` finds the country.

   If the city exists but not in the state/country you typed, the sign keeps its
   old location. The Serial Monitor and web page then list the places it did find.
   You can also use coordinates (right-click a spot in Google Maps to copy them).
   The optional label is what the sign shows:
   ```
   /latlon 42.0308 -93.6319 AMES
   ```
   Setting a location switches the sign to weather mode and fetches right away.
   To change the built-in default, edit `DEFAULT_PLACE`, `DEFAULT_PLACE_QUERY`,
   `DEFAULT_PLACE_DETAIL`, `DEFAULT_LATITUDE` and `DEFAULT_LONGITUDE` at the top
   of `led_sign.ino`.

3. **Optional:** `/units c` for Celsius, `/showplace off` to drop the city
   name from the cycle.

### What the sign shows

| Screen | Example | When |
|---|---|---|
| City | `AMES` | Always (turn off with `/showplace off`) |
| Today's conditions | `PARTLY CLOUDY` | Always |
| Temperatures | `NOW 62F  HIGH 66F  LOW 51F` | Always |
| Precipitation: type, amount and chance on one screen | `RAIN 0.30 IN  40% CHANCE` | When today's chance of precipitation is **above 5%** |
| Clouds: how cloudy the sky is | `MOSTLY CLOUDY  70% CLOUD COVER` | Instead of precipitation, when the chance is **5% or less** |

- Every screen scrolls right to left. To change that, set `WEATHER_SCREEN_MODE`
  at the top of `led_sign.ino` to another mode from `/help`, e.g. `"hold"`.
  (`/mode` only affects typed messages.) With `"hold"`, screens wider than the
  sign's ~13 characters are split into several held screens, e.g.
  `NOW 62F` → `HIGH 66F` → `LOW 51F`, so nothing is cut off.
- **Type** comes from Open-Meteo's forecast for today: whichever of rain
  (including showers) and snowfall has an amount forecast. If neither has an
  amount, today's weather code decides. The precipitation screen can read:

  | Forecast | Screen |
  |---|---|
  | Rain only | `RAIN 0.30 IN  40% CHANCE` |
  | Snow only | `SNOW 1.2 IN  40% CHANCE` |
  | Both | `RAIN 0.10 IN + SNOW 0.5 IN  60% CHANCE` |
  | Freezing rain (weather code) | `FREEZING RAIN 0.05 IN  30% CHANCE` |
  | A chance but no type forecast | `PRECIP <0.01 IN  20% CHANCE` |

- **Chance** is today's highest chance of any precipitation. Open-Meteo
  doesn't give separate rain and snow chances, so on a mixed day the one
  percentage covers both.
- **Amounts** are today's forecast totals in **inches**, even with
  `/units c`. Amounts too small to show read `<0.01 IN` (rain) or `<0.1 IN`
  (snow).
- **Cloud cover** is today's average of Open-Meteo's hourly cloud cover. The
  wording follows the National Weather Service's sky-condition bands:

  | Cloud cover | Shown as |
  |---|---|
  | 0–12% | `CLEAR` |
  | 13–37% | `MOSTLY CLEAR` |
  | 38–62% | `PARTLY CLOUDY` |
  | 63–87% | `MOSTLY CLOUDY` |
  | 88–100% | `CLOUDY` |

The ESP32 sends all the screens to the sign as one multi-page message, so the
sign does the cycling itself. The sign is only rewritten when the forecast
changes, plus once an hour in case the sign was power cycled on its own. If
updates fail for 3 hours the sign shows `WEATHER OFFLINE` instead of old data.

Typing a message switches to message mode. `/weather on` switches back.

## Using it

### Serial Monitor

Type any text and press Enter to display it (this turns weather off). Commands:

| Command | Effect |
|---|---|
| `/ssid <name>`, `/pass <password>` | Home Wi-Fi network and password (saved; reconnects) |
| `/location <city>[, <state>][, <country>]` | Look up a place and show its weather, e.g. `/location Ames, Iowa, US` (default) |
| `/latlon <lat> <lon> [label]` | Set the weather location by coordinates |
| `/units f\|c` | Fahrenheit or Celsius |
| `/showplace on\|off` | Include the city name in the cycle |
| `/weather on\|off` | Switch between weather and your message |
| `/update` | Fetch the weather now |
| `/mode <name>` | `rotate` `hold` `flash` `rollup` `rolldown` `rollleft` `rollright` `wipeup` `wipedown` `wipeleft` `wiperight` `scroll` `auto` `rollin` `rollout` `wipein` `wipeout` `compressed` `twinkle` `sparkle` `snow` `interlock` `switch` `slide` `spray` `starburst` |
| `/speed <0-5>` | Scroll speed for the weather and messages: 1 = slowest, 5 = fastest, 0 = sign default (also on the web page) |
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

- set the **scroll speed** (1 = slowest … 5 = fastest, or the sign's default)
  for everything on the sign. It's saved, and what's showing is re-sent at
  the new speed straight away. Same as `/speed` in the Serial Monitor;
- set the location (`City, State, Country`) and units and switch to weather;
- send a message with a mode, colour and priority;
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

### J1587 (this sign, the default)

Every J1708 message is `MID, PID, byte count, data…, checksum`. The checksum
makes the 8-bit sum of the whole message zero, and a message is at most 21
bytes. Sending "HELLO" (taken from [`docs/j1587_data.c`](docs/j1587_data.c)):

```
ESP32 → sign  BC C5 07 BD 01 00 00 00 42 01 77            RTS   (PID 197, to MID BD, 1 segment)
sign → ESP32  BD C5 …                                     CTS   (~10 bytes)
              80 ms pause
ESP32 → sign  BC C6 0A BD 01 41 42 48 45 4C 4C 4F 04 BB   data  (PID 198, segment 1: 'A' 'B' "HELLO" EOT)
```

- `BC` is the controller's MID, `BD` the sign's.
- The data is Alpha-style: write-text command `A`, file label `B`, the text, `EOT`.
- Text longer than 12 characters is split into several PID 198 segments (15 data bytes each). The sign's replies (CTS `BD C5 06 BC 02 00 01 42 01 76`, end of message `BD C5 04 BC 03 42 01 78`) end in the same `42 01` as the RTS, so that looks like a fixed ID, and long messages use the exact known-good RTS too.
  - A message counts as delivered when the sign answers with its **end-of-message** frame; the monitor prints `sign confirmed the message`.
  - If a long message isn't confirmed, the firmware tries other places the RTS could carry the size, and remembers the one the sign confirms. `/status` shows it as `long-message RTS`.
- Each screen keeps its own Alpha mode field (`ESC 0x20 a` = scroll right to left). The weather therefore plays as separate screens, one after another, exactly as with Alpha signs: `'A' 'B' ESC 20 'a' "AMES" ESC 20 'a' "PARTLY CLOUDY" … EOT`.
- The known-good example sends plain text with no mode codes. If the sign rejects the mode codes, `/j1587mode off` falls back to plain text, with all screens joined into one line.

### Alpha protocol (`/protocol alpha`)

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
g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_j1587.cpp -o test_j1587 && ./test_j1587
g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_geocode.cpp -o test_geocode && ./test_geocode
```

`test_j1587` checks the J1587 packets byte for byte against the known-good
Linux program.
