// Host-side tests for JsonStream.h and Transit.h (bus arrival times).
//
//   g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_transit.cpp -o test_transit && ./test_transit

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "JsonStream.h"
#include "Transit.h"

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

static bool streq(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }

// Records every event as "S path", "E path" or "V path=value".
struct Recorder : jsonstream::Handler {
  std::vector<std::string> events;
  void onStart(const char* p, bool o) override { events.push_back(std::string(o ? "{ " : "[ ") + p); }
  void onEnd(const char* p, bool o) override { events.push_back(std::string(o ? "} " : "] ") + p); }
  void onValue(const char* p, const char* v, bool s) override {
    events.push_back(std::string("V ") + p + "=" + (s ? "\"" : "") + v + (s ? "\"" : ""));
  }
};

// Feeds `json` in pieces of `chunk` bytes, as it arrives over the network.
static void feedChunks(jsonstream::Parser& p, const char* json, size_t chunk) {
  size_t len = strlen(json);
  for (size_t i = 0; i < len; i += chunk) p.feed(json + i, i + chunk < len ? chunk : len - i);
}

// Shaped like Transitland's /stops/<id>/departures response (abridged).
static const char* DEPARTURES =
    "{\"stops\":[{\"id\":1,\"onestop_id\":\"s-9zmm1-lincolnway~beach\",\"stop_name\":\"Lincoln Way & Beach Ave\","
    "\"stop_code\":\"1092\",\"stop_timezone\":\"America/Chicago\",\"geometry\":{\"type\":\"Point\","
    "\"coordinates\":[-93.6514,42.0229]},\"departures\":["
    // 1: Red, live prediction 4.5 min after "now"
    "{\"arrival\":{\"scheduled\":\"09:04:00\",\"scheduled_utc\":\"2026-10-18T14:04:00Z\",\"estimated_utc\":null},"
    "\"departure\":{\"delay\":30,\"estimated\":\"09:04:30\",\"estimated_utc\":\"2026-10-18T14:04:30Z\","
    "\"scheduled\":\"09:04:00\",\"scheduled_utc\":\"2026-10-18T14:04:00Z\",\"uncertainty\":30},"
    "\"departure_time\":\"09:04:00\",\"service_date\":\"2026-10-18\",\"schedule_relationship\":\"SCHEDULED\","
    "\"trip\":{\"trip_headsign\":\"Ames Mall\",\"route\":{\"onestop_id\":\"r-9zmm-1\",\"route_short_name\":\"1\","
    "\"route_long_name\":\"Red\",\"route_color\":\"ff0000\",\"agency\":{\"onestop_id\":\"o-9zmm-cyride\","
    "\"agency_name\":\"CyRide\"}}}},"
    // 2: Green, timetable only, 12 min
    "{\"departure\":{\"estimated_utc\":null,\"scheduled_utc\":\"2026-10-18T14:12:00Z\"},"
    "\"service_date\":\"2026-10-18\",\"trip\":{\"route\":{\"onestop_id\":\"r-9zmm-2\","
    "\"route_short_name\":\"2\",\"route_long_name\":\"Green\"}}},"
    // 3: Red again, later
    "{\"departure\":{\"scheduled_utc\":\"2026-10-18T14:20:00Z\"},\"trip\":{\"route\":{"
    "\"onestop_id\":\"r-9zmm-1\",\"route_short_name\":\"1\",\"route_long_name\":\"Red\"}}},"
    // 4: Orange, canceled
    "{\"departure\":{\"scheduled_utc\":\"2026-10-18T14:01:00Z\"},\"schedule_relationship\":\"CANCELED\","
    "\"trip\":{\"route\":{\"onestop_id\":\"r-9zmm-23\",\"route_short_name\":\"23\","
    "\"route_long_name\":\"Orange\"}}},"
    // 5: Brown, only local times (older API shape), 09:30 Central = 14:30Z
    "{\"departure\":{\"scheduled\":\"09:30:00\"},\"departure_time\":\"09:30:00\",\"service_date\":\"2026-10-18\","
    "\"trip\":{\"route\":{\"onestop_id\":\"r-9zmm-6\",\"route_short_name\":\"6\","
    "\"route_long_name\":\"Brown\"}}},"
    // 6: Gold, already left
    "{\"departure\":{\"scheduled_utc\":\"2026-10-18T13:58:00Z\"},\"trip\":{\"route\":{"
    "\"onestop_id\":\"r-9zmm-5\",\"route_short_name\":\"5\",\"route_long_name\":\"Yellow\"}}},"
    // 7: Blue, ISO with an offset in scheduled_local only, 09:00:20 Central = 14:00:20Z
    "{\"departure\":{\"scheduled_local\":\"2026-10-18T09:00:20-05:00\"},\"trip\":{\"route\":{"
    "\"onestop_id\":\"r-9zmm-3\",\"route_short_name\":\"3\",\"route_long_name\":\"Blue\"}}}"
    "]}],\"meta\":{\"after\":7}}";

// Shaped like Transitland's /stops response (abridged).
static const char* STOPS =
    "{\"stops\":["
    "{\"onestop_id\":\"s-9zmm1-lincolnway~beach\",\"stop_name\":\"Lincoln Way & Beach Ave (EB)\","
    "\"stop_code\":\"1092\",\"location_type\":0,\"geometry\":{\"type\":\"Point\",\"coordinates\":[-93.6514,42.0229]},"
    "\"parent\":null,\"route_stops\":[{\"route\":{\"route_short_name\":\"1\"}},{\"route\":{\"route_short_name\":"
    "\"1\"}},{\"route\":{\"route_short_name\":\"11\"}},{\"route\":{\"route_short_name\":\"23\"}}],"
    "\"feed_version\":{\"feed\":{\"onestop_id\":\"f-9zmm-cyride\"}}},"
    "{\"onestop_id\":\"s-9zmm2-memorialunion\",\"stop_name\":\"Memorial Union \\u00e9\",\"stop_code\":null,"
    "\"geometry\":{\"coordinates\":[-93.6458,42.0237]}}"
    "],\"meta\":{}}";

int main() {
  // ---- JsonStream: paths and events
  {
    Recorder r;
    jsonstream::Parser p(r);
    p.feed("{\"a\":[1,{\"b\":\"x\\\"y\",\"c\":[true,null]}],\"d\":{\"e\":-2.5e3}, \"f\":[] }");
    CHECK(p.done() && !p.failed());
    std::vector<std::string> want = {"{ ",          "[ a",        "V a=1",        "{ a",     "V a.b=\"x\"y\"",
                                     "[ a.c",       "V a.c=true", "V a.c=null",   "] a.c",   "} a",
                                     "] a",         "{ d",        "V d.e=-2.5e3", "} d",     "[ f",
                                     "] f",         "} "};
    CHECK(r.events == want);
    if (r.events != want)
      for (auto& e : r.events) printf("     %s\n", e.c_str());
  }
  {
    Recorder r;
    jsonstream::Parser p(r);
    p.feed("{\"s\":\"caf\\u00e9 \\ud83d\\ude00!\\n\"}");  // escapes; emoji dropped
    CHECK(p.done());
    CHECK(r.events.size() == 3 && r.events[1] == "V s=\"caf\xc3\xa9 ! \"");
  }
  {
    Recorder r;
    jsonstream::Parser p(r);
    p.feed("{\"a\":1,}");  // trailing comma
    CHECK(p.failed());
    Recorder r2;
    jsonstream::Parser p2(r2);
    p2.feed("{\"a\":[1}");  // mismatched bracket
    CHECK(p2.failed());
    Recorder r3;
    jsonstream::Parser p3(r3);
    p3.feed("<html>502</html>");
    CHECK(p3.failed());
  }
  {
    // chunked feeding gives the same events
    Recorder whole, bytes;
    jsonstream::Parser a(whole), b(bytes);
    a.feed(DEPARTURES);
    feedChunks(b, DEPARTURES, 1);
    CHECK(a.done() && b.done());
    CHECK(whole.events == bytes.events);
    Recorder seven;
    jsonstream::Parser c(seven);
    feedChunks(c, DEPARTURES, 7);
    CHECK(whole.events == seven.events);
  }

  // ---- time parsing
  {
    int64_t t;
    CHECK(transit::parseIsoTime("1970-01-01T00:00:00Z", t) && t == 0);
    CHECK(transit::parseIsoTime("2026-10-18T14:04:30Z", t) && t == 1792332270);
    CHECK(transit::parseIsoTime("2026-10-18T09:04:30-05:00", t) && t == 1792332270);
    CHECK(transit::parseIsoTime("2026-10-18T09:04:30.250-0500", t) && t == 1792332270);
    CHECK(transit::parseIsoTime("2026-10-18T19:34:30+05:30", t) && t == 1792332270);
    CHECK(transit::parseIsoTime("2024-02-29T12:00:00Z", t) && t == 1709208000);
    CHECK(!transit::parseIsoTime("09:04:30", t));
    CHECK(!transit::parseIsoTime("2026-10-18T09:04:30", t));  // no zone: unknown
    CHECK(!transit::parseIsoTime("", t));
    int64_t s;
    CHECK(transit::parseClock("25:10:05", s) && s == 25 * 3600 + 10 * 60 + 5);  // after midnight
    int64_t d;
    CHECK(transit::parseDate("2026-10-18", d) && d == 20744);
    CHECK(!transit::parseDate("2026-13-01", d));
  }

  // ---- route names
  {
    char label[transit::LABEL_LEN + 1];
    transit::Route r;
    strcpy(r.id, "r-1");
    strcpy(r.shortName, "1");
    strcpy(r.longName, "Red");
    transit::routeLabel(r, label, sizeof(label));
    CHECK(streq(label, "1 RED"));
    strcpy(r.longName, "1 Red West");
    transit::routeLabel(r, label, sizeof(label));
    CHECK(streq(label, "1 RED WEST"));
    strcpy(r.longName, "10 Pink");  // "1" isn't a prefix word of "10 PINK"
    transit::routeLabel(r, label, sizeof(label));
    CHECK(streq(label, "1 10 PINK"));
    strcpy(r.shortName, "");
    strcpy(r.longName, "Moonlight Express Caf\xc3\xa9 Shuttle");
    transit::routeLabel(r, label, sizeof(label));
    CHECK(streq(label, "MOONLIGHT EXPRESS CA"));  // capped at 20 characters
    strcpy(r.shortName, "23");
    strcpy(r.longName, "");
    transit::routeLabel(r, label, sizeof(label));
    CHECK(streq(label, "23"));
    strcpy(r.shortName, "");
    transit::routeLabel(r, label, sizeof(label));
    CHECK(streq(label, "r-1"));
  }

  // ---- route lists: dedupe, sort, save and load
  {
    transit::RouteList list;
    transit::Route a, b, c, d;
    strcpy(a.id, "r-23"); strcpy(a.shortName, "23"); strcpy(a.longName, "Orange");
    strcpy(b.id, "r-2");  strcpy(b.shortName, "2");  strcpy(b.longName, "Green");
    strcpy(c.id, "r-x");  strcpy(c.shortName, "");   strcpy(c.longName, "Moonlight");
    strcpy(d.id, "r-10"); strcpy(d.shortName, "10"); strcpy(d.longName, "Pink");
    CHECK(list.add(a) == 0 && list.add(b) == 1 && list.add(c) == 2 && list.add(d) == 3);
    CHECK(list.add(b) == 1 && list.count == 4);
    transit::sortRoutes(list);
    CHECK(streq(list.items[0].id, "r-2") && streq(list.items[1].id, "r-10") && streq(list.items[2].id, "r-23") &&
          streq(list.items[3].id, "r-x"));
    char saved[512];
    transit::serialize(list, saved, sizeof(saved));
    CHECK(streq(saved, "r-2\t2\tGreen\nr-10\t10\tPink\nr-23\t23\tOrange\nr-x\t\tMoonlight\n"));
    transit::RouteList back;
    transit::deserialize(saved, back);
    CHECK(back.count == 4 && streq(back.items[3].id, "r-x") && streq(back.items[3].shortName, "") &&
          streq(back.items[3].longName, "Moonlight") && streq(back.items[1].longName, "Pink"));
    transit::deserialize("r-9\t9", back);  // missing field, no newline
    CHECK(back.count == 1 && streq(back.items[0].shortName, "9") && streq(back.items[0].longName, ""));
    transit::deserialize("", back);
    CHECK(back.count == 0);
    char tiny[20];
    transit::serialize(list, tiny, sizeof(tiny));
    CHECK(streq(tiny, "r-2\t2\tGreen\n"));  // only whole lines
  }

  // ---- URLs
  {
    char url[400];
    CHECK(transit::stopSearchUrl(url, sizeof(url), "Lincoln Way & Beach", 42.0308, -93.6319, 10000, 20));
    CHECK(streq(url, "https://transit.land/api/v2/rest/stops?search=Lincoln%20Way%20%26%20Beach"
                     "&lat=42.03080&lon=-93.63190&radius=10000&limit=20"));
    CHECK(transit::stopSearchUrl(url, sizeof(url), "MU", NAN, NAN, 0, 5));
    CHECK(streq(url, "https://transit.land/api/v2/rest/stops?search=MU&limit=5"));
    CHECK(transit::departuresUrl(url, sizeof(url), "s-9zmm1-lincolnway~beach", 7200, 60));
    CHECK(streq(url, "https://transit.land/api/v2/rest/stops/s-9zmm1-lincolnway~beach/departures?next=7200&limit=60"));
    CHECK(!transit::departuresUrl(url, 40, "s-9zmm1-lincolnway~beach", 7200, 60));
    CHECK(strstr(url, "apikey") == nullptr);
  }

  // ---- stop search results
  {
    transit::Stop stops[transit::MAX_STOPS];
    transit::StopReader reader(stops, transit::MAX_STOPS);
    jsonstream::Parser p(reader);
    feedChunks(p, STOPS, 5);
    CHECK(p.done());
    CHECK(reader.count == 2);
    CHECK(streq(stops[0].id, "s-9zmm1-lincolnway~beach"));
    CHECK(streq(stops[0].name, "Lincoln Way & Beach Ave (EB)"));
    CHECK(streq(stops[0].code, "1092"));
    CHECK(streq(stops[0].routes, "1 11 23"));
    CHECK(stops[0].lat > 42.02 && stops[0].lat < 42.03 && stops[0].lon < -93.65);
    CHECK(streq(stops[1].name, "Memorial Union \xc3\xa9") && streq(stops[1].code, "") && streq(stops[1].routes, ""));
    transit::Stop one[1];
    transit::StopReader capped(one, 1);
    jsonstream::Parser p2(capped);
    p2.feed(STOPS);
    CHECK(capped.count == 1);
  }

  // ---- departures
  {
    static transit::Board board;
    transit::DepartureReader reader(board);
    jsonstream::Parser p(reader);
    feedChunks(p, DEPARTURES, 3);
    CHECK(p.done());
    CHECK(board.stopFound);
    CHECK(streq(board.stopName, "Lincoln Way & Beach Ave"));
    CHECK(streq(board.stopCode, "1092"));
    CHECK(board.count == 6);  // canceled Orange dropped
    CHECK(board.routes.count == 5);
    CHECK(board.routes.find("r-9zmm-23") < 0);
    CHECK(board.deps[0].realtime && board.deps[0].time == 1792332270);  // estimated beats scheduled
    CHECK(!board.deps[1].realtime && board.deps[1].time == 1792332720);
    CHECK(board.deps[3].local);
    CHECK(board.deps[5].time == 1792332020);  // from scheduled_local with offset

    // The sketch converts local times with the sign's time zone (CDT, -5 h).
    transit::resolveLocalTimes(board, [](int64_t wall) { return wall + 5 * 3600; });
    CHECK(!board.deps[3].local && board.deps[3].time == 1792333800);  // 14:30Z

    const int64_t now = 1792332000;  // 2026-10-18 14:00:00Z = 9:00 CDT
    CHECK(transit::nextMinutes(board, "r-9zmm-1", now) == 4);
    CHECK(transit::nextMinutes(board, "r-9zmm-2", now) == 12);
    CHECK(transit::nextMinutes(board, "r-9zmm-6", now) == 30);
    CHECK(transit::nextMinutes(board, "r-9zmm-5", now) == -1);   // left at 13:58
    CHECK(transit::nextMinutes(board, "r-9zmm-3", now) == 0);    // 20 s away
    CHECK(transit::nextMinutes(board, "r-9zmm-23", now) == -1);  // canceled
    CHECK(transit::nextMinutes(board, "r-9zmm-1", now + 5 * 60) == 15);  // next Red after the first

    transit::RouteList chosen;
    transit::Route r;
    strcpy(r.id, "r-9zmm-1"); strcpy(r.shortName, "1"); strcpy(r.longName, "Red");
    chosen.add(r);
    char text[200];
    transit::formatArrivals(board, chosen, now, text, sizeof(text));
    CHECK(streq(text, "1 RED 4 MIN"));

    strcpy(r.id, "r-9zmm-2"); strcpy(r.shortName, "2"); strcpy(r.longName, "Green");
    chosen.add(r);
    strcpy(r.id, "r-9zmm-3"); strcpy(r.shortName, "3"); strcpy(r.longName, "Blue");
    chosen.add(r);
    strcpy(r.id, "r-9zmm-23"); strcpy(r.shortName, "23"); strcpy(r.longName, "Orange");
    chosen.add(r);
    transit::formatArrivals(board, chosen, now, text, sizeof(text));
    CHECK(streq(text, "1 RED 4 MIN   2 GREEN 12 MIN   3 BLUE NOW   23 ORANGE NO BUS"));
    char small[24];
    transit::formatArrivals(board, chosen, now, small, sizeof(small));
    CHECK(streq(small, "1 RED 4 MIN"));  // whole entries only
  }

  // ---- a stop that doesn't exist: {"stops":[]}
  {
    static transit::Board board;
    transit::DepartureReader reader(board);
    jsonstream::Parser p(reader);
    p.feed("{\"stops\":[]}");
    CHECK(p.done() && !board.stopFound && board.count == 0);
  }

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
