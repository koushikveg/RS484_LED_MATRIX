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
  int code = -1;           // WMO weather code for today (most significant weather)
  double rainInches = 0;   // today's rain + showers total
  double snowInches = 0;   // today's snowfall total
  int precipChance = -1;   // %, today's highest chance of any precipitation, -1 = unknown
  int cloudCover = -1;     // %, today's average cloud cover, -1 = unknown
};

// The precipitation screen only appears when the chance is above this (percent).
constexpr int MIN_PRECIP_CHANCE = 5;

struct Place {
  char name[48] = "";
  double latitude = NAN;
  double longitude = NAN;
};

// ---------------------------------------------------------------- URLs

// Precipitation amounts (rain and snowfall) are always requested in inches.
// Returns false if `cap` is too small.
inline bool forecastUrl(char* out, size_t cap, double lat, double lon, bool fahrenheit) {
  int len = snprintf(out, cap,
                     "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                     "&current=temperature_2m"
                     "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
                     "rain_sum,showers_sum,snowfall_sum,precipitation_probability_max"
                     "&hourly=cloud_cover"
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

// Averages the numeric array under `key`, skipping JSON nulls. Returns false
// if there are no values.
inline bool average(const char* begin, const char* end, const char* key, double& out) {
  const char* v = findKey(begin, end, key);
  if (!v || *v != '[') return false;
  const char* close = matchBracket(v, end);
  if (!close) return false;
  double sum = 0;
  int count = 0;
  for (const char* p = v + 1; p < close;) {
    while (p < close && (*p == ' ' || *p == ',')) ++p;
    if (p >= close || *p == ']') break;
    if (strncmp(p, "null", 4) == 0) {
      p += 4;
      continue;
    }
    char* stop = nullptr;
    double d = strtod(p, &stop);
    if (stop == p) return false;
    sum += d;
    ++count;
    p = stop;
  }
  if (count == 0) return false;
  out = sum / count;
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

// Parses an Open-Meteo forecast response (see forecastUrl). Returns false
// unless at least today's high and low were found.
inline bool parseForecast(const char* json, Forecast& f) {
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
  if (number(db, de, "precipitation_probability_max", v)) f.precipChance = (int)lround(v);

  // Hourly cloud cover for today (local midnight to midnight), averaged.
  const char* hb = json;
  const char* he = end;
  if (section(hb, he, "hourly") && average(hb, he, "cloud_cover", v)) f.cloudCover = (int)lround(v);
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
constexpr size_t PAGE_LEN = 64;

// Formats an amount in inches, showing "<0.01" / "<0.1" for traces.
inline void formatInches(char* out, size_t cap, double inches, int decimals) {
  double step = decimals == 1 ? 0.1 : 0.01;
  if (inches < step / 2) snprintf(out, cap, "<%.*f", decimals, step);
  else snprintf(out, cap, "%.*f", decimals, inches);
}

// What kind of precipitation today's weather code describes.
enum class CodeType { NONE, RAIN, FREEZING_RAIN, SNOW };

inline CodeType codeType(int code) {
  switch (code) {
    case 56: case 57: case 66: case 67:
      return CodeType::FREEZING_RAIN;
    case 71: case 73: case 75: case 77: case 85: case 86:
      return CodeType::SNOW;
    case 51: case 53: case 55: case 61: case 63: case 65:
    case 80: case 81: case 82: case 95: case 96: case 99:
      return CodeType::RAIN;
    default:
      return CodeType::NONE;
  }
}

// Builds the precipitation screen: type, amount and chance together, e.g.
//   "RAIN 0.30 IN  40% CHANCE"
//   "SNOW 1.2 IN  40% CHANCE"
//   "RAIN 0.10 IN + SNOW 0.5 IN  60% CHANCE"   (both forecast)
//   "FREEZING RAIN 0.05 IN  30% CHANCE"
// The type comes from Open-Meteo's forecast amounts; if none are forecast,
// from today's weather code. The chance is today's overall precipitation
// probability (Open-Meteo doesn't give separate rain and snow chances).
// Returns false when the chance isn't above MIN_PRECIP_CHANCE.
inline bool formatPrecip(const Forecast& f, char* out, size_t cap) {
  if (f.precipChance <= MIN_PRECIP_CHANCE) return false;

  CodeType byCode = codeType(f.code);
  bool rain = f.rainInches > 0;
  bool snow = f.snowInches > 0;
  if (!rain && !snow) {
    rain = byCode == CodeType::RAIN || byCode == CodeType::FREEZING_RAIN;
    snow = byCode == CodeType::SNOW;
  }
  const char* rainName = byCode == CodeType::FREEZING_RAIN ? "FREEZING RAIN" : "RAIN";

  char rainAmt[16], snowAmt[16];
  formatInches(rainAmt, sizeof(rainAmt), f.rainInches, 2);
  formatInches(snowAmt, sizeof(snowAmt), f.snowInches, 1);

  if (rain && snow) {
    snprintf(out, cap, "%s %s IN + SNOW %s IN  %d%% CHANCE", rainName, rainAmt, snowAmt, f.precipChance);
  } else if (snow) {
    snprintf(out, cap, "SNOW %s IN  %d%% CHANCE", snowAmt, f.precipChance);
  } else if (rain) {
    snprintf(out, cap, "%s %s IN  %d%% CHANCE", rainName, rainAmt, f.precipChance);
  } else {
    // A chance with no type in the forecast (e.g. a cloudy day with a slight chance)
    snprintf(out, cap, "PRECIP %s IN  %d%% CHANCE", rainAmt, f.precipChance);
  }
  return true;
}

// Describes cloud cover using the National Weather Service's sky-condition
// bands, e.g. "MOSTLY CLOUDY  70% CLOUD COVER". Returns false if unknown.
inline bool formatClouds(const Forecast& f, char* out, size_t cap) {
  if (f.cloudCover < 0) return false;
  int c = f.cloudCover;
  const char* sky = c <= 12   ? "CLEAR"
                    : c <= 37 ? "MOSTLY CLEAR"
                    : c <= 62 ? "PARTLY CLOUDY"
                    : c <= 87 ? "MOSTLY CLOUDY"
                              : "CLOUDY";
  snprintf(out, cap, "%s  %d%% CLOUD COVER", sky, c);
  return true;
}

// Turns a forecast into the screens shown on the sign, in order:
//   AMES  ->  PARTLY CLOUDY  ->  NOW 62F  HIGH 66F  LOW 51F  ->  RAIN 0.30 IN  40% CHANCE
// The last screen is precipitation when its chance is above
// MIN_PRECIP_CHANCE, otherwise how cloudy the sky is. The sign has no degree symbol, so temperatures read
// like "66F". Returns the number of screens written.
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

  if (n < maxPages && (formatPrecip(f, pages[n], PAGE_LEN) || formatClouds(f, pages[n], PAGE_LEN))) ++n;
  return n;
}

}  // namespace weather
