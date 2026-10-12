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
  int code = -1;          // WMO weather code for today
  double rainInches = 0;  // today's rain + showers total
  double snowInches = 0;  // today's snowfall total
  int rainChance = -1;    // %, highest hourly chance among rain hours, -1 = none
  int snowChance = -1;    // %, highest hourly chance among snow hours, -1 = none
};

// Precipitation is only shown when its chance is above this (percent).
constexpr int MIN_PRECIP_CHANCE = 5;

constexpr size_t MAX_HOURS = 25;  // forecast_days=1 returns 24 hourly values

struct Place {
  char name[48] = "";
  double latitude = NAN;
  double longitude = NAN;
};

// ---------------------------------------------------------------- URLs

// Precipitation amounts (rain and snowfall) are always requested in inches.
// Hourly data covers today (local midnight to midnight) and is used to work
// out separate rain and snow chances; Open-Meteo only gives one combined
// probability per hour.
// Returns false if `cap` is too small (the URL is up to ~350 bytes).
inline bool forecastUrl(char* out, size_t cap, double lat, double lon, bool fahrenheit) {
  int len = snprintf(out, cap,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m"
           "&hourly=temperature_2m,precipitation_probability,rain,showers,snowfall"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,rain_sum,showers_sum,snowfall_sum"
           "&timezone=auto&forecast_days=1&precipitation_unit=inch%s",
           lat, lon, fahrenheit ? "&temperature_unit=fahrenheit" : "");
  return len > 0 && (size_t)len < cap;
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

// Reads the numeric array under `key` into `out` (JSON null becomes NAN).
// Returns the number of elements read, at most `max`.
inline size_t numbers(const char* begin, const char* end, const char* key, double* out, size_t max) {
  const char* v = findKey(begin, end, key);
  if (!v || *v != '[') return 0;
  size_t n = 0;
  const char* p = v + 1;
  while (p < end && n < max) {
    while (p < end && (*p == ' ' || *p == ',')) ++p;
    if (p >= end || *p == ']') break;
    if (strncmp(p, "null", 4) == 0) {
      out[n++] = NAN;
      p += 4;
      continue;
    }
    char* stop = nullptr;
    double d = strtod(p, &stop);
    if (stop == p || stop > end) break;
    out[n++] = d;
    p = stop;
  }
  return n;
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

// Parses an Open-Meteo forecast response (see forecastUrl). `fahrenheit`
// must match the request so the freezing point is right. Returns false
// unless at least today's high and low were found.
inline bool parseForecast(const char* json, bool fahrenheit, Forecast& f) {
  f = Forecast();
  const char* end = json + strlen(json);

  const char* cb = json;
  const char* ce = end;
  double v;
  if (section(cb, ce, "current") && number(cb, ce, "temperature_2m", v)) f.tempNow = v;

  const char* db = json;
  const char* de = end;
  if (!section(db, de, "daily")) return false;
  if (number(db, de, "weather_code", v)) f.code = (int)v;
  if (number(db, de, "temperature_2m_max", v)) f.tempHigh = v;
  if (number(db, de, "temperature_2m_min", v)) f.tempLow = v;
  if (number(db, de, "rain_sum", v) && !isnan(v)) f.rainInches += v;
  if (number(db, de, "showers_sum", v) && !isnan(v)) f.rainInches += v;
  if (number(db, de, "snowfall_sum", v) && !isnan(v)) f.snowInches = v;

  // Split each hour's precipitation chance into rain or snow. The hour's
  // forecast amounts decide; if neither is forecast, the temperature does.
  const char* hb = json;
  const char* he = end;
  if (section(hb, he, "hourly")) {
    double temp[MAX_HOURS], chance[MAX_HOURS], rain[MAX_HOURS], showers[MAX_HOURS], snow[MAX_HOURS];
    size_t n = numbers(hb, he, "precipitation_probability", chance, MAX_HOURS);
    size_t nt = numbers(hb, he, "temperature_2m", temp, MAX_HOURS);
    size_t nr = numbers(hb, he, "rain", rain, MAX_HOURS);
    size_t ns = numbers(hb, he, "showers", showers, MAX_HOURS);
    size_t nf = numbers(hb, he, "snowfall", snow, MAX_HOURS);
    double freezing = fahrenheit ? 32.0 : 0.0;
    for (size_t i = 0; i < n; ++i) {
      if (isnan(chance[i])) continue;
      double r = (i < nr && !isnan(rain[i]) ? rain[i] : 0) + (i < ns && !isnan(showers[i]) ? showers[i] : 0);
      double sn = i < nf && !isnan(snow[i]) ? snow[i] : 0;
      bool isSnow = sn > 0;
      bool isRain = r > 0;
      if (!isSnow && !isRain && i < nt && !isnan(temp[i])) {
        isSnow = temp[i] <= freezing;
        isRain = !isSnow;
      }
      int c = (int)lround(chance[i]);
      if (isRain && c > f.rainChance) f.rainChance = c;
      if (isSnow && c > f.snowChance) f.snowChance = c;
    }
  }
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
constexpr size_t PAGE_LEN = 48;

// Formats an amount in inches, showing "<0.01" / "<0.1" for traces.
inline void formatInches(char* out, size_t cap, double inches, int decimals) {
  double step = decimals == 1 ? 0.1 : 0.01;
  if (inches < step / 2) snprintf(out, cap, "<%.*f", decimals, step);
  else snprintf(out, cap, "%.*f", decimals, inches);
}

// Turns a forecast into the screens shown on the sign, in order:
//   AMES  ->  PARTLY CLOUDY  ->  NOW 62F  HIGH 66F  LOW 51F
//   ->  RAIN 0.25 IN  40% CHANCE  ->  SNOW 1.2 IN  30% CHANCE
// Rain and snow screens only appear when their chance is above
// MIN_PRECIP_CHANCE. The sign has no degree symbol, so temperatures read like
// "66F". Returns the number of screens written.
inline size_t formatPages(const Forecast& f, const char* label, bool fahrenheit,
                          char pages[][PAGE_LEN], size_t maxPages) {
  size_t n = 0;
  char unit = fahrenheit ? 'F' : 'C';

  if (label && *label && n < maxPages) snprintf(pages[n++], PAGE_LEN, "%s", label);
  if (*describe(f.code) && n < maxPages) snprintf(pages[n++], PAGE_LEN, "%s", describe(f.code));

  // Current, high and low together on one screen.
  char temps[PAGE_LEN] = "";
  size_t len = 0;
  if (!isnan(f.tempNow))
    len += snprintf(temps + len, sizeof(temps) - len, "NOW %ld%c  ", lround(f.tempNow), unit);
  if (!isnan(f.tempHigh) && len < sizeof(temps))
    len += snprintf(temps + len, sizeof(temps) - len, "HIGH %ld%c  ", lround(f.tempHigh), unit);
  if (!isnan(f.tempLow) && len < sizeof(temps))
    len += snprintf(temps + len, sizeof(temps) - len, "LOW %ld%c", lround(f.tempLow), unit);
  while (len > 0 && len <= sizeof(temps) && temps[len - 1] == ' ') temps[--len] = '\0';
  if (len && n < maxPages) snprintf(pages[n++], PAGE_LEN, "%s", temps);

  char amount[16];
  if (f.rainChance > MIN_PRECIP_CHANCE && n < maxPages) {
    formatInches(amount, sizeof(amount), f.rainInches, 2);
    snprintf(pages[n++], PAGE_LEN, "RAIN %s IN  %d%% CHANCE", amount, f.rainChance);
  }
  if (f.snowChance > MIN_PRECIP_CHANCE && n < maxPages) {
    formatInches(amount, sizeof(amount), f.snowInches, 1);
    snprintf(pages[n++], PAGE_LEN, "SNOW %s IN  %d%% CHANCE", amount, f.snowChance);
  }
  return n;
}

}  // namespace weather
