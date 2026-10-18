// JsonStream.h - a tiny streaming JSON reader.
//
// Transitland responses can be tens of kilobytes, more than is comfortable to
// hold in the ESP32's RAM at once. This parser takes the response a few bytes
// at a time and reports each value with its key path, so the caller keeps
// only the fields it needs. Paths join object keys with '.', and array levels
// add nothing: in {"stops":[{"stop_name":"X"}]} the value X has the path
// "stops.stop_name", and each element object of "stops" starts and ends at
// path "stops".
//
// Pure C++ with no Arduino dependencies so it can be unit tested on a PC.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace jsonstream {

constexpr size_t MAX_DEPTH = 24;
constexpr size_t PATH_LEN = 160;
constexpr size_t VALUE_LEN = 96;  // longer strings are cut short

struct Handler {
  virtual ~Handler() {}
  virtual void onStart(const char* path, bool object) { (void)path; (void)object; }
  virtual void onEnd(const char* path, bool object) { (void)path; (void)object; }
  // Strings are UTF-8 with escapes decoded; numbers, true, false and null
  // arrive as their literal text with isString false.
  virtual void onValue(const char* path, const char* value, bool isString) {
    (void)path; (void)value; (void)isString;
  }
};

class Parser {
 public:
  explicit Parser(Handler& h) : h_(h) { path_[0] = '\0'; }

  void feed(const char* data, size_t len) {
    for (size_t i = 0; i < len && state_ != FAILED; ++i) step(data[i]);
  }
  void feed(const char* text) { feed(text, strlen(text)); }

  bool failed() const { return state_ == FAILED; }
  // True once a complete top-level value has been read.
  bool done() const { return state_ == DONE; }

 private:
  enum State : uint8_t {
    VALUE,         // expecting a value
    VALUE_OR_END,  // just after '['
    KEY_OR_END,    // just after '{'
    KEY,           // after ',' in an object
    COLON,
    AFTER_VALUE,
    STRING,
    ESCAPE,
    UNICODE,
    LITERAL,
    DONE,
    FAILED
  };

  struct Level {
    bool object;
    uint8_t baseLen;  // path length when the container opened
  };

  Handler& h_;
  State state_ = VALUE;
  Level stack_[MAX_DEPTH];
  size_t depth_ = 0;
  char path_[PATH_LEN];
  size_t pathLen_ = 0;
  char buf_[VALUE_LEN];
  size_t bufLen_ = 0;
  bool inKey_ = false;
  uint16_t hex_ = 0;
  uint8_t hexDigits_ = 0;

  static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
  static bool isLiteralChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
           c == '+' || c == '-';
  }

  void put(char c) {
    if (bufLen_ + 1 < sizeof(buf_)) buf_[bufLen_++] = c;
  }

  void putCodepoint(uint16_t cp) {
    if (cp < 0x80) {
      put((char)cp);
    } else if (cp < 0x800) {
      put((char)(0xC0 | (cp >> 6)));
      put((char)(0x80 | (cp & 0x3F)));
    } else {
      put((char)(0xE0 | (cp >> 12)));
      put((char)(0x80 | ((cp >> 6) & 0x3F)));
      put((char)(0x80 | (cp & 0x3F)));
    }
  }

  void truncatePath(size_t len) {
    pathLen_ = len < PATH_LEN ? len : PATH_LEN - 1;
    path_[pathLen_] = '\0';
  }

  void setKey() {
    truncatePath(stack_[depth_ - 1].baseLen);
    size_t need = (pathLen_ ? 1 : 0) + bufLen_;
    if (pathLen_ + need < PATH_LEN) {
      if (pathLen_) path_[pathLen_++] = '.';
      memcpy(path_ + pathLen_, buf_, bufLen_);
      pathLen_ += bufLen_;
    } else {
      // Too deep to name: give it a path nothing will match.
      truncatePath(stack_[depth_ - 1].baseLen);
      if (pathLen_ + 2 < PATH_LEN) {
        path_[pathLen_++] = '.';
        path_[pathLen_++] = '?';
      }
    }
    path_[pathLen_] = '\0';
  }

  void afterValue() { state_ = depth_ ? AFTER_VALUE : DONE; }

  void open(bool object) {
    if (depth_ >= MAX_DEPTH || pathLen_ > 255) {
      state_ = FAILED;
      return;
    }
    h_.onStart(path_, object);
    stack_[depth_++] = {object, (uint8_t)pathLen_};
    state_ = object ? KEY_OR_END : VALUE_OR_END;
  }

  void close(bool object) {
    if (!depth_ || stack_[depth_ - 1].object != object) {
      state_ = FAILED;
      return;
    }
    truncatePath(stack_[depth_ - 1].baseLen);
    --depth_;
    h_.onEnd(path_, object);
    afterValue();
  }

  void finishString() {
    buf_[bufLen_] = '\0';
    if (inKey_) {
      setKey();
      state_ = COLON;
    } else {
      h_.onValue(path_, buf_, true);
      afterValue();
    }
  }

  void startValue(char c) {
    if (c == '{') {
      open(true);
    } else if (c == '[') {
      open(false);
    } else if (c == '"') {
      bufLen_ = 0;
      inKey_ = false;
      state_ = STRING;
    } else if (isLiteralChar(c)) {
      bufLen_ = 0;
      put(c);
      state_ = LITERAL;
    } else {
      state_ = FAILED;
    }
  }

  void step(char c) {
    switch (state_) {
      case VALUE:
        if (!isSpace(c)) startValue(c);
        break;
      case VALUE_OR_END:
        if (isSpace(c)) break;
        if (c == ']') close(false);
        else startValue(c);
        break;
      case KEY_OR_END:
      case KEY:
        if (isSpace(c)) break;
        if (c == '}' && state_ == KEY_OR_END) {
          close(true);
        } else if (c == '"') {
          bufLen_ = 0;
          inKey_ = true;
          state_ = STRING;
        } else {
          state_ = FAILED;
        }
        break;
      case COLON:
        if (isSpace(c)) break;
        state_ = c == ':' ? VALUE : FAILED;
        break;
      case AFTER_VALUE:
        if (isSpace(c)) break;
        if (c == ',') state_ = stack_[depth_ - 1].object ? KEY : VALUE;
        else if (c == '}') close(true);
        else if (c == ']') close(false);
        else state_ = FAILED;
        break;
      case STRING:
        if (c == '\\') state_ = ESCAPE;
        else if (c == '"') finishString();
        else put(c);
        break;
      case ESCAPE:
        state_ = STRING;
        switch (c) {
          case 'u': hex_ = 0; hexDigits_ = 0; state_ = UNICODE; break;
          case 'n': case 'r': case 't': case 'b': case 'f': put(' '); break;
          default: put(c);  // \" \\ \/
        }
        break;
      case UNICODE: {
        uint8_t v;
        if (c >= '0' && c <= '9') v = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
        else { state_ = FAILED; break; }
        hex_ = (uint16_t)(hex_ << 4 | v);
        if (++hexDigits_ == 4) {
          // Lone surrogate halves (emoji etc.) can't be shown on the sign.
          if (hex_ < 0xD800 || hex_ > 0xDFFF) putCodepoint(hex_);
          state_ = STRING;
        }
        break;
      }
      case LITERAL:
        if (isLiteralChar(c)) {
          put(c);
        } else {
          buf_[bufLen_] = '\0';
          h_.onValue(path_, buf_, false);
          afterValue();
          step(c);  // the character that ended the literal
        }
        break;
      case DONE:
        if (!isSpace(c)) state_ = FAILED;
        break;
      case FAILED:
        break;
    }
  }
};

}  // namespace jsonstream
