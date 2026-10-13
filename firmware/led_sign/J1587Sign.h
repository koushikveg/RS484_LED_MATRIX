// J1587Sign.h - packet builder for Adaptive transit signs that take text over
// SAE J1708 / J1587 (e.g. TDR80x7-.3A, PN 1105-2126, firmware 1105-6001a).
//
// These signs ignore plain Alpha packets on their J1708 wires. Instead the
// controller uses the J1587 transport protocol (PID 197 connection management
// and PID 198 connection-mode data) to carry an Alpha-style text command:
//
//   1. Controller -> sign: RTS  (PID 0xC5, request to send)
//   2. Sign -> controller: CTS  (PID 0xC5, clear to send, ~10 bytes)
//   3. Controller -> sign: data (PID 0xC6), one or more segments carrying
//        'A'  FileLabel  [ESC 0x20 Mode]  Text  EOT
//
// The byte layout is taken from a known-good Linux program that drives this
// exact sign at 9600 8N1 (see test/test_j1587.cpp for its exact bytes). Every
// J1708 message is: MID, PID, byte count, data..., checksum, and is limited
// to 21 bytes in total.
//
// Pure C++ with no Arduino dependencies so it can be unit tested on a PC.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace j1587 {

constexpr uint8_t MID_CONTROLLER = 0xBC;  // sender (MID 188)
constexpr uint8_t MID_SIGN = 0xBD;        // the sign (MID 189)
constexpr uint8_t PID_CONN_MGMT = 0xC5;   // PID 197: RTS / CTS / EOM / abort
constexpr uint8_t PID_CONN_DATA = 0xC6;   // PID 198: connection-mode data
constexpr uint8_t CM_RTS = 0x01;          // request to send
constexpr uint8_t CM_CTS = 0x02;          // clear to send (from the sign)

constexpr char CMD_WRITE_TEXT = 'A';
constexpr char FILE_LABEL = 'B';  // file used by the known-good program
constexpr uint8_t ESC = 0x1B;
constexpr uint8_t EOT = 0x04;
constexpr uint8_t SPEED_1 = 0x15;  // Alpha speed codes: 0x15 (slowest) .. 0x19 (fastest)

constexpr size_t MAX_FRAME = 21;  // J1708 limit, including MID and checksum
// Data bytes per PID 198 segment: 21 - MID - PID - count - dest - seg# - checksum
constexpr size_t MAX_SEGMENT_DATA = MAX_FRAME - 6;
constexpr size_t MAX_SEGMENTS = 16;
constexpr size_t MAX_PAYLOAD = MAX_SEGMENT_DATA * MAX_SEGMENTS;

// J1708 checksum: two's complement of the 8-bit sum of every byte before it.
inline uint8_t checksum(const uint8_t* data, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; ++i) sum += data[i];
  return (uint8_t)(0x100 - sum);
}

// True if a complete frame (including its checksum byte) sums to zero.
inline bool checksumOk(const uint8_t* frame, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; ++i) sum += frame[i];
  return len > 0 && sum == 0;
}

constexpr uint8_t CM_EOM = 0x03;  // end of message (sign's acknowledgement)

// Where the RTS carries the message size. The known-good single-segment RTS is
//   BC C5 07 BD 01 00 00 00 42 01 77
// and the sign's replies end in the same "42 01" (CTS: BD C5 06 BC 02 00 01
// 42 01 76, EOM: BD C5 04 BC 03 42 01 78), so "42 01" looks like a fixed ID
// rather than a size. Sending "42 06" for a 6-segment message got no CTS.
// The firmware tries these layouts for multi-segment messages and remembers
// the one the sign answers:
enum RtsLayout : uint8_t {
  RTS_FIXED = 0,       // 00 00 00 42 01 exactly (sign ignores the size)
  RTS_STANDARD = 1,    // segs lenLo lenHi 42 01 (SAE J1587 order)
  RTS_SEGS_FIRST = 2,  // segs 00 00 42 01
  RTS_TOTAL_ONLY = 3,  // 00 lenLo lenHi 42 01
  RTS_SEGS_LAST = 4,   // 00 00 00 42 segs (first guess; not answered)
  RTS_LAYOUT_COUNT = 5
};

inline const char* rtsLayoutName(uint8_t layout) {
  switch (layout) {
    case RTS_FIXED: return "fixed";
    case RTS_STANDARD: return "standard";
    case RTS_SEGS_FIRST: return "segs-first";
    case RTS_TOTAL_ONLY: return "total-only";
    case RTS_SEGS_LAST: return "segs-last";
    default: return "?";
  }
}

// Builds an RTS frame for a message of `segments` PID 198 segments carrying
// `totalBytes` of payload. RTS_FIXED reproduces the known-good packet byte for
// byte whatever the size. Returns the frame length (11).
inline size_t buildRts(uint8_t* out, uint8_t layout, size_t segments, size_t totalBytes) {
  uint8_t segs = (uint8_t)segments;
  uint8_t lo = (uint8_t)(totalBytes & 0xFF);
  uint8_t hi = (uint8_t)(totalBytes >> 8);
  uint8_t x0 = 0, x1 = 0, x2 = 0, last = 0x01;
  switch (layout) {
    case RTS_STANDARD: x0 = segs; x1 = lo; x2 = hi; break;
    case RTS_SEGS_FIRST: x0 = segs; break;
    case RTS_TOTAL_ONLY: x1 = lo; x2 = hi; break;
    case RTS_SEGS_LAST: last = segs; break;
    default: break;  // RTS_FIXED
  }
  size_t n = 0;
  out[n++] = MID_CONTROLLER;
  out[n++] = PID_CONN_MGMT;
  out[n++] = 0x07;  // data bytes that follow
  out[n++] = MID_SIGN;
  out[n++] = CM_RTS;
  out[n++] = x0;
  out[n++] = x1;
  out[n++] = x2;
  out[n++] = (uint8_t)FILE_LABEL;
  out[n++] = last;
  out[n] = checksum(out, n);
  return n + 1;
}

// One display page inside the payload. With a mode, each page starts its own
// Alpha mode field (ESC 0x20 mode [special]) and the sign shows the pages one
// after another, e.g. each scrolling right to left with mode 'a' (rotate).
struct Page {
  const char* text;
  char mode;      // Alpha mode code, e.g. 'a' = rotate; 0 = no mode field
  char special;   // special-mode specifier when mode == 'n', else 0
  uint8_t speed;  // 1 (slowest) .. 5 (fastest); 0 = sign default
};

// Builds the Alpha-style text payload:
//   'A' FileLabel { [ESC 0x20 mode [special]] [speed] text }... EOT
// Only printable ASCII (0x20-0x7E) from the page text is copied. Returns the
// payload length, or 0 if it doesn't fit in `cap`.
inline size_t buildPayloadPages(uint8_t* out, size_t cap, const Page* pages, size_t count) {
  size_t n = 0;
  auto put = [&](uint8_t b) {
    if (n < cap) out[n] = b;
    ++n;
  };
  put((uint8_t)CMD_WRITE_TEXT);
  put((uint8_t)FILE_LABEL);
  for (size_t i = 0; i < count; ++i) {
    if (pages[i].mode) {
      put(ESC);
      put(0x20);  // display position (ignored on one-line signs)
      put((uint8_t)pages[i].mode);
      if (pages[i].special) put((uint8_t)pages[i].special);
    }
    if (pages[i].speed >= 1 && pages[i].speed <= 5) put((uint8_t)(SPEED_1 + pages[i].speed - 1));
    for (const char* p = pages[i].text; p && *p; ++p) {
      uint8_t c = (uint8_t)*p;
      if (c >= 0x20 && c <= 0x7E) put(c);
    }
  }
  put(EOT);
  return n <= cap ? n : 0;
}

// Single-page payload: 'A' FileLabel [ESC 0x20 mode] text EOT.
inline size_t buildPayload(uint8_t* out, size_t cap, const char* text, char mode) {
  Page page = {text, mode, 0, 0};
  return buildPayloadPages(out, cap, &page, 1);
}

inline size_t segmentsFor(size_t payloadLen) {
  return (payloadLen + MAX_SEGMENT_DATA - 1) / MAX_SEGMENT_DATA;
}

// Builds data segment `index` (0-based) of `payload` as a PID 198 frame:
//   BC C6 count BD segNo data... checksum
// Segments are numbered from 1 on the wire. Returns the frame length, or 0 if
// `index` is past the end of the payload.
inline size_t buildSegment(uint8_t* out, const uint8_t* payload, size_t payloadLen, size_t index) {
  size_t start = index * MAX_SEGMENT_DATA;
  if (start >= payloadLen) return 0;
  size_t chunk = payloadLen - start;
  if (chunk > MAX_SEGMENT_DATA) chunk = MAX_SEGMENT_DATA;

  size_t n = 0;
  out[n++] = MID_CONTROLLER;
  out[n++] = PID_CONN_DATA;
  out[n++] = (uint8_t)(chunk + 2);  // dest MID + segment number + data
  out[n++] = MID_SIGN;
  out[n++] = (uint8_t)(index + 1);
  for (size_t i = 0; i < chunk; ++i) out[n++] = payload[start + i];
  out[n] = checksum(out, n);
  return n + 1;
}

// Finds a valid PID 197 frame from the sign addressed to us with the given
// connection-management code (CM_CTS, CM_EOM, ...). On success copies the
// data bytes after the code into `data` (up to `cap`) and sets `dataLen`.
inline bool findConnMgmt(const uint8_t* rx, size_t len, uint8_t code, uint8_t* data = nullptr,
                         size_t cap = 0, size_t* dataLen = nullptr) {
  for (size_t i = 0; i + 5 <= len; ++i) {
    if (rx[i] != MID_SIGN || rx[i + 1] != PID_CONN_MGMT) continue;
    size_t count = rx[i + 2];
    if (count < 2 || i + 3 + count + 1 > len) continue;  // too short / incomplete
    if (!checksumOk(rx + i, 3 + count + 1)) continue;
    if (rx[i + 3] != MID_CONTROLLER || rx[i + 4] != code) continue;
    size_t extra = count - 2;
    if (data) {
      for (size_t k = 0; k < extra && k < cap; ++k) data[k] = rx[i + 5 + k];
    }
    if (dataLen) *dataLen = extra;
    return true;
  }
  return false;
}

inline bool findCts(const uint8_t* rx, size_t len) { return findConnMgmt(rx, len, CM_CTS); }
inline bool findEom(const uint8_t* rx, size_t len) { return findConnMgmt(rx, len, CM_EOM); }

}  // namespace j1587
