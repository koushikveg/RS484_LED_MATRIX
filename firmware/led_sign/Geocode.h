// Geocode.h - turns "City, State, Country" into a weather location using the
// Open-Meteo geocoding API (https://open-meteo.com/en/docs/geocoding-api).
//
// Open-Meteo only searches by place name, so the firmware asks for the top
// matches for the city and picks the first one whose state/province and
// country agree with what was typed. Accepted input:
//   "Ames"                         first match anywhere
//   "Ames, Iowa" / "Ames, IA"      second part is a state OR a country
//   "Paris, France"                (decided per match, so "Atlanta, Georgia"
//   "Tbilisi, Georgia"              and "Tbilisi, Georgia" both work)
//   "Ames, IA, US"                 city, state, country
// Countries: two/three-letter codes or names and common aliases, e.g. US,
// USA, U.S.A., United States, United States of America, America. States:
// full names, or postal abbreviations for the US, Canada and Australia.
//
// Pure C++ with no Arduino dependencies so it can be unit tested on a PC.

#pragma once

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "Weather.h"

namespace geo {

constexpr size_t FIELD_LEN = 48;
constexpr size_t MAX_RESULTS = 20;

// Normalises for comparison: upper case, accents folded (UTF-8 Latin-1),
// letters/digits kept, '-' and runs of spaces become one space, everything
// else (periods, apostrophes, ...) dropped. "U.S.A." -> "USA",
// "Winston-Salem" -> "WINSTON SALEM", "Montréal" -> "MONTREAL".
inline void normalize(const char* in, char* out, size_t cap) {
  size_t n = 0;
  bool space = false;
  for (const unsigned char* p = (const unsigned char*)in; p && *p && n + 1 < cap; ++p) {
    char c = 0;
    if (*p == 0xC3 && p[1]) {
      ++p;
      c = weather::foldLatin1(0xC0 + (*p & 0x3F));
    } else if (*p < 0x80 && isalnum(*p)) {
      c = (char)toupper(*p);
    } else if (*p == ' ' || *p == '-' || *p == '\t' || *p == '_') {
      space = n > 0;
      continue;
    }
    if (!c) continue;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (space && n + 2 < cap) out[n++] = ' ';
    space = false;
    out[n++] = c;
  }
  out[n] = '\0';
}

// ---------------------------------------------------------------- countries

struct Country {
  const char* iso2;
  const char* names;  // normalised, '|' separated: ISO3, names, aliases
};

// Common countries and their aliases. Countries not listed still match by the
// name or two-letter code Open-Meteo returns for each result.
static const Country COUNTRIES[] = {
    {"US", "USA|UNITED STATES|UNITED STATES OF AMERICA|AMERICA|US OF A|U S|U S A"},
    {"CA", "CAN|CANADA"},
    {"MX", "MEX|MEXICO"},
    {"GB", "GBR|UK|U K|UNITED KINGDOM|GREAT BRITAIN|BRITAIN|ENGLAND|SCOTLAND|WALES|NORTHERN IRELAND"},
    {"IE", "IRL|IRELAND|REPUBLIC OF IRELAND"},
    {"FR", "FRA|FRANCE"},
    {"DE", "DEU|GERMANY|DEUTSCHLAND"},
    {"ES", "ESP|SPAIN|ESPANA"},
    {"PT", "PRT|PORTUGAL"},
    {"IT", "ITA|ITALY|ITALIA"},
    {"NL", "NLD|NETHERLANDS|THE NETHERLANDS|HOLLAND"},
    {"BE", "BEL|BELGIUM"},
    {"CH", "CHE|SWITZERLAND"},
    {"AT", "AUT|AUSTRIA"},
    {"SE", "SWE|SWEDEN"},
    {"NO", "NOR|NORWAY"},
    {"DK", "DNK|DENMARK"},
    {"FI", "FIN|FINLAND"},
    {"IS", "ISL|ICELAND"},
    {"PL", "POL|POLAND"},
    {"CZ", "CZE|CZECHIA|CZECH REPUBLIC"},
    {"SK", "SVK|SLOVAKIA"},
    {"HU", "HUN|HUNGARY"},
    {"RO", "ROU|ROMANIA"},
    {"BG", "BGR|BULGARIA"},
    {"GR", "GRC|GREECE"},
    {"TR", "TUR|TURKEY|TURKIYE"},
    {"UA", "UKR|UKRAINE"},
    {"RU", "RUS|RUSSIA|RUSSIAN FEDERATION"},
    {"HR", "HRV|CROATIA"},
    {"RS", "SRB|SERBIA"},
    {"SI", "SVN|SLOVENIA"},
    {"BA", "BIH|BOSNIA|BOSNIA AND HERZEGOVINA"},
    {"LT", "LTU|LITHUANIA"},
    {"LV", "LVA|LATVIA"},
    {"EE", "EST|ESTONIA"},
    {"LU", "LUX|LUXEMBOURG"},
    {"MT", "MLT|MALTA"},
    {"CY", "CYP|CYPRUS"},
    {"GE", "GEO|GEORGIA"},
    {"IL", "ISR|ISRAEL"},
    {"AE", "ARE|UAE|UNITED ARAB EMIRATES|EMIRATES"},
    {"SA", "SAU|KSA|SAUDI ARABIA"},
    {"QA", "QAT|QATAR"},
    {"KW", "KWT|KUWAIT"},
    {"OM", "OMN|OMAN"},
    {"JO", "JOR|JORDAN"},
    {"LB", "LBN|LEBANON"},
    {"IQ", "IRQ|IRAQ"},
    {"IR", "IRN|IRAN"},
    {"EG", "EGY|EGYPT"},
    {"MA", "MAR|MOROCCO"},
    {"DZ", "DZA|ALGERIA"},
    {"TN", "TUN|TUNISIA"},
    {"NG", "NGA|NIGERIA"},
    {"GH", "GHA|GHANA"},
    {"KE", "KEN|KENYA"},
    {"ET", "ETH|ETHIOPIA"},
    {"TZ", "TZA|TANZANIA"},
    {"UG", "UGA|UGANDA"},
    {"ZA", "ZAF|SOUTH AFRICA|RSA"},
    {"IN", "IND|INDIA"},
    {"PK", "PAK|PAKISTAN"},
    {"BD", "BGD|BANGLADESH"},
    {"LK", "LKA|SRI LANKA"},
    {"NP", "NPL|NEPAL"},
    {"CN", "CHN|CHINA|PRC|PEOPLES REPUBLIC OF CHINA"},
    {"HK", "HKG|HONG KONG"},
    {"TW", "TWN|TAIWAN"},
    {"JP", "JPN|JAPAN"},
    {"KR", "KOR|SOUTH KOREA|KOREA|REPUBLIC OF KOREA"},
    {"PH", "PHL|PHILIPPINES"},
    {"VN", "VNM|VIETNAM|VIET NAM"},
    {"TH", "THA|THAILAND"},
    {"MY", "MYS|MALAYSIA"},
    {"SG", "SGP|SINGAPORE"},
    {"ID", "IDN|INDONESIA"},
    {"AU", "AUS|AUSTRALIA"},
    {"NZ", "NZL|NEW ZEALAND"},
    {"BR", "BRA|BRAZIL|BRASIL"},
    {"AR", "ARG|ARGENTINA"},
    {"CL", "CHL|CHILE"},
    {"CO", "COL|COLOMBIA"},
    {"PE", "PER|PERU"},
    {"VE", "VEN|VENEZUELA"},
    {"EC", "ECU|ECUADOR"},
    {"UY", "URY|URUGUAY"},
    {"CR", "CRI|COSTA RICA"},
    {"PA", "PAN|PANAMA"},
    {"CU", "CUB|CUBA"},
    {"DO", "DOM|DOMINICAN REPUBLIC"},
    {"JM", "JAM|JAMAICA"},
    {"PR", "PRI|PUERTO RICO"},
};
constexpr size_t COUNTRY_COUNT = sizeof(COUNTRIES) / sizeof(COUNTRIES[0]);

// True if `normalized` equals one of the '|' separated entries in `list`.
inline bool inList(const char* normalized, const char* list) {
  size_t len = strlen(normalized);
  const char* p = list;
  while (*p) {
    const char* bar = strchr(p, '|');
    size_t itemLen = bar ? (size_t)(bar - p) : strlen(p);
    if (itemLen == len && strncmp(p, normalized, len) == 0) return true;
    if (!bar) break;
    p = bar + 1;
  }
  return false;
}

// Maps a typed country (already normalised) to its ISO 3166 two-letter code,
// or nullptr if it isn't in the table (it can still match a result by name).
inline const char* countryCode(const char* normalized) {
  if (!*normalized) return nullptr;
  for (size_t i = 0; i < COUNTRY_COUNT; ++i) {
    if (strcmp(normalized, COUNTRIES[i].iso2) == 0 || inList(normalized, COUNTRIES[i].names))
      return COUNTRIES[i].iso2;
  }
  return nullptr;
}

// ---------------------------------------------------------------- states

struct Region {
  const char* iso2;  // country
  const char* abbr;
  const char* name;  // normalised, as Open-Meteo's admin1
};

static const Region REGIONS[] = {
    // United States
    {"US", "AL", "ALABAMA"}, {"US", "AK", "ALASKA"}, {"US", "AZ", "ARIZONA"}, {"US", "AR", "ARKANSAS"},
    {"US", "CA", "CALIFORNIA"}, {"US", "CO", "COLORADO"}, {"US", "CT", "CONNECTICUT"},
    {"US", "DE", "DELAWARE"}, {"US", "FL", "FLORIDA"}, {"US", "GA", "GEORGIA"}, {"US", "HI", "HAWAII"},
    {"US", "ID", "IDAHO"}, {"US", "IL", "ILLINOIS"}, {"US", "IN", "INDIANA"}, {"US", "IA", "IOWA"},
    {"US", "KS", "KANSAS"}, {"US", "KY", "KENTUCKY"}, {"US", "LA", "LOUISIANA"}, {"US", "ME", "MAINE"},
    {"US", "MD", "MARYLAND"}, {"US", "MA", "MASSACHUSETTS"}, {"US", "MI", "MICHIGAN"},
    {"US", "MN", "MINNESOTA"}, {"US", "MS", "MISSISSIPPI"}, {"US", "MO", "MISSOURI"},
    {"US", "MT", "MONTANA"}, {"US", "NE", "NEBRASKA"}, {"US", "NV", "NEVADA"},
    {"US", "NH", "NEW HAMPSHIRE"}, {"US", "NJ", "NEW JERSEY"}, {"US", "NM", "NEW MEXICO"},
    {"US", "NY", "NEW YORK"}, {"US", "NC", "NORTH CAROLINA"}, {"US", "ND", "NORTH DAKOTA"},
    {"US", "OH", "OHIO"}, {"US", "OK", "OKLAHOMA"}, {"US", "OR", "OREGON"}, {"US", "PA", "PENNSYLVANIA"},
    {"US", "RI", "RHODE ISLAND"}, {"US", "SC", "SOUTH CAROLINA"}, {"US", "SD", "SOUTH DAKOTA"},
    {"US", "TN", "TENNESSEE"}, {"US", "TX", "TEXAS"}, {"US", "UT", "UTAH"}, {"US", "VT", "VERMONT"},
    {"US", "VA", "VIRGINIA"}, {"US", "WA", "WASHINGTON"}, {"US", "WV", "WEST VIRGINIA"},
    {"US", "WI", "WISCONSIN"}, {"US", "WY", "WYOMING"}, {"US", "DC", "DISTRICT OF COLUMBIA"},
    {"US", "DC", "WASHINGTON DC"},
    // Canada
    {"CA", "AB", "ALBERTA"}, {"CA", "BC", "BRITISH COLUMBIA"}, {"CA", "MB", "MANITOBA"},
    {"CA", "NB", "NEW BRUNSWICK"}, {"CA", "NL", "NEWFOUNDLAND AND LABRADOR"}, {"CA", "NS", "NOVA SCOTIA"},
    {"CA", "NT", "NORTHWEST TERRITORIES"}, {"CA", "NU", "NUNAVUT"}, {"CA", "ON", "ONTARIO"},
    {"CA", "PE", "PRINCE EDWARD ISLAND"}, {"CA", "QC", "QUEBEC"}, {"CA", "SK", "SASKATCHEWAN"},
    {"CA", "YT", "YUKON"},
    // Australia
    {"AU", "NSW", "NEW SOUTH WALES"}, {"AU", "VIC", "VICTORIA"}, {"AU", "QLD", "QUEENSLAND"},
    {"AU", "SA", "SOUTH AUSTRALIA"}, {"AU", "WA", "WESTERN AUSTRALIA"}, {"AU", "TAS", "TASMANIA"},
    {"AU", "NT", "NORTHERN TERRITORY"}, {"AU", "ACT", "AUSTRALIAN CAPITAL TERRITORY"},
};
constexpr size_t REGION_COUNT = sizeof(REGIONS) / sizeof(REGIONS[0]);

// ---------------------------------------------------------------- results

struct Place {
  char name[FIELD_LEN] = "";
  char admin1[FIELD_LEN] = "";   // state / province, as returned
  char country[FIELD_LEN] = "";
  char countryCode[3] = "";
  double latitude = 0;
  double longitude = 0;
};

// Parses every entry of an Open-Meteo geocoding response's "results" array
// (up to `max`). Text fields are upper case ASCII. Returns how many parsed.
inline size_t parsePlaces(const char* json, Place* out, size_t max) {
  const char* b = json;
  const char* e = json + strlen(json);
  if (!weather::section(b, e, "results")) return 0;
  size_t n = 0;
  const char* p = b + 1;
  while (n < max) {
    const char* obj = (const char*)memchr(p, '{', (size_t)(e - p));
    if (!obj) break;
    const char* objEnd = weather::matchBracket(obj, e);
    if (!objEnd) break;
    Place pl;
    if (weather::number(obj, objEnd, "latitude", pl.latitude) &&
        weather::number(obj, objEnd, "longitude", pl.longitude)) {
      weather::text(obj, objEnd, "name", pl.name, sizeof(pl.name));
      weather::text(obj, objEnd, "admin1", pl.admin1, sizeof(pl.admin1));
      weather::text(obj, objEnd, "country", pl.country, sizeof(pl.country));
      weather::text(obj, objEnd, "country_code", pl.countryCode, sizeof(pl.countryCode));
      out[n++] = pl;
    }
    p = objEnd;
  }
  return n;
}

// ---------------------------------------------------------------- query

struct Query {
  char city[FIELD_LEN] = "";    // as typed (sent to the search)
  char second[FIELD_LEN] = "";  // normalised; with two parts: state OR country
  char region[FIELD_LEN] = "";  // normalised; with three parts
  char country[FIELD_LEN] = ""; // normalised; with three parts
};

inline void trimCopy(const char* begin, const char* end, char* out, size_t cap) {
  while (begin < end && isspace((unsigned char)*begin)) ++begin;
  while (end > begin && isspace((unsigned char)end[-1])) --end;
  size_t len = (size_t)(end - begin);
  if (len >= cap) len = cap - 1;
  memcpy(out, begin, len);
  out[len] = '\0';
}

// Splits "City, State, Country" at commas. Returns false if there's no city.
inline bool parseQuery(const char* in, Query& q) {
  q = Query();
  const char* parts[8];
  const char* ends[8];
  size_t count = 0;
  const char* p = in;
  while (count < 8) {
    const char* comma = strchr(p, ',');
    parts[count] = p;
    ends[count] = comma ? comma : p + strlen(p);
    ++count;
    if (!comma) break;
    p = comma + 1;
  }
  char tmp[FIELD_LEN];
  trimCopy(parts[0], ends[0], q.city, sizeof(q.city));
  if (count == 2) {
    trimCopy(parts[1], ends[1], tmp, sizeof(tmp));
    normalize(tmp, q.second, sizeof(q.second));
  } else if (count >= 3) {
    trimCopy(parts[1], ends[1], tmp, sizeof(tmp));
    normalize(tmp, q.region, sizeof(q.region));
    trimCopy(parts[count - 1], ends[count - 1], tmp, sizeof(tmp));
    normalize(tmp, q.country, sizeof(q.country));
  }
  return q.city[0] != '\0';
}

// Does a result's state/province match what was typed (name or abbreviation)?
inline bool regionMatches(const char* typed, const Place& pl) {
  if (!*typed) return true;
  char admin[FIELD_LEN];
  normalize(pl.admin1, admin, sizeof(admin));
  if (*admin && strcmp(admin, typed) == 0) return true;
  for (size_t i = 0; i < REGION_COUNT; ++i) {
    if (strcmp(REGIONS[i].iso2, pl.countryCode) != 0) continue;
    if (strcmp(REGIONS[i].abbr, typed) == 0 || strcmp(REGIONS[i].name, typed) == 0) {
      if (strcmp(REGIONS[i].name, admin) == 0) return true;
    }
  }
  return false;
}

// Does a result's country match what was typed (code, name or alias)?
inline bool countryMatches(const char* typed, const Place& pl) {
  if (!*typed) return true;
  const char* iso = countryCode(typed);
  if (iso && strcmp(iso, pl.countryCode) == 0) return true;
  if (strcmp(typed, pl.countryCode) == 0) return true;
  char name[FIELD_LEN];
  normalize(pl.country, name, sizeof(name));
  return *name && strcmp(name, typed) == 0;
}

// Picks the first (most relevant) result that fits the query, or -1.
inline int pick(const Query& q, const Place* places, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const Place& pl = places[i];
    if (*q.second) {
      if (regionMatches(q.second, pl) || countryMatches(q.second, pl)) return (int)i;
    } else if (regionMatches(q.region, pl) && countryMatches(q.country, pl)) {
      return (int)i;
    }
  }
  return -1;
}

// Is this (normalised) text a known state/province name or abbreviation?
inline bool isRegion(const char* typed) {
  for (size_t i = 0; i < REGION_COUNT; ++i) {
    if (strcmp(REGIONS[i].abbr, typed) == 0 || strcmp(REGIONS[i].name, typed) == 0) return true;
  }
  return false;
}

// ISO code of the country the query names, or nullptr. A two-part query like
// "Paris, France" counts, but not one whose second part is also a state
// ("Atlanta, Georgia", "Ames, CA"), since that could be either.
inline const char* queryCountryCode(const Query& q) {
  if (*q.country) return countryCode(q.country);
  if (*q.second && !isRegion(q.second)) return countryCode(q.second);
  return nullptr;
}

// Builds the geocoding search URL for the city. With a known country it also
// passes countryCode so smaller towns aren't crowded out by bigger places of
// the same name elsewhere.
inline void searchUrl(char* out, size_t cap, const Query& q, bool withCountryCode) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  size_t n = (size_t)snprintf(out, cap,
                              "https://geocoding-api.open-meteo.com/v1/search?count=%u&language=en&format=json",
                              (unsigned)MAX_RESULTS);
  const char* iso = withCountryCode ? queryCountryCode(q) : nullptr;
  if (iso && n + 16 < cap) n += (size_t)snprintf(out + n, cap - n, "&countryCode=%s", iso);
  if (n + 6 < cap) n += (size_t)snprintf(out + n, cap - n, "&name=");
  for (const unsigned char* p = (const unsigned char*)q.city; *p && n + 4 < cap; ++p) {
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

// "IOWA, UNITED STATES" style description of a result.
inline void describePlace(const Place& pl, char* out, size_t cap) {
  if (*pl.admin1 && *pl.country) snprintf(out, cap, "%s, %s", pl.admin1, pl.country);
  else if (*pl.country) snprintf(out, cap, "%s", pl.country);
  else snprintf(out, cap, "%s", pl.admin1);
}

}  // namespace geo
