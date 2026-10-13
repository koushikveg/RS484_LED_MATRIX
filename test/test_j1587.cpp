// Host-side tests for J1587Sign.h. The expected bytes are copied from a
// known-good Linux program (j1587_data.c) that displays "HELLO" on the same
// TDR80x7 sign over a USB-RS485 adapter at 9600 8N1.
//
//   g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_j1587.cpp -o test_j1587 && ./test_j1587

#include <stdio.h>
#include <string.h>

#include <vector>

#include "J1587Sign.h"

static int failures = 0;

#define CHECK(cond)                                    \
  do {                                                 \
    if (cond) {                                        \
      printf("ok   %s\n", #cond);                      \
    } else {                                           \
      ++failures;                                      \
      printf("FAIL %s  (line %d)\n", #cond, __LINE__); \
    }                                                  \
  } while (0)

static std::vector<uint8_t> bytes(const uint8_t* p, size_t n) { return std::vector<uint8_t>(p, p + n); }

int main() {
  // ---- known-good packet 1: PID 0xC5 RTS
  {
    const uint8_t expected[] = {0xBC, 0xC5, 0x07, 0xBD, 0x01, 0x00, 0x00, 0x00, 0x42, 0x01, 0x77};
    uint8_t out[j1587::MAX_FRAME];
    size_t n = j1587::buildRts(out, j1587::RTS_FIXED, 1, 8);
    CHECK(bytes(out, n) == bytes(expected, sizeof(expected)));
    CHECK(j1587::checksumOk(out, n));
  }

  // ---- known-good packet 2: PID 0xC6 data, "HELLO"
  {
    const uint8_t expected[] = {0xBC, 0xC6, 0x0A, 0xBD, 0x01, 'A', 0x42,
                                'H',  'E',  'L',  'L',  'O',  0x04, 0xBB};
    uint8_t payload[64];
    size_t plen = j1587::buildPayload(payload, sizeof(payload), "HELLO", 0);
    CHECK(plen == 8);
    CHECK(j1587::segmentsFor(plen) == 1);
    uint8_t out[j1587::MAX_FRAME];
    size_t n = j1587::buildSegment(out, payload, plen, 0);
    CHECK(bytes(out, n) == bytes(expected, sizeof(expected)));
    CHECK(j1587::buildSegment(out, payload, plen, 1) == 0);
  }

  // ---- checksum helper agrees with the reference program's values
  {
    const uint8_t rts[] = {0xBC, 0xC5, 0x07, 0xBD, 0x01, 0x00, 0x00, 0x00, 0x42, 0x01};
    CHECK(j1587::checksum(rts, sizeof(rts)) == 0x77);
    const uint8_t bad[] = {0xBC, 0xC5, 0x07, 0x00};
    CHECK(!j1587::checksumOk(bad, sizeof(bad)));
  }

  // ---- mode prefix and sanitising
  {
    uint8_t payload[64];
    size_t plen = j1587::buildPayload(payload, sizeof(payload), "H\x01i\n", 'a');
    const uint8_t expected[] = {'A', 'B', 0x1B, 0x20, 'a', 'H', 'i', 0x04};
    CHECK(bytes(payload, plen) == bytes(expected, sizeof(expected)));
    CHECK(j1587::buildPayload(payload, 4, "TOO LONG", 0) == 0);
  }

  // ---- weather-style screens: each keeps its own scroll mode, in order
  {
    j1587::Page pages[] = {{"AMES", 'a', 0, 0}, {"NOW 62F  HIGH 66F  LOW 51F", 'a', 0, 0}, {"X", 'n', '7', 0}};
    uint8_t payload[j1587::MAX_PAYLOAD];
    size_t plen = j1587::buildPayloadPages(payload, sizeof(payload), pages, 3);
    std::vector<uint8_t> expected = {'A', 'B', 0x1B, 0x20, 'a', 'A', 'M', 'E', 'S', 0x1B, 0x20, 'a'};
    const char* t = "NOW 62F  HIGH 66F  LOW 51F";
    expected.insert(expected.end(), t, t + strlen(t));
    const uint8_t tail[] = {0x1B, 0x20, 'n', '7', 'X', 0x04};
    expected.insert(expected.end(), tail, tail + sizeof(tail));
    CHECK(bytes(payload, plen) == expected);
    CHECK(plen == 44);  // 2 + (3+4) + (3+26) + (4+1) + EOT
    CHECK(j1587::segmentsFor(plen) == 3);  // 15 + 15 + 14
  }

  // ---- scroll speed code after each mode field (1 = 0x15 ... 5 = 0x19)
  {
    j1587::Page pages[] = {{"HI", 'a', 0, 5}, {"YO", 'a', 0, 1}, {"NO", 0, 0, 0}};
    uint8_t payload[64];
    size_t plen = j1587::buildPayloadPages(payload, sizeof(payload), pages, 3);
    const uint8_t expected[] = {'A', 'B', 0x1B, 0x20, 'a', 0x19, 'H', 'I',
                                0x1B, 0x20, 'a', 0x15, 'Y', 'O', 'N', 'O', 0x04};
    CHECK(bytes(payload, plen) == bytes(expected, sizeof(expected)));
    j1587::Page bad = {"X", 'a', 0, 9};  // out of range: no speed byte
    plen = j1587::buildPayloadPages(payload, sizeof(payload), &bad, 1);
    CHECK(plen == 7);
  }

  // ---- long text splits into <= 21-byte frames, numbered from 1
  {
    const char* text = "NOW 62F  HIGH 66F  LOW 51F";  // 26 chars -> 29-byte payload
    uint8_t payload[j1587::MAX_PAYLOAD];
    size_t plen = j1587::buildPayload(payload, sizeof(payload), text, 0);
    CHECK(plen == 29);
    CHECK(j1587::segmentsFor(plen) == 2);

    std::vector<uint8_t> rebuilt;
    for (size_t s = 0; s < j1587::segmentsFor(plen); ++s) {
      uint8_t out[j1587::MAX_FRAME];
      size_t n = j1587::buildSegment(out, payload, plen, s);
      CHECK(n <= j1587::MAX_FRAME);
      CHECK(out[0] == 0xBC && out[1] == 0xC6 && out[3] == 0xBD);
      CHECK(out[4] == s + 1);
      CHECK(out[2] == n - 4);  // count = everything between count byte and checksum
      CHECK(j1587::checksumOk(out, n));
      rebuilt.insert(rebuilt.end(), out + 5, out + n - 1);
    }
    CHECK(rebuilt == bytes(payload, plen));

  }

  // ---- RTS layouts (multi-segment message: 3 segments, 44 payload bytes)
  {
    uint8_t r[j1587::MAX_FRAME];
    j1587::buildRts(r, j1587::RTS_FIXED, 3, 44);
    const uint8_t fixed[] = {0xBC, 0xC5, 0x07, 0xBD, 0x01, 0x00, 0x00, 0x00, 0x42, 0x01, 0x77};
    CHECK(bytes(r, 11) == bytes(fixed, 11));  // fixed layout ignores size: always known-good
    j1587::buildRts(r, j1587::RTS_STANDARD, 3, 44);
    CHECK(r[5] == 3 && r[6] == 44 && r[7] == 0 && r[8] == 0x42 && r[9] == 0x01 && j1587::checksumOk(r, 11));
    j1587::buildRts(r, j1587::RTS_SEGS_FIRST, 3, 44);
    CHECK(r[5] == 3 && r[6] == 0 && r[7] == 0 && r[9] == 0x01 && j1587::checksumOk(r, 11));
    j1587::buildRts(r, j1587::RTS_TOTAL_ONLY, 3, 300);
    CHECK(r[5] == 0 && r[6] == 44 && r[7] == 1 && r[9] == 0x01 && j1587::checksumOk(r, 11));  // 300 = 0x012C
    j1587::buildRts(r, j1587::RTS_SEGS_LAST, 6, 80);
    CHECK(r[5] == 0 && r[9] == 6 && j1587::checksumOk(r, 11));
  }

  // ---- the sign's real replies (captured on hardware)
  {
    const uint8_t cts[] = {0xBD, 0xC5, 0x06, 0xBC, 0x02, 0x00, 0x01, 0x42, 0x01, 0x76};
    const uint8_t eom[] = {0xBD, 0xC5, 0x04, 0xBC, 0x03, 0x42, 0x01, 0x78};
    CHECK(j1587::checksumOk(cts, sizeof(cts)) && j1587::checksumOk(eom, sizeof(eom)));
    CHECK(j1587::findCts(cts, sizeof(cts)));
    CHECK(!j1587::findEom(cts, sizeof(cts)));
    CHECK(j1587::findEom(eom, sizeof(eom)));
    CHECK(!j1587::findCts(eom, sizeof(eom)));
    uint8_t data[8];
    size_t dlen = 0;
    CHECK(j1587::findConnMgmt(cts, sizeof(cts), j1587::CM_CTS, data, sizeof(data), &dlen));
    CHECK(dlen == 4 && data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x42 && data[3] == 0x01);
    // with junk before it, and our own RTS echo (from BC, not the sign) ignored
    const uint8_t mixed[] = {0x00, 0xBC, 0xC5, 0x07, 0xBD, 0x01, 0x00, 0x00, 0x00, 0x42, 0x01, 0x77,
                             0xBD, 0xC5, 0x06, 0xBC, 0x02, 0x00, 0x01, 0x42, 0x01, 0x76};
    CHECK(j1587::findCts(mixed, sizeof(mixed)));
    CHECK(!j1587::findCts(mixed, 12));
    uint8_t bad[sizeof(cts)];
    memcpy(bad, cts, sizeof(cts));
    bad[9] ^= 0xFF;  // corrupt checksum
    CHECK(!j1587::findCts(bad, sizeof(bad)));
  }

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
