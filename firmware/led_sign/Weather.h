// Weather.h - Open-Meteo request URLs, response parsing and sign page
// formatting. Pure C++ with no Arduino dependencies so it can be unit tested
// on a PC (see test/test_weather.cpp).
//
// Open-Meteo is free, needs no API key, and returns small, flat JSON, so a
// few targeted lookups are enough and we avoid pulling in a JSON library.
//   Forecast:  https://open-meteo.com/en/docs
//   Geocoding: https://open-meteo.com/en/docs/geocoding-api

#pragma once

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace weather {

// ---------------------------------------------------------------- data

struct Forecast {
  double tempNow = NAN;
  double tempHigh = NAN;
  double tempLow = NAN;
  int humidity = -1;     // %, current relative humidity
  int precipChance = -1; // %, today's max precipitation probability
  int code = -1;         // WMO weather code for today
};

struct Place {
  char name[48] = "";
  double latitude = NAN;
  double longitude = NAN;
};

// ---------------------------------------------------------------- URLs

inline void forecastUrl(char* out, size_t cap, double lat, double lon, bool fahrenheit) {
  snprintf(out, cap,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&timezone=auto&forecast_days=1%s",
           lat, lon, fahrenheit ? "&temperature_unit=fahrenheit" : "");
}

// Percent-encodes `name` for the geocoding search. Open-Meteo matches on the
// place name only, so "Milwaukee" works but "Milwaukee, WI" does not.
inline void geocodeUrl(char* out, size_t cap, const char* name) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  size_t n = (size_t)snprintf(out, cap,
                              "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=");
  for (const unsigned char* p = (const unsigned char*)name; *p && n + 4 < cap; ++p) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.') {
      out[n++] = *p;
    } else {
      out[n++] = '%';
      out[n++] = HEX_DIGITS[*p >> 4];
      out[n++] = HEX_DIGITS[*p & 0xF];
    }
  }
  if (n < cap) out[n] = '\0';
}

// ---------------------------------------------------------------- JSON helpers

// Finds the value that follows `"key":` between [begin, end). Matching the
// closing quote and colon keeps "temperature_2m" from matching
// "temperature_2m_max". Returns a pointer to the value, or nullptr.
inline const char* findKey(const char* begin, const char* end, const char* key) {
  size_t klen = strlen(key);
  for (const char* p = begin; p && p + klen + 3 <= end; ++p) {
    if (p[0] == '"' && strncmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
      const char* v = p + klen + 2;
      while (v < end && (*v == ' ' || *v == ':')) ++v;
      return v < end ? v : nullptr;
    }
  }
  return nullptr;
}

// Returns the end of the {...} or [...] that starts at `open` (just past the
// closing bracket), skipping over strings. nullptr if unbalanced.
inline const char* matchBracket(const char* open, const char* end) {
  char o = *open, c = (o == '{') ? '}' : ']';
  int depth = 0;
  bool inString = false;
  for (const char* p = open; p < end; ++p) {
    if (inString) {
      if (*p == '\\') ++p;
      else if (*p == '"') inString = false;
    } else if (*p == '"') {
      inString = true;
    } else if (*p == o) {
      ++depth;
    } else if (*p == c && --depth == 0) {
      return p + 1;
    }
  }
  return nullptr;
}

// Narrows [begin, end) to the object or array stored under `key`.
inline bool section(const char*& begin, const char*& end, const char* key) {
  const char* v = findKey(begin, end, key);
  if (!v || (*v != '{' && *v != '[')) return false;
  const char* close = matchBracket(v, end);
  if (!close) return false;
  begin = v;
  end = close;
  return true;
}

// Reads a number under `key`. If the value is an array, reads its first
// element. Returns false for missing keys and JSON null.
inline bool number(const char* begin, const char* end, const char* key, double& out) {
  const char* v = findKey(begin, end, key);
  if (!v) return false;
  if (*v == '[') ++v;
  char* stop = nullptr;
  double d = strtod(v, &stop);
  if (stop == v || stop > end) return false;
  out = d;
  return true;
}

// Reads a string under `key` into `out`, upper-cased and reduced to ASCII
// (the sign can't show other characters). Returns false if missing.
inline bool text(const char* begin, const char* end, const char* key, char* out, size_t cap) {
  const char* v = findKey(begin, end, key);
  if (!v || *v != '"' || cap == 0) return false;
  size_t n = 0;
  for (const char* p = v + 1; p < end && *p != '"'; ++p) {
    char c = *p;
    if (c == '\\' && p + 1 < end) {
      ++p;
      if (*p == 'u') {  // \uXXXX: not displayable, skip it
        p += 4;
        continue;
      }
      c = *p;
    }
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) continue;
    if (n + 1 < cap) out[n++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
  }
  out[n] = '\0';
  return true;
}

// ---------------------------------------------------------------- parsers

// Parses an Open-Meteo forecast response. Returns false unless at least
// today's high and low were found.
inline bool parseForecast(const char* json, Forecast& f) {
  f = Forecast();
  const char* end = json + strlen(json);

  const char* cb = json;
  const char* ce = end;
  if (section(cb, ce, "current")) {
    double v;
    if (number(cb, ce, "temperature_2m", v)) f.tempNow = v;
    if (number(cb, ce, "relative_humidity_2m", v)) f.humidity = (int)lround(v);
  }

  const char* db = json;
  const char* de = end;
  if (!section(db, de, "daily")) return false;
  double v;
  if (number(db, de, "weather_code", v)) f.code = (int)v;
  if (number(db, de, "temperature_2m_max", v)) f.tempHigh = v;
  if (number(db, de, "temperature_2m_min", v)) f.tempLow = v;
  if (number(db, de, "precipitation_probability_max", v)) f.precipChance = (int)lround(v);
  return !isnan(f.tempHigh) && !isnan(f.tempLow);
}

// Parses the first result of an Open-Meteo geocoding response.
inline bool parsePlace(const char* json, Place& place) {
  place = Place();
  const char* b = json;
  const char* e = json + strlen(json);
  if (!section(b, e, "results")) return false;
  const char* obj = strchr(b, '{');
  if (!obj || obj >= e) return false;
  const char* objEnd = matchBracket(obj, e);
  if (!objEnd) return false;

  if (!number(obj, objEnd, "latitude", place.latitude)) return false;
  if (!number(obj, objEnd, "longitude", place.longitude)) return false;
  text(obj, objEnd, "name", place.name, sizeof(place.name));
  return true;
}

// ---------------------------------------------------------------- formatting

// Short descriptions of WMO weather codes, sized for an 80x7 sign
// (about 13 characters fit without scrolling).
inline const char* describe(int code) {
  switch (code) {
    case 0: return "CLEAR";
    case 1: return "MOSTLY CLEAR";
    case 2: return "PARTLY CLOUDY";
    case 3: return "OVERCAST";
    case 45: case 48: return "FOG";
    case 51: case 53: case 55: return "DRIZZLE";
    case 56: case 57: return "FREEZING DRIZZLE";
    case 61: return "LIGHT RAIN";
    case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66: case 67: return "FREEZING RAIN";
    case 71: return "LIGHT SNOW";
    case 73: return "SNOW";
    case 75: return "HEAVY SNOW";
    case 77: return "SNOW GRAINS";
    case 80: case 81: return "SHOWERS";
    case 82: return "HEAVY SHOWERS";
    case 85: case 86: return "SNOW SHOWERS";
    case 95: return "THUNDERSTORMS";
    case 96: case 99: return "T-STORMS + HAIL";
    default: return "";
  }
}

constexpr size_t MAX_PAGES = 8;
constexpr size_t PAGE_LEN = 40;

// Turns a forecast into the lines shown on the sign, in order. The sign has
// no degree symbol, so temperatures read like "HIGH 74F". Returns the number
// of pages written.
inline size_t formatPages(const Forecast& f, const char* label, bool fahrenheit,
                          char pages[][PAGE_LEN], size_t maxPages) {
  size_t n = 0;
  char unit = fahrenheit ? 'F' : 'C';
#define WEATHER_PAGE(...) \
  if (n < maxPages) snprintf(pages[n++], PAGE_LEN, __VA_ARGS__)

  if (label && *label) WEATHER_PAGE("%s", label);
  if (*describe(f.code)) WEATHER_PAGE("%s", describe(f.code));
  if (!isnan(f.tempNow)) WEATHER_PAGE("NOW %ld%c", lround(f.tempNow), unit);
  if (!isnan(f.tempHigh)) WEATHER_PAGE("HIGH %ld%c", lround(f.tempHigh), unit);
  if (!isnan(f.tempLow)) WEATHER_PAGE("LOW %ld%c", lround(f.tempLow), unit);
  if (f.humidity >= 0) WEATHER_PAGE("HUMIDITY %d%%", f.humidity);
  if (f.precipChance >= 0) WEATHER_PAGE("RAIN %d%%", f.precipChance);
#undef WEATHER_PAGE
  return n;
}

}  // namespace weather
