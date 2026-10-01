# RS-485 LED Matrix Sign Controller (ESP32)

ESP32 firmware for driving an **Adaptive Micro Systems 80×7 transit sign**
(PN 1105-2111) over RS-485 using the **Alpha sign protocol**.

You can send messages from the USB Serial Monitor, from a web page served by
the ESP32 over Wi-Fi, or from your own code with `showMessage("...")`. The last
message is saved in flash and sent again whenever the ESP32 starts.

| Document | What it covers |
|---|---|
| [`docs/80x7_Transit_Sign_1105-2111.pdf`](docs/80x7_Transit_Sign_1105-2111.pdf) | Sign specs, power, connector pinout |
| [`docs/Alpha_Sign_Communications_Protocol_9708-8061F.pdf`](docs/Alpha_Sign_Communications_Protocol_9708-8061F.pdf) | Alpha protocol reference (packet format, modes, colours) |

## Parts

| Part | Notes |
|---|---|
| ESP32 dev board | Any ESP32 / ESP32-S3 board. On an ESP32-C3, change the RX/TX pins. |
| RS-485 transceiver, **3.3 V** | MAX3485 / SP3485 module. Avoid 5 V MAX485 modules: their RO output would put 5 V on an ESP32 pin. |
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
                                   └──► ESP32 GND ─── transceiver GND

   ESP32                 MAX3485 module               Sign
   3V3      ───────────► VCC
   GPIO17 (TX) ────────► DI
   GPIO16 (RX) ◄──────── RO
   GPIO4    ───────────► DE + RE (tied)
                         A  ───────────────────────► pin 3 (black, 20 AWG)
                         B  ───────────────────────► pin 6 (white)
```

- **Grounds must be common.** The ESP32/transceiver ground has to connect to sign pin 1, or the RS-485 lines float.
- If your transceiver module switches direction automatically (no DE/RE pins), set `RS485_DE_PIN = -1` in `led_sign.ino`.
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

The defaults are at the top of `led_sign.ino`: pins, Wi-Fi name/password, and an
optional existing network to join.

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

## Using it

### Serial Monitor

Type any text and press Enter to display it. Commands:

| Command | Effect |
|---|---|
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

### Wi-Fi

Join the Wi-Fi network **`LED-Sign`** (password `ledsign123`) and browse to
**http://192.168.4.1**. The page lets you type a message and pick mode, speed,
colour and priority. **Change the password** in `led_sign.ino` before using it
anywhere public.

### From your own code

```cpp
showMessage("LAP 3  1:23.4");   // uses the current mode/speed/colour
settings.text.mode = alpha::findMode("flash");
showMessage("BOX BOX");
```

`AlphaSign.h` is a standalone encoder with no Arduino dependencies:

```cpp
alpha::TextOptions opts;
opts.mode = alpha::findMode("hold");
uint8_t buf[300];
size_t len = alpha::buildWriteText(buf, sizeof(buf), opts, "HELLO");
// write buf[0..len) to the RS-485 UART
```

## How the protocol works

Each message is a single packet (manual section 5.1 / 6.1.1):

```
00 00 00 00 00   01   'Z'   '0' '0'   02   'A'   'A'   1B 20 'a'   HELLO   04
└── 5×NUL ────┘  SOH  type  address   STX  write file  ESC pos mode  text   EOT
   (autobaud)        (all) (broadcast)     TEXT   'A'
```

- File `A` exists from power-up, so no memory configuration is needed.
- File `0` is the priority file. It overrides everything until an empty priority message is sent.
- The ESP32 strips control characters from user text so typed input can't break the framing.

## Tests

The packet encoder is unit tested on a PC against the examples in the
protocol manual (Appendix F):

```sh
g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_alpha.cpp -o test_alpha && ./test_alpha
```
