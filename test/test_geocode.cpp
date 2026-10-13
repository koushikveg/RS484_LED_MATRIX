// Host-side tests for Geocode.h ("City, State, Country" location lookup).
//
//   g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_geocode.cpp -o test_geocode && ./test_geocode

#include <stdio.h>
#include <string.h>

#include "Weather.h"
#include "Geocode.h"

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

static const char* code(const char* typed) {
  char norm[geo::FIELD_LEN];
  geo::normalize(typed, norm, sizeof(norm));
  return geo::countryCode(norm);
}

// Abridged Open-Meteo responses.
static const char* AMES =
    "{\"results\":["
    "{\"id\":1,\"name\":\"Ames\",\"latitude\":42.03471,\"longitude\":-93.61994,\"country_code\":\"US\","
    "\"admin1\":\"Iowa\",\"country\":\"United States\"},"
    "{\"id\":2,\"name\":\"Ames\",\"latitude\":32.83,\"longitude\":-96.6,\"country_code\":\"US\","
    "\"admin1\":\"Texas\",\"country\":\"United States\"},"
    "{\"id\":3,\"name\":\"Ames\",\"latitude\":50.54,\"longitude\":2.41,\"country_code\":\"FR\","
    "\"admin1\":\"Hauts-de-France\",\"country\":\"France\"}"
    "]}";

static const char* PARIS =
    "{\"results\":["
    "{\"name\":\"Paris\",\"latitude\":48.85341,\"longitude\":2.3488,\"country_code\":\"FR\","
    "\"admin1\":\"\\u00cele-de-France\",\"country\":\"France\"},"
    "{\"name\":\"Paris\",\"latitude\":33.66094,\"longitude\":-95.55551,\"country_code\":\"US\","
    "\"admin1\":\"Texas\",\"country\":\"United States\"}"
    "]}";

static const char* ATLANTA =
    "{\"results\":["
    "{\"name\":\"Atlanta\",\"latitude\":33.749,\"longitude\":-84.38798,\"country_code\":\"US\","
    "\"admin1\":\"Georgia\",\"country\":\"United States\"}"
    "]}";

// A place name found in both Georgias (US state first, as Open-Meteo ranks it).
static const char* GEORGIA =
    "{\"results\":["
    "{\"name\":\"Tbilisi\",\"latitude\":31.0,\"longitude\":-83.0,\"country_code\":\"US\","
    "\"admin1\":\"Florida\",\"country\":\"United States\"},"
    "{\"name\":\"Tbilisi\",\"latitude\":41.69411,\"longitude\":44.83368,\"country_code\":\"GE\","
    "\"admin1\":\"Tbilisi\",\"country\":\"Georgia\"}"
    "]}";

static const char* ZURICH =
    "{\"results\":[{\"name\":\"Z\xc3\xbcrich\",\"latitude\":47.36667,\"longitude\":8.55,"
    "\"country_code\":\"CH\",\"admin1\":\"Zurich\",\"country\":\"Switzerland\"}]}";

static int pickFor(const char* typed, const char* json) {
  geo::Query q;
  if (!geo::parseQuery(typed, q)) return -2;
  geo::Place places[geo::MAX_RESULTS];
  size_t n = geo::parsePlaces(json, places, geo::MAX_RESULTS);
  return geo::pick(q, places, n);
}

int main() {
  // ---- normalising typed text
  {
    char out[geo::FIELD_LEN];
    geo::normalize("  u.s.a. ", out, sizeof(out));
    CHECK(streq(out, "USA"));
    geo::normalize("United   States-of_America", out, sizeof(out));
    CHECK(streq(out, "UNITED STATES OF AMERICA"));
    geo::normalize("C\xc3\xb4te d'Ivoire", out, sizeof(out));
    CHECK(streq(out, "COTE DIVOIRE"));
  }

  // ---- every spelling of the United States
  CHECK(streq(code("US"), "US"));
  CHECK(streq(code("us"), "US"));
  CHECK(streq(code("USA"), "US"));
  CHECK(streq(code("U.S."), "US"));
  CHECK(streq(code("U.S.A."), "US"));
  CHECK(streq(code("United States"), "US"));
  CHECK(streq(code("united states of america"), "US"));
  CHECK(streq(code("UK"), "GB"));
  CHECK(streq(code("Great Britain"), "GB"));
  CHECK(streq(code("Canada"), "CA"));
  CHECK(streq(code("france"), "FR"));
  CHECK(code("Narnia") == nullptr);

  // ---- splitting the query
  {
    geo::Query q;
    CHECK(geo::parseQuery(" Ames , Iowa , United States ", q));
    CHECK(streq(q.city, "Ames") && streq(q.region, "IOWA") && streq(q.country, "UNITED STATES"));
    CHECK(q.second[0] == '\0');
    CHECK(geo::parseQuery("Paris, France", q));
    CHECK(streq(q.city, "Paris") && streq(q.second, "FRANCE") && q.region[0] == '\0');
    CHECK(geo::parseQuery("Ames", q));
    CHECK(streq(q.city, "Ames") && !q.second[0] && !q.region[0] && !q.country[0]);
    CHECK(!geo::parseQuery("  , Iowa", q));
    CHECK(!geo::parseQuery("", q));
  }

  // ---- parsing several results, with accents folded
  {
    geo::Place places[geo::MAX_RESULTS];
    CHECK(geo::parsePlaces(AMES, places, geo::MAX_RESULTS) == 3);
    CHECK(streq(places[0].name, "AMES") && streq(places[0].admin1, "IOWA"));
    CHECK(streq(places[0].country, "UNITED STATES") && streq(places[0].countryCode, "US"));
    CHECK(streq(places[2].admin1, "HAUTS-DE-FRANCE"));
    CHECK(geo::parsePlaces(AMES, places, 2) == 2);
    CHECK(geo::parsePlaces(PARIS, places, geo::MAX_RESULTS) == 2);
    CHECK(streq(places[0].admin1, "ILE-DE-FRANCE"));
    CHECK(geo::parsePlaces(ZURICH, places, geo::MAX_RESULTS) == 1);
    CHECK(streq(places[0].name, "ZURICH"));
    CHECK(geo::parsePlaces("{\"generationtime_ms\":0.5}", places, geo::MAX_RESULTS) == 0);

    char detail[2 * geo::FIELD_LEN + 4];
    geo::parsePlaces(AMES, places, geo::MAX_RESULTS);
    geo::describePlace(places[0], detail, sizeof(detail));
    CHECK(streq(detail, "IOWA, UNITED STATES"));
  }

  // ---- picking the right place
  CHECK(pickFor("Ames", AMES) == 0);
  CHECK(pickFor("Ames, Iowa, US", AMES) == 0);
  CHECK(pickFor("Ames, IA, USA", AMES) == 0);
  CHECK(pickFor("ames, iowa, united states of america", AMES) == 0);
  CHECK(pickFor("Ames, Texas, United States", AMES) == 1);
  CHECK(pickFor("Ames, TX", AMES) == 1);
  CHECK(pickFor("Ames, France", AMES) == 2);
  CHECK(pickFor("Ames, FR", AMES) == 2);
  CHECK(pickFor("Ames, Ohio, US", AMES) == -1);
  CHECK(pickFor("Ames, Iowa, Canada", AMES) == -1);
  CHECK(pickFor("Paris, France", PARIS) == 0);
  CHECK(pickFor("Paris, Texas", PARIS) == 1);
  CHECK(pickFor("Paris, TX, US", PARIS) == 1);
  CHECK(pickFor("Paris, Ile-de-France, France", PARIS) == 0);
  CHECK(pickFor("Atlanta, Georgia", ATLANTA) == 0);    // the state
  CHECK(pickFor("Atlanta, GA, USA", ATLANTA) == 0);
  CHECK(pickFor("Tbilisi, Georgia", GEORGIA) == 1);    // the country
  CHECK(pickFor("Zurich, Switzerland", ZURICH) == 0);
  CHECK(pickFor("Zurich, CH", ZURICH) == 0);

  // ---- search URL
  {
    char url[320];
    geo::Query q;
    geo::parseQuery("Ames, Iowa, USA", q);
    geo::searchUrl(url, sizeof(url), q, true);
    CHECK(strstr(url, "count=20") != nullptr);
    CHECK(strstr(url, "&countryCode=US") != nullptr);
    CHECK(strstr(url, "&name=Ames") != nullptr);
    geo::searchUrl(url, sizeof(url), q, false);
    CHECK(strstr(url, "countryCode") == nullptr);

    geo::parseQuery("Paris, France", q);
    CHECK(streq(geo::queryCountryCode(q), "FR"));
    geo::parseQuery("Atlanta, Georgia", q);
    CHECK(geo::queryCountryCode(q) == nullptr);  // state or country: don't filter
    geo::parseQuery("Ames, CA", q);
    CHECK(geo::queryCountryCode(q) == nullptr);  // California or Canada
    geo::parseQuery("Ames", q);
    CHECK(geo::queryCountryCode(q) == nullptr);

    geo::parseQuery("San Jos\xc3\xa9, Costa Rica", q);
    geo::searchUrl(url, sizeof(url), q, true);
    CHECK(strstr(url, "&name=San%20Jos%C3%A9") != nullptr);
  }

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
