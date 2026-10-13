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
    size_t n = j1587::buildRts(out, 1);
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

    uint8_t rts[j1587::MAX_FRAME];
    size_t n = j1587::buildRts(rts, 2);
    CHECK(rts[9] == 2 && j1587::checksumOk(rts, n));
  }

  // ---- CTS detection in received bytes (with junk before it)
  {
    uint8_t cts[] = {0x00, 0xFF, 0xBD, 0xC5, 0x05, 0xBC, 0x02, 0x01, 0x01, 0x00, 0x00};
    cts[sizeof(cts) - 1] = j1587::checksum(cts + 2, sizeof(cts) - 3);
    uint8_t segs = 0, next = 0;
    CHECK(j1587::findCts(cts, sizeof(cts), &segs, &next));
    CHECK(segs == 1 && next == 1);
    cts[sizeof(cts) - 1] ^= 0xFF;  // corrupt checksum
    CHECK(!j1587::findCts(cts, sizeof(cts), &segs, &next));
    CHECK(!j1587::findCts(cts, 4, &segs, &next));  // too short
  }

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
