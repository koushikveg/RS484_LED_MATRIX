// Host-side tests for AlphaSign.h. Expected bytes come from the examples in
// the Alpha Sign Communications Protocol manual (Appendix F).
//
//   g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_alpha.cpp -o test_alpha && ./test_alpha

#include <stdio.h>
#include <string.h>

#include <string>

#include "AlphaSign.h"

static int failures = 0;

static std::string hex(const uint8_t* b, size_t n) {
  std::string s;
  char tmp[4];
  for (size_t i = 0; i < n; ++i) {
    snprintf(tmp, sizeof(tmp), "%02X ", b[i]);
    s += tmp;
  }
  return s;
}

static void expectPacket(const char* name, const alpha::TextOptions& o,
                         const char* text, const std::string& expected) {
  uint8_t buf[512];
  size_t n = alpha::buildWriteText(buf, sizeof(buf), o, text);
  std::string got((const char*)buf, n);
  if (got != expected) {
    ++failures;
    printf("FAIL %s\n  expected: %s\n  got:      %s\n", name,
           hex((const uint8_t*)expected.data(), expected.size()).c_str(),
           hex(buf, n).c_str());
  } else {
    printf("ok   %s\n", name);
  }
}

static const std::string HDR("\x00\x00\x00\x00\x00\x01", 6);

int main() {
  // Table 38: <NUL>x5 <SOH> "Z00" <STX> "AAHELLO" <EOT>
  {
    alpha::TextOptions o;
    o.mode = -1;
    expectPacket("manual example: HELLO to all signs", o, "HELLO",
                 HDR + "Z00\x02" "AAHELLO\x04");
  }

  // Table 39: <NUL>x5 <SOH> "102" <STX> "AAHELLO" <EOT>
  {
    alpha::TextOptions o;
    o.mode = -1;
    o.typeCode = alpha::TYPE_ONE_LINE;
    strcpy(o.address, "02");
    expectPacket("manual example: 1-line sign at address 02", o, "HELLO",
                 HDR + "102\x02" "AAHELLO\x04");
  }

  // Mode field: ESC, position 0x20, mode 'b' (HOLD)
  {
    alpha::TextOptions o;
    o.mode = alpha::findMode("hold");
    expectPacket("hold mode", o, "HI", HDR + "Z00\x02" "AA\x1B\x20" "bHI\x04");
  }

  // Special mode: 'n' + specifier '7' (STARBURST)
  {
    alpha::TextOptions o;
    o.mode = alpha::findMode("starburst");
    expectPacket("special mode", o, "X", HDR + "Z00\x02" "AA\x1B\x20" "n7X\x04");
  }

  // Speed 5 -> 0x19, colour green -> 0x1C '2'
  {
    alpha::TextOptions o;
    o.mode = alpha::findMode("rotate");
    o.speed = 5;
    o.color = alpha::findColor("GREEN");
    expectPacket("speed + colour", o, "GO",
                 HDR + "Z00\x02" "AA\x1B\x20" "a\x19\x1C" "2GO\x04");
  }

  // Control characters in user text are dropped.
  {
    alpha::TextOptions o;
    o.mode = -1;
    expectPacket("sanitises text", o, "A\x04" "B\x01\nC",
                 HDR + "Z00\x02" "AAABC\x04");
  }

  // Table 41: <NUL>x5 <SOH> "Z00" <STX> "AAHELLO" <ETX> "01FB" <EOT>
  {
    alpha::TextOptions o;
    o.mode = -1;
    o.checksum = true;
    expectPacket("manual example: checksum", o, "HELLO",
                 HDR + "Z00\x02" "AAHELLO\x03" "01FB\x04");
  }

  // Priority file is capped at 125 bytes of data (mode field counts).
  {
    alpha::TextOptions o;
    o.fileLabel = alpha::FILE_PRIORITY;
    o.mode = alpha::findMode("hold");
    std::string longText(200, 'x');
    std::string expected = HDR + "Z00\x02" "A0\x1B\x20" "b" + std::string(122, 'x') + "\x04";
    expectPacket("priority length cap", o, longText.c_str(), expected);
  }

  // Too-small buffer reports failure instead of truncating silently.
  {
    uint8_t small[8];
    alpha::TextOptions o;
    size_t n = alpha::buildWriteText(small, sizeof(small), o, "HELLO");
    if (n != 0) {
      ++failures;
      printf("FAIL overflow returned %zu\n", n);
    } else {
      printf("ok   overflow detected\n");
    }
  }

  // Lookups
  if (alpha::findMode("nope") != -1 || alpha::findColor("nope") != -1) {
    ++failures;
    printf("FAIL unknown names should return -1\n");
  } else {
    printf("ok   unknown names\n");
  }

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
