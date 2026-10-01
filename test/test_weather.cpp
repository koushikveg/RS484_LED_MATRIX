// Host-side tests for Weather.h and multi-page packets in AlphaSign.h.
//
//   g++ -std=c++11 -Wall -Wextra -I firmware/led_sign test/test_weather.cpp -o test_weather && ./test_weather

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "AlphaSign.h"
#include "Weather.h"

static int failures = 0;

#define CHECK(cond)                                         \
  do {                                                      \
    if (cond) {                                             \
      printf("ok   %s\n", #cond);                           \
    } else {                                                \
      ++failures;                                           \
      printf("FAIL %s  (line %d)\n", #cond, __LINE__);      \
    }                                                       \
  } while (0)

// Shape of a real Open-Meteo forecast response for the request built by
// weather::forecastUrl(). Note the *_units objects repeat every key with
// string values; the parser must read from "current"/"daily" instead.
static const char* FORECAST =
    "{\"latitude\":43.04,\"longitude\":-87.91,\"generationtime_ms\":0.05,"
    "\"utc_offset_seconds\":-18000,\"timezone\":\"America/Chicago\","
    "\"timezone_abbreviation\":\"GMT-5\",\"elevation\":188.0,"
    "\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\","
    "\"temperature_2m\":\"\\u00b0F\",\"relative_humidity_2m\":\"%\"},"
    "\"current\":{\"time\":\"2026-10-01T17:00\",\"interval\":900,"
    "\"temperature_2m\":61.6,\"relative_humidity_2m\":72},"
    "\"daily_units\":{\"time\":\"iso8601\",\"weather_code\":\"wmo code\","
    "\"temperature_2m_max\":\"\\u00b0F\",\"temperature_2m_min\":\"\\u00b0F\","
    "\"precipitation_probability_max\":\"%\"},"
    "\"daily\":{\"time\":[\"2026-10-01\"],\"weather_code\":[2],"
    "\"temperature_2m_max\":[66.4],\"temperature_2m_min\":[-0.6],"
    "\"precipitation_probability_max\":[20]}}";

static const char* FORECAST_NULLS =
    "{\"current\":{\"time\":\"2026-10-01T17:00\",\"interval\":900,"
    "\"temperature_2m\":null,\"relative_humidity_2m\":null},"
    "\"daily\":{\"time\":[\"2026-10-01\"],\"weather_code\":[95],"
    "\"temperature_2m_max\":[30.2],\"temperature_2m_min\":[21.9],"
    "\"precipitation_probability_max\":[null]}}";

static const char* GEOCODE =
    "{\"results\":[{\"id\":5263045,\"name\":\"Milwaukee\",\"latitude\":43.0389,"
    "\"longitude\":-87.90647,\"elevation\":188.0,\"feature_code\":\"PPLA2\","
    "\"country_code\":\"US\",\"timezone\":\"America/Chicago\","
    "\"country\":\"United States\",\"admin1\":\"Wisconsin\"},"
    "{\"id\":1,\"name\":\"Other\",\"latitude\":1.0,\"longitude\":2.0}],"
    "\"generationtime_ms\":0.6}";

static const char* GEOCODE_ACCENTS =
    "{\"results\":[{\"name\":\"Montr\\u00e9al \\\"QC\\\"\",\"latitude\":45.50884,"
    "\"longitude\":-73.58781}]}";

static const char* GEOCODE_EMPTY = "{\"generationtime_ms\":0.3}";

int main() {
  // ---- forecast parsing
  weather::Forecast f;
  CHECK(weather::parseForecast(FORECAST, f));
  CHECK(fabs(f.tempNow - 61.6) < 1e-9);
  CHECK(f.humidity == 72);
  CHECK(f.code == 2);
  CHECK(fabs(f.tempHigh - 66.4) < 1e-9);
  CHECK(fabs(f.tempLow - -0.6) < 1e-9);
  CHECK(f.precipChance == 20);

  CHECK(weather::parseForecast(FORECAST_NULLS, f));
  CHECK(isnan(f.tempNow));
  CHECK(f.humidity == -1);
  CHECK(f.precipChance == -1);
  CHECK(f.code == 95);

  CHECK(!weather::parseForecast("{\"error\":true,\"reason\":\"bad\"}", f));
  CHECK(!weather::parseForecast("", f));
  CHECK(!weather::parseForecast("{\"daily\":{\"temperature_2m_max\":[1", f));

  // ---- page formatting
  weather::parseForecast(FORECAST, f);
  char pages[weather::MAX_PAGES][weather::PAGE_LEN];
  size_t n = weather::formatPages(f, "MILWAUKEE", true, pages, weather::MAX_PAGES);
  CHECK(n == 7);
  CHECK(strcmp(pages[0], "MILWAUKEE") == 0);
  CHECK(strcmp(pages[1], "PARTLY CLOUDY") == 0);
  CHECK(strcmp(pages[2], "NOW 62F") == 0);
  CHECK(strcmp(pages[3], "HIGH 66F") == 0);
  CHECK(strcmp(pages[4], "LOW -1F") == 0);
  CHECK(strcmp(pages[5], "HUMIDITY 72%") == 0);
  CHECK(strcmp(pages[6], "RAIN 20%") == 0);

  weather::parseForecast(FORECAST_NULLS, f);
  n = weather::formatPages(f, "", false, pages, weather::MAX_PAGES);
  CHECK(n == 3);
  CHECK(strcmp(pages[0], "THUNDERSTORMS") == 0);
  CHECK(strcmp(pages[1], "HIGH 30C") == 0);
  CHECK(strcmp(pages[2], "LOW 22C") == 0);

  n = weather::formatPages(f, "X", true, pages, 2);
  CHECK(n == 2);

  // ---- geocoding
  weather::Place place;
  CHECK(weather::parsePlace(GEOCODE, place));
  CHECK(strcmp(place.name, "MILWAUKEE") == 0);
  CHECK(fabs(place.latitude - 43.0389) < 1e-9);
  CHECK(fabs(place.longitude - -87.90647) < 1e-9);

  CHECK(weather::parsePlace(GEOCODE_ACCENTS, place));
  CHECK(strcmp(place.name, "MONTRAL \"QC\"") == 0);
  CHECK(fabs(place.latitude - 45.50884) < 1e-9);

  CHECK(!weather::parsePlace(GEOCODE_EMPTY, place));

  // ---- URLs
  char url[300];
  weather::geocodeUrl(url, sizeof(url), "New York");
  CHECK(strstr(url, "&name=New%20York") != nullptr);
  weather::forecastUrl(url, sizeof(url), 43.0389, -87.90647, true);
  CHECK(strstr(url, "latitude=43.0389&longitude=-87.9065") != nullptr);
  CHECK(strstr(url, "temperature_unit=fahrenheit") != nullptr);
  weather::forecastUrl(url, sizeof(url), 1, 2, false);
  CHECK(strstr(url, "temperature_unit") == nullptr);

  // ---- multi-page Alpha packet
  {
    alpha::TextOptions o;
    alpha::Page p[] = {{"HIGH 66F", alpha::findMode("hold")},
                       {"PARTLY CLOUDY", alpha::findMode("rotate")}};
    uint8_t buf[128];
    size_t len = alpha::buildWriteTextPages(buf, sizeof(buf), o, p, 2);
    std::string got((const char*)buf, len);
    std::string expected = std::string("\x00\x00\x00\x00\x00\x01", 6) +
                           "Z00\x02" "AA" "\x1B\x20" "bHIGH 66F" "\x1B\x20" "aPARTLY CLOUDY\x04";
    CHECK(got == expected);
  }

  // Priority file: pages stop once 125 bytes of data are used.
  {
    alpha::TextOptions o;
    o.fileLabel = alpha::FILE_PRIORITY;
    std::string big(100, 'x');
    alpha::Page p[] = {{big.c_str(), alpha::findMode("hold")},
                       {big.c_str(), alpha::findMode("hold")}};
    uint8_t buf[400];
    size_t len = alpha::buildWriteTextPages(buf, sizeof(buf), o, p, 2);
    // 5 NUL + SOH + "Z00" + STX + "A0" = 12 header bytes, EOT = 1
    CHECK(len - 12 - 1 == alpha::PRIORITY_MAX_BYTES);
  }

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
