// AlphaSign.h - packet encoder for the Adaptive Micro Systems "Alpha" sign protocol.
//
// Reference: Alpha Sign Communications Protocol, PN 9708-8061F (docs/ in this repo).
// Pure C++ with no Arduino dependencies so it can be unit tested on a PC
// (see test/test_alpha.cpp).
//
// Standard ("1-byte") transmission packet, section 5.1:
//
//   NUL x5  SOH  TypeCode  Address(2)  STX  CommandCode  DataField  [ETX Checksum(4)]  EOT
//
// Write TEXT file data field, section 6.1.1:
//
//   'A'  FileLabel  [ESC  DisplayPosition  ModeCode  [SpecialSpecifier]]  Message

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace alpha {

// Control characters (Appendix G)
constexpr uint8_t NUL = 0x00;
constexpr uint8_t SOH = 0x01;
constexpr uint8_t STX = 0x02;
constexpr uint8_t ETX = 0x03;
constexpr uint8_t EOT = 0x04;
constexpr uint8_t ESC = 0x1B;
constexpr uint8_t SELECT_COLOR = 0x1C;
constexpr uint8_t SPEED_1 = 0x15;  // 0x15 (slowest) .. 0x19 (fastest)

// Type codes (Table 5)
constexpr char TYPE_ALL = 'Z';
constexpr char TYPE_ONE_LINE = '1';

// Command codes (Table 6)
constexpr char CMD_WRITE_TEXT = 'A';

// File labels (section 6.1 / 6.1.3)
constexpr char FILE_DEFAULT = 'A';   // exists on power-up, no memory config needed
constexpr char FILE_PRIORITY = '0';  // 125-byte file that overrides all others

// Display position (Table 12). Irrelevant on one-line signs but must be sent.
constexpr char POS_MIDDLE = 0x20;

constexpr size_t SYNC_NULS = 5;
constexpr size_t PRIORITY_MAX_BYTES = 125;

struct Mode {
  const char* name;
  char code;     // Standard Mode code (Table 65)
  char special;  // Special Mode specifier when code == 'n' (Table 66), else 0
};

// Modes from Appendix I. Not every sign supports every mode; unsupported
// ones are usually shown as ROTATE or HOLD.
static const Mode MODES[] = {
    {"rotate", 'a', 0},    {"hold", 'b', 0},       {"flash", 'c', 0},
    {"rollup", 'e', 0},    {"rolldown", 'f', 0},   {"rollleft", 'g', 0},
    {"rollright", 'h', 0}, {"wipeup", 'i', 0},     {"wipedown", 'j', 0},
    {"wipeleft", 'k', 0},  {"wiperight", 'l', 0},  {"scroll", 'm', 0},
    {"auto", 'o', 0},      {"rollin", 'p', 0},     {"rollout", 'q', 0},
    {"wipein", 'r', 0},    {"wipeout", 's', 0},    {"compressed", 't', 0},
    {"twinkle", 'n', '0'}, {"sparkle", 'n', '1'},  {"snow", 'n', '2'},
    {"interlock", 'n', '3'}, {"switch", 'n', '4'}, {"slide", 'n', '5'},
    {"spray", 'n', '6'},   {"starburst", 'n', '7'},
};
constexpr size_t MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);

struct Color {
  const char* name;
  char code;  // sent as SELECT_COLOR + code
};

// Colours (Appendix G, 1CH). A red-only sign ignores these.
static const Color COLORS[] = {
    {"red", '1'},      {"green", '2'},    {"amber", '3'},    {"dimred", '4'},
    {"dimgreen", '5'}, {"brown", '6'},    {"orange", '7'},   {"yellow", '8'},
    {"rainbow1", '9'}, {"rainbow2", 'A'}, {"mix", 'B'},      {"auto", 'C'},
};
constexpr size_t COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);

inline bool nameEquals(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return false;
  }
  return *a == *b;
}

// Returns an index into MODES, or -1 if not found.
inline int findMode(const char* name) {
  for (size_t i = 0; i < MODE_COUNT; ++i)
    if (nameEquals(MODES[i].name, name)) return (int)i;
  return -1;
}

// Returns an index into COLORS, or -1 if not found.
inline int findColor(const char* name) {
  for (size_t i = 0; i < COLOR_COUNT; ++i)
    if (nameEquals(COLORS[i].name, name)) return (int)i;
  return -1;
}

struct TextOptions {
  char typeCode = TYPE_ALL;
  char address[3] = "00";     // "00" = broadcast
  char fileLabel = FILE_DEFAULT;
  int mode = 0;               // index into MODES, -1 = omit the mode field
  int color = -1;             // index into COLORS, -1 = sign default
  uint8_t speed = 0;          // 1 (slow) .. 5 (fast), 0 = sign default
  bool checksum = false;      // append ETX + 4-digit checksum
};

// Bounded byte writer; sets `overflow` instead of writing past the end.
class Writer {
 public:
  Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}
  void put(uint8_t b) {
    if (len_ < cap_) buf_[len_++] = b;
    else overflow = true;
  }
  size_t len() const { return len_; }
  const uint8_t* data() const { return buf_; }
  bool overflow = false;

 private:
  uint8_t* buf_;
  size_t cap_;
  size_t len_ = 0;
};

inline char hexDigit(uint8_t v) { return v < 10 ? '0' + v : 'A' + (v - 10); }

// Builds a complete Write TEXT file packet into `out`.
// Only printable ASCII (0x20-0x7E) from `text` is copied so user input can't
// inject protocol control codes. Returns the packet length, or 0 if `cap`
// was too small.
inline size_t buildWriteText(uint8_t* out, size_t cap, const TextOptions& o,
                             const char* text) {
  Writer w(out, cap);
  for (size_t i = 0; i < SYNC_NULS; ++i) w.put(NUL);
  w.put(SOH);
  w.put(o.typeCode);
  w.put(o.address[0]);
  w.put(o.address[1]);

  size_t stx = w.len();
  w.put(STX);
  w.put(CMD_WRITE_TEXT);
  w.put(o.fileLabel);

  if (o.mode >= 0 && (size_t)o.mode < MODE_COUNT) {
    w.put(ESC);
    w.put(POS_MIDDLE);
    w.put(MODES[o.mode].code);
    if (MODES[o.mode].special) w.put(MODES[o.mode].special);
  }
  if (o.speed >= 1 && o.speed <= 5) w.put(SPEED_1 + (o.speed - 1));
  if (o.color >= 0 && (size_t)o.color < COLOR_COUNT) {
    w.put(SELECT_COLOR);
    w.put(COLORS[o.color].code);
  }

  // The priority file holds at most 125 bytes of TEXT file data.
  size_t limit = (o.fileLabel == FILE_PRIORITY)
                     ? PRIORITY_MAX_BYTES - (w.len() - stx - 3)
                     : (size_t)-1;
  size_t written = 0;
  for (const char* p = text; p && *p && written < limit; ++p) {
    uint8_t c = (uint8_t)*p;
    if (c >= 0x20 && c <= 0x7E) {
      w.put(c);
      ++written;
    }
  }

  if (o.checksum) {
    w.put(ETX);
    // 16-bit sum of everything from STX through ETX inclusive (section 5.1.1)
    uint16_t sum = 0;
    for (size_t i = stx; i < w.len(); ++i) sum += w.data()[i];
    w.put(hexDigit((sum >> 12) & 0xF));
    w.put(hexDigit((sum >> 8) & 0xF));
    w.put(hexDigit((sum >> 4) & 0xF));
    w.put(hexDigit(sum & 0xF));
  }
  w.put(EOT);
  return w.overflow ? 0 : w.len();
}

}  // namespace alpha
