// Transit.h - bus arrival times from the Transitland v2 REST API
// (https://www.transit.land/documentation/rest-api).
//
//   stop search:  GET /api/v2/rest/stops?search=<name>&lat=&lon=&radius=
//   departures:   GET /api/v2/rest/stops/<stop onestop_id>/departures?next=<s>
//
// The API key goes in an "apikey" request header. Each departure carries
// departure.scheduled_utc, plus departure.estimated_utc when the agency
// publishes live (GTFS-Realtime) predictions. Responses are read with the
// streaming parser in JsonStream.h, so only the fields below are kept.
//
// Pure C++ with no Arduino dependencies so it can be unit tested on a PC.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "JsonStream.h"
#include "Weather.h"

namespace transit {

constexpr const char* API_BASE = "https://transit.land/api/v2/rest";
constexpr size_t ID_LEN = 80;     // onestop ids, e.g. "s-9zmm1qqzhd-lincolnway~beachave"
constexpr size_t NAME_LEN = 48;
constexpr size_t SHORT_LEN = 16;
constexpr size_t LABEL_LEN = 20;  // route name as shown on the sign
constexpr size_t MAX_ROUTES = 16;
constexpr size_t MAX_STOPS = 20;
constexpr size_t MAX_DEPARTURES = 64;
constexpr int64_t NO_TIME = INT64_MIN;

// ---------------------------------------------------------------- text & time

// UTF-8 text to the sign's upper-case ASCII ("Café" -> "CAFE").
inline void signText(const char* in, char* out, size_t cap) {
  if (!cap) return;
  size_t n = 0;
  for (const unsigned char* p = (const unsigned char*)in; *p; ++p) {
    char c = (char)*p;
    if (*p == 0xC3 && p[1]) {
      ++p;
      c = weather::foldLatin1(0xC0 + (*p & 0x3F));
      if (!c) continue;
    } else if (*p >= 0x80 || *p < 0x20 || *p == 0x7F) {
      continue;
    }
    if (n + 1 < cap) out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  }
  while (n && out[n - 1] == ' ') --n;
  out[n] = '\0';
}

inline void copy(char* out, size_t cap, const char* in) {
  if (!cap) return;
  size_t n = strlen(in);
  if (n >= cap) n = cap - 1;
  memcpy(out, in, n);
  out[n] = '\0';
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant).
inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

inline bool digits(const char* s, int n, int& out) {
  out = 0;
  for (int i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    out = out * 10 + (s[i] - '0');
  }
  return true;
}

// "YYYY-MM-DD" -> days since 1970.
inline bool parseDate(const char* s, int64_t& days) {
  int y, mo, d;
  if (!digits(s, 4, y) || s[4] != '-' || !digits(s + 5, 2, mo) || s[7] != '-' || !digits(s + 8, 2, d))
    return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
  days = daysFromCivil(y, (unsigned)mo, (unsigned)d);
  return true;
}

// "HH:MM:SS" -> seconds. GTFS times can pass 24:00:00 for trips after midnight.
inline bool parseClock(const char* s, int64_t& secs) {
  int h, m, sec;
  if (!digits(s, 2, h) || s[2] != ':' || !digits(s + 3, 2, m) || s[5] != ':' || !digits(s + 6, 2, sec))
    return false;
  secs = h * 3600 + m * 60 + sec;
  return true;
}

// RFC 3339 / ISO 8601 date-time with a zone ("2026-10-18T14:05:00Z",
// "2026-10-18T09:05:00.000-05:00") -> Unix time. False without a date or zone.
inline bool parseIsoTime(const char* s, int64_t& epoch) {
  int64_t days, secs;
  if (strlen(s) < 19 || !parseDate(s, days) || (s[10] != 'T' && s[10] != ' ') || !parseClock(s + 11, secs))
    return false;
  const char* p = s + 19;
  if (*p == '.')
    for (++p; *p >= '0' && *p <= '9'; ++p) {
    }
  int64_t offset = 0;
  if (*p == 'Z' || *p == 'z') {
    offset = 0;
  } else if (*p == '+' || *p == '-') {
    int oh, om;
    const char* q = p + 1;
    if (!digits(q, 2, oh)) return false;
    q += 2;
    if (*q == ':') ++q;
    if (!digits(q, 2, om)) return false;
    offset = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
  } else {
    return false;
  }
  epoch = days * 86400 + secs - offset;
  return true;
}

// ---------------------------------------------------------------- routes

struct Route {
  char id[ID_LEN] = "";           // route onestop_id, e.g. "r-9zmm-1"
  char shortName[SHORT_LEN] = "";  // e.g. "1"
  char longName[NAME_LEN] = "";    // e.g. "Red"
};

// Name shown on the sign: "1 RED" from short "1" + long "Red", or whichever
// one exists. A long name that already starts with the short one is used
// alone ("1 Red West" stays "1 RED WEST").
inline void routeLabel(const Route& r, char* out, size_t cap) {
  char s[SHORT_LEN], l[NAME_LEN], both[SHORT_LEN + NAME_LEN + 2];
  signText(r.shortName, s, sizeof(s));
  signText(r.longName, l, sizeof(l));
  size_t sl = strlen(s);
  if (!*s) copy(both, sizeof(both), l);
  else if (!*l || (strncmp(l, s, sl) == 0 && (l[sl] == ' ' || l[sl] == '\0'))) copy(both, sizeof(both), *l ? l : s);
  else snprintf(both, sizeof(both), "%s %s", s, l);
  if (!*both) copy(both, sizeof(both), r.id);
  size_t limit = cap < LABEL_LEN + 1 ? cap : LABEL_LEN + 1;
  copy(out, limit, both);
  size_t n = strlen(out);
  while (n && out[n - 1] == ' ') out[--n] = '\0';
}

struct RouteList {
  Route items[MAX_ROUTES];
  size_t count = 0;

  int find(const char* id) const {
    for (size_t i = 0; i < count; ++i)
      if (strcmp(items[i].id, id) == 0) return (int)i;
    return -1;
  }
  // Adds (or refreshes the names of) a route. Returns its index or -1 if full.
  int add(const Route& r) {
    if (!*r.id) return -1;
    int i = find(r.id);
    if (i >= 0) {
      if (*r.shortName) copy(items[i].shortName, SHORT_LEN, r.shortName);
      if (*r.longName) copy(items[i].longName, NAME_LEN, r.longName);
      return i;
    }
    if (count >= MAX_ROUTES) return -1;
    items[count] = r;
    return (int)count++;
  }
  void clear() { count = 0; }
};

// Sorts routes by short name the way people expect: 1, 2, 10, 23 (numbers
// numerically, then text), then long name.
inline int compareRoutes(const Route& a, const Route& b) {
  char* ea;
  char* eb;
  long na = strtol(a.shortName, &ea, 10);
  long nb = strtol(b.shortName, &eb, 10);
  bool hasA = ea != a.shortName, hasB = eb != b.shortName;
  if (hasA && hasB && na != nb) return na < nb ? -1 : 1;
  if (hasA != hasB) return hasA ? -1 : 1;
  int c = strcmp(a.shortName, b.shortName);
  return c ? c : strcmp(a.longName, b.longName);
}

inline void sortRoutes(RouteList& list) {
  for (size_t i = 1; i < list.count; ++i)
    for (size_t j = i; j > 0 && compareRoutes(list.items[j], list.items[j - 1]) < 0; --j) {
      Route t = list.items[j];
      list.items[j] = list.items[j - 1];
      list.items[j - 1] = t;
    }
}

// Saved form of a route list: one "id<TAB>short<TAB>long" line per route.
inline size_t serialize(const RouteList& list, char* out, size_t cap) {
  size_t n = 0;
  if (cap) out[0] = '\0';
  for (size_t i = 0; i < list.count; ++i) {
    const Route& r = list.items[i];
    int w = snprintf(out + n, cap - n, "%s\t%s\t%s\n", r.id, r.shortName, r.longName);
    if (w < 0 || (size_t)w >= cap - n) {
      out[n] = '\0';
      break;
    }
    n += (size_t)w;
  }
  return n;
}

inline void deserialize(const char* in, RouteList& list) {
  list.clear();
  const char* p = in;
  while (*p) {
    const char* end = strchr(p, '\n');
    if (!end) end = p + strlen(p);
    Route r;
    char* fields[3] = {r.id, r.shortName, r.longName};
    const size_t caps[3] = {ID_LEN, SHORT_LEN, NAME_LEN};
    const char* f = p;
    for (int k = 0; k < 3; ++k) {
      const char* tab = k < 2 ? (const char*)memchr(f, '\t', (size_t)(end - f)) : nullptr;
      const char* fe = tab ? tab : end;
      size_t len = (size_t)(fe - f);
      if (len >= caps[k]) len = caps[k] - 1;
      memcpy(fields[k], f, len);
      fields[k][len] = '\0';
      if (!tab) break;
      f = tab + 1;
    }
    list.add(r);
    p = *end ? end + 1 : end;
  }
}

// ---------------------------------------------------------------- URLs

inline size_t urlEncode(char* out, size_t cap, const char* in) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  size_t n = 0;
  for (const unsigned char* p = (const unsigned char*)in; *p && n + 4 < cap; ++p) {
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
        *p == '_' || *p == '.' || *p == '~') {
      out[n++] = (char)*p;
    } else {
      out[n++] = '%';
      out[n++] = HEX_DIGITS[*p >> 4];
      out[n++] = HEX_DIGITS[*p & 0xF];
    }
  }
  if (cap) out[n < cap ? n : cap - 1] = '\0';
  return n;
}

// Stops whose name matches `query`, within `radiusM` metres of lat/lon
// (pass NAN coordinates to search everywhere).
inline bool stopSearchUrl(char* out, size_t cap, const char* query, double lat, double lon, int radiusM,
                          int limit) {
  char q[128];
  urlEncode(q, sizeof(q), query);
  int n;
  if (lat == lat && lon == lon) {
    n = snprintf(out, cap, "%s/stops?search=%s&lat=%.5f&lon=%.5f&radius=%d&limit=%d", API_BASE, q, lat, lon,
                 radiusM, limit);
  } else {
    n = snprintf(out, cap, "%s/stops?search=%s&limit=%d", API_BASE, q, limit);
  }
  return n > 0 && (size_t)n < cap;
}

// Departures from a stop over the next `nextSeconds`.
inline bool departuresUrl(char* out, size_t cap, const char* stopId, long nextSeconds, int limit) {
  char id[ID_LEN * 3];
  urlEncode(id, sizeof(id), stopId);
  int n = snprintf(out, cap, "%s/stops/%s/departures?next=%ld&limit=%d", API_BASE, id, nextSeconds, limit);
  return n > 0 && (size_t)n < cap;
}

// ---------------------------------------------------------------- stop search

struct Stop {
  char id[ID_LEN] = "";
  char name[NAME_LEN] = "";
  char code[SHORT_LEN] = "";
  char routes[NAME_LEN] = "";  // short names of the routes serving it, if listed
  double lat = 0, lon = 0;
};

// Collects the stops of a /stops response.
struct StopReader : jsonstream::Handler {
  Stop* out;
  size_t max;
  size_t count = 0;
  Stop cur;
  int coord = 0;
  bool active = false;

  StopReader(Stop* o, size_t m) : out(o), max(m) {}

  void onStart(const char* path, bool object) override {
    if (object && strcmp(path, "stops") == 0) {
      cur = Stop();
      coord = 0;
      active = true;
    }
  }
  void onEnd(const char* path, bool object) override {
    if (!object || !active || strcmp(path, "stops") != 0) return;
    active = false;
    if (*cur.id && count < max) out[count++] = cur;
  }
  void addRoute(const char* name) {
    if (!*name) return;
    char t[SHORT_LEN];
    signText(name, t, sizeof(t));
    // skip duplicates (each direction can be listed)
    size_t len = strlen(t);
    for (const char* p = cur.routes; (p = strstr(p, t)) != nullptr; p += len) {
      bool startOk = p == cur.routes || p[-1] == ' ';
      if (startOk && (p[len] == ' ' || p[len] == '\0')) return;
    }
    size_t n = strlen(cur.routes);
    if (n + len + 2 > sizeof(cur.routes)) return;
    if (n) cur.routes[n++] = ' ';
    memcpy(cur.routes + n, t, len + 1);
  }
  void onValue(const char* path, const char* v, bool isString) override {
    if (!active || strncmp(path, "stops.", 6) != 0) return;
    const char* k = path + 6;
    if (strcmp(k, "onestop_id") == 0) copy(cur.id, ID_LEN, v);
    else if (strcmp(k, "stop_name") == 0) copy(cur.name, NAME_LEN, v);
    else if (strcmp(k, "stop_code") == 0 && isString) copy(cur.code, SHORT_LEN, v);
    else if (strcmp(k, "geometry.coordinates") == 0) {
      double d = atof(v);
      if (coord == 0) cur.lon = d;
      else if (coord == 1) cur.lat = d;
      ++coord;
    } else if (strcmp(k, "route_stops.route.route_short_name") == 0) {
      addRoute(v);
    }
  }
};

// ---------------------------------------------------------------- departures

struct Departure {
  uint8_t route = 0;        // index into Board::routes
  bool realtime = false;    // live prediction, not just the timetable
  bool local = false;       // `time` is local wall-clock (resolve with the time zone)
  int64_t time = NO_TIME;   // Unix time (or local wall-clock seconds, see `local`)
};

struct Board {
  char stopName[NAME_LEN] = "";
  char stopCode[SHORT_LEN] = "";
  RouteList routes;
  Departure deps[MAX_DEPARTURES];
  size_t count = 0;
  bool stopFound = false;

  void reset() {  // in place: a Board is a few KB, too big for a temporary on the ESP32's stack
    stopName[0] = stopCode[0] = '\0';
    routes.clear();
    count = 0;
    stopFound = false;
  }
};

// Collects the departures of a /stops/<id>/departures response.
struct DepartureReader : jsonstream::Handler {
  Board& b;
  bool inDep = false;
  Route route;
  int64_t estimated = NO_TIME, scheduled = NO_TIME;
  int64_t serviceDay = NO_TIME, depClock = NO_TIME, arrClock = NO_TIME;
  bool canceled = false;
  bool fromDeparture = false;  // have times from "departure" (preferred over "arrival")

  explicit DepartureReader(Board& board) : b(board) { b.reset(); }

  void onStart(const char* path, bool object) override {
    if (!object) return;
    if (strcmp(path, "stops") == 0) {
      b.stopFound = true;
    } else if (strcmp(path, "stops.departures") == 0) {
      inDep = true;
      route = Route();
      estimated = scheduled = NO_TIME;
      serviceDay = depClock = arrClock = NO_TIME;
      canceled = false;
      fromDeparture = false;
    }
  }

  void onEnd(const char* path, bool object) override {
    if (!object || !inDep || strcmp(path, "stops.departures") != 0) return;
    inDep = false;
    if (canceled || b.count >= MAX_DEPARTURES) return;
    if (!*route.id) return;
    Departure d;
    if (estimated != NO_TIME) {
      d.time = estimated;
      d.realtime = true;
    } else if (scheduled != NO_TIME) {
      d.time = scheduled;
    } else if (serviceDay != NO_TIME && (depClock != NO_TIME || arrClock != NO_TIME)) {
      d.time = serviceDay * 86400 + (depClock != NO_TIME ? depClock : arrClock);
      d.local = true;
    } else {
      return;
    }
    int i = b.routes.add(route);
    if (i < 0) return;
    d.route = (uint8_t)i;
    b.deps[b.count++] = d;
  }

  // departure.* and arrival.* times; departure wins when both are present.
  void takeTime(const char* field, const char* v, bool isDeparture) {
    if (!isDeparture && fromDeparture) return;
    int64_t t;
    if (!parseIsoTime(v, t)) return;
    bool est = strncmp(field, "estimated", 9) == 0;
    bool sched = strncmp(field, "scheduled", 9) == 0;
    if (!est && !sched) return;
    if (isDeparture && !fromDeparture) {
      fromDeparture = true;
      estimated = scheduled = NO_TIME;  // drop arrival times
    }
    if (est) estimated = t;
    else scheduled = t;
  }

  void onValue(const char* path, const char* v, bool isString) override {
    if (!inDep) {
      if (strcmp(path, "stops.stop_name") == 0) copy(b.stopName, NAME_LEN, v);
      else if (strcmp(path, "stops.stop_code") == 0 && isString) copy(b.stopCode, SHORT_LEN, v);
      return;
    }
    if (strncmp(path, "stops.departures.", 17) != 0) return;
    const char* k = path + 17;
    if (strncmp(k, "departure.", 10) == 0) takeTime(k + 10, v, true);
    else if (strncmp(k, "arrival.", 8) == 0) takeTime(k + 8, v, false);
    else if (strcmp(k, "trip.route.onestop_id") == 0) copy(route.id, ID_LEN, v);
    else if (strcmp(k, "trip.route.route_id") == 0 && !*route.id) copy(route.id, ID_LEN, v);
    else if (strcmp(k, "trip.route.route_short_name") == 0) copy(route.shortName, SHORT_LEN, v);
    else if (strcmp(k, "trip.route.route_long_name") == 0) copy(route.longName, NAME_LEN, v);
    else if (strcmp(k, "service_date") == 0) parseDate(v, serviceDay);
    else if (strcmp(k, "departure_time") == 0) parseClock(v, depClock);
    else if (strcmp(k, "arrival_time") == 0) parseClock(v, arrClock);
    else if (strcmp(k, "schedule_relationship") == 0) canceled = strcmp(v, "CANCELED") == 0 || strcmp(v, "CANCELLED") == 0;
  }
};

// Converts departures that only had a local wall-clock time to Unix time.
template <typename F>
inline void resolveLocalTimes(Board& b, F wallToEpoch) {
  for (size_t i = 0; i < b.count; ++i) {
    if (b.deps[i].local) {
      b.deps[i].time = wallToEpoch(b.deps[i].time);
      b.deps[i].local = false;
    }
  }
}

// ---------------------------------------------------------------- sign text

// Minutes until the next bus on `routeId` at or after `now`, or -1 if none.
inline long nextMinutes(const Board& b, const char* routeId, int64_t now) {
  int r = b.routes.find(routeId);
  if (r < 0) return -1;
  int64_t best = NO_TIME;
  for (size_t i = 0; i < b.count; ++i) {
    const Departure& d = b.deps[i];
    if (d.route != r || d.local || d.time < now) continue;
    if (best == NO_TIME || d.time < best) best = d.time;
  }
  return best == NO_TIME ? -1 : (long)((best - now) / 60);
}

// "1 RED 5 MIN   23 ORANGE NOW   6 BROWN NO BUS" for the chosen routes, in
// the order given. A bus less than a minute away shows as NOW.
inline size_t formatArrivals(const Board& b, const RouteList& chosen, int64_t now, char* out, size_t cap,
                             const char* separator = "   ") {
  size_t n = 0;
  if (cap) out[0] = '\0';
  for (size_t i = 0; i < chosen.count; ++i) {
    char label[LABEL_LEN + 1];
    routeLabel(chosen.items[i], label, sizeof(label));
    long m = nextMinutes(b, chosen.items[i].id, now);
    char when[24];
    if (m < 0) copy(when, sizeof(when), "NO BUS");
    else if (m == 0) copy(when, sizeof(when), "NOW");
    else snprintf(when, sizeof(when), "%ld MIN", m);
    int w = snprintf(out + n, cap - n, "%s%s %s", i ? separator : "", label, when);
    if (w < 0 || (size_t)w >= cap - n) {
      out[n] = '\0';
      break;
    }
    n += (size_t)w;
  }
  return n;
}

}  // namespace transit
