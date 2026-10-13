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

// Builds a response shaped like Open-Meteo's for the request made by
// weather::forecastUrl(). The "*_units" objects repeat every key with string
// values; the parser must read from "current"/"daily" instead.
static std::string num(double v) {
  if (isnan(v)) return "null";
  char b[32];
  snprintf(b, sizeof(b), "%g", v);
  return b;
}

// `clouds` is the hourly cloud_cover array contents, e.g. "60,80,null";
// nullptr leaves the hourly section out.
static std::string forecastJson(const char* now, int code, double hi, double lo, double rainSum,
                                double showersSum, double snowSum, double chance,
                                const char* clouds = nullptr) {
  std::string hourly;
  if (clouds) {
    hourly = std::string("\"hourly_units\":{\"time\":\"iso8601\",\"cloud_cover\":\"%\"},") +
             "\"hourly\":{\"time\":[\"2026-10-12T00:00\",\"2026-10-12T01:00\"],\"cloud_cover\":[" +
             clouds + "]},";
  }
  return std::string("{\"latitude\":42.03,\"longitude\":-93.63,\"timezone\":\"America/Chicago\",") + hourly +
         "\"current_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"\\u00b0F\"}," +
         "\"current\":{\"time\":\"2026-10-12T17:00\",\"interval\":900,\"temperature_2m\":" + now + "}," +
         "\"daily_units\":{\"time\":\"iso8601\",\"weather_code\":\"wmo code\",\"temperature_2m_max\":\"\\u00b0F\"," +
         "\"rain_sum\":\"inch\",\"showers_sum\":\"inch\",\"snowfall_sum\":\"inch\"," +
         "\"precipitation_probability_max\":\"%\"}," +
         "\"daily\":{\"time\":[\"2026-10-12\"],\"weather_code\":[" + num(code) + "],\"temperature_2m_max\":[" +
         num(hi) + "],\"temperature_2m_min\":[" + num(lo) + "],\"rain_sum\":[" + num(rainSum) +
         "],\"showers_sum\":[" + num(showersSum) + "],\"snowfall_sum\":[" + num(snowSum) +
         "],\"precipitation_probability_max\":[" + num(chance) + "]}}";
}

static size_t pagesFor(const std::string& json, bool fahrenheit, const char* label,
                       char pages[][weather::PAGE_LEN]) {
  weather::Forecast f;
  if (!weather::parseForecast(json.c_str(), f)) return 0;
  return weather::formatPages(f, label, fahrenheit, pages, weather::MAX_PAGES);
}

// The screen after the temperatures (precipitation or clouds), "" if none.
static std::string lastScreenFor(int code, double rain, double showers, double snow, double chance,
                                 const char* clouds = nullptr) {
  char pages[weather::MAX_PAGES][weather::PAGE_LEN];
  size_t n = pagesFor(forecastJson("50", code, 55, 45, rain, showers, snow, chance, clouds), true, "", pages);
  for (size_t i = 0; i + 1 < n; ++i)
    if (strncmp(pages[i], "NOW ", 4) == 0) return pages[i + 1];
  return "";
}

static std::string precipFor(int code, double rain, double showers, double snow, double chance) {
  return lastScreenFor(code, rain, showers, snow, chance);
}

static std::string cloudsFor(int cover) {
  char clouds[16];
  snprintf(clouds, sizeof(clouds), "%d", cover);
  return lastScreenFor(3, 0, 0, 0, 0, clouds);
}

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
  char pages[weather::MAX_PAGES][weather::PAGE_LEN];
  size_t n;

  // ---- full cycle on a rainy day
  {
    std::string json = forecastJson("61.6", 61, 66.4, 50.6, 0.25, 0.05, 0, 40);
    weather::Forecast f;
    CHECK(weather::parseForecast(json.c_str(), f));
    CHECK(fabs(f.tempNow - 61.6) < 1e-9);
    CHECK(f.code == 61);
    CHECK(fabs(f.rainInches - 0.30) < 1e-9);
    CHECK(f.snowInches == 0);
    CHECK(f.precipChance == 40);

    n = pagesFor(json, true, "AMES", pages);
    CHECK(n == 4);
    CHECK(strcmp(pages[0], "AMES") == 0);
    CHECK(strcmp(pages[1], "LIGHT RAIN") == 0);
    CHECK(strcmp(pages[2], "NOW 62F  HIGH 66F  LOW 51F") == 0);
    CHECK(strcmp(pages[3], "RAIN 0.30 IN  40% CHANCE") == 0);
  }

  // ---- type from forecast amounts, chance is the overall daily chance
  CHECK(precipFor(73, 0, 0, 2.4, 75) == "SNOW 2.4 IN  75% CHANCE");
  CHECK(precipFor(71, 0.1, 0, 0.5, 60) == "RAIN 0.10 IN + SNOW 0.5 IN  60% CHANCE");
  CHECK(precipFor(66, 0.05, 0, 0, 30) == "FREEZING RAIN 0.05 IN  30% CHANCE");
  CHECK(precipFor(80, 0, 0.4, 0, 55) == "RAIN 0.40 IN  55% CHANCE");  // showers count as rain

  // ---- no amounts forecast: the weather code decides the type
  CHECK(precipFor(71, 0, 0, 0, 20) == "SNOW <0.1 IN  20% CHANCE");
  CHECK(precipFor(51, 0, 0, 0, 20) == "RAIN <0.01 IN  20% CHANCE");
  CHECK(precipFor(3, 0, 0, 0, 20) == "PRECIP <0.01 IN  20% CHANCE");

  // ---- only shown ABOVE 5%
  CHECK(precipFor(61, 0.01, 0, 0, 5) == "");
  CHECK(precipFor(61, 0.01, 0, 0, 6) == "RAIN 0.01 IN  6% CHANCE");
  CHECK(precipFor(61, 0.2, 0, 0, NAN) == "");  // unknown chance: hidden

  // ---- 5% or less: how cloudy the sky is instead
  {
    std::string json = forecastJson("48", 2, 52, 40, 0, 0, 0, 5, "60,80,null,72");
    weather::Forecast f;
    CHECK(weather::parseForecast(json.c_str(), f));
    CHECK(f.cloudCover == 71);  // (60 + 80 + 72) / 3, null skipped
    n = pagesFor(json, true, "AMES", pages);
    CHECK(n == 4);
    CHECK(strcmp(pages[2], "NOW 48F  HIGH 52F  LOW 40F") == 0);
    CHECK(strcmp(pages[3], "MOSTLY CLOUDY  71% CLOUD COVER") == 0);
  }
  CHECK(lastScreenFor(61, 0.2, 0, 0, 6, "90") == "RAIN 0.20 IN  6% CHANCE");  // rain wins above 5%
  CHECK(lastScreenFor(1, 0, 0, 0, NAN, "20") == "MOSTLY CLEAR  20% CLOUD COVER");  // unknown chance
  CHECK(lastScreenFor(1, 0, 0, 0, 0, "null,null") == "");  // no cloud data: no screen
  CHECK(lastScreenFor(1, 0, 0, 0, 0, "") == "");

  // ---- NWS sky-condition bands
  CHECK(cloudsFor(0) == "CLEAR  0% CLOUD COVER");
  CHECK(cloudsFor(12) == "CLEAR  12% CLOUD COVER");
  CHECK(cloudsFor(13) == "MOSTLY CLEAR  13% CLOUD COVER");
  CHECK(cloudsFor(37) == "MOSTLY CLEAR  37% CLOUD COVER");
  CHECK(cloudsFor(38) == "PARTLY CLOUDY  38% CLOUD COVER");
  CHECK(cloudsFor(62) == "PARTLY CLOUDY  62% CLOUD COVER");
  CHECK(cloudsFor(63) == "MOSTLY CLOUDY  63% CLOUD COVER");
  CHECK(cloudsFor(87) == "MOSTLY CLOUDY  87% CLOUD COVER");
  CHECK(cloudsFor(88) == "CLOUDY  88% CLOUD COVER");
  CHECK(cloudsFor(100) == "CLOUDY  100% CLOUD COVER");

  // ---- longest screen fits
  CHECK(precipFor(67, 12.34, 0, 25.5, 100) == "FREEZING RAIN 12.34 IN + SNOW 25.5 IN  100% CHANCE");

  // ---- Celsius temperatures; amounts stay in inches
  {
    std::string json = forecastJson("-0.4", 73, 2, -3, 0, 0, 1.2, 50);
    n = pagesFor(json, false, "", pages);
    CHECK(n == 3);
    CHECK(strcmp(pages[1], "NOW 0C  HIGH 2C  LOW -3C") == 0);
    CHECK(strcmp(pages[2], "SNOW 1.2 IN  50% CHANCE") == 0);
  }

  // ---- nulls: missing current temp and precipitation are skipped
  {
    std::string json = forecastJson("null", 95, 30.2, 21.9, NAN, NAN, NAN, NAN);
    n = pagesFor(json, false, "", pages);
    CHECK(n == 2);
    CHECK(strcmp(pages[0], "THUNDERSTORMS") == 0);
    CHECK(strcmp(pages[1], "HIGH 30C  LOW 22C") == 0);
  }

  // ---- bad responses
  {
    weather::Forecast f;
    CHECK(!weather::parseForecast("{\"error\":true,\"reason\":\"bad\"}", f));
    CHECK(!weather::parseForecast("", f));
    CHECK(!weather::parseForecast("{\"daily\":{\"temperature_2m_max\":[1", f));
  }

  // ---- page limit respected
  {
    weather::Forecast f;
    weather::parseForecast(forecastJson("40", 61, 45, 35, 0.1, 0, 0, 50).c_str(), f);
    CHECK(weather::formatPages(f, "AMES", true, pages, 2) == 2);
  }

  // ---- fitting held screens to a 13-character display
  {
    char sc[weather::MAX_SCREENS][weather::PAGE_LEN];
    size_t k = weather::fitScreens("AMES", 13, sc, 0, weather::MAX_SCREENS);
    CHECK(k == 1 && strcmp(sc[0], "AMES") == 0);
    k = weather::fitScreens("PARTLY CLOUDY", 13, sc, k, weather::MAX_SCREENS);  // exactly 13
    CHECK(k == 2 && strcmp(sc[1], "PARTLY CLOUDY") == 0);
    k = weather::fitScreens("NOW 62F  HIGH 66F  LOW 51F", 13, sc, k, weather::MAX_SCREENS);
    CHECK(k == 5);
    CHECK(strcmp(sc[2], "NOW 62F") == 0 && strcmp(sc[3], "HIGH 66F") == 0 && strcmp(sc[4], "LOW 51F") == 0);
    k = weather::fitScreens("MOSTLY CLOUDY  70% CLOUD COVER", 13, sc, k, weather::MAX_SCREENS);
    CHECK(k == 8);
    CHECK(strcmp(sc[5], "MOSTLY CLOUDY") == 0 && strcmp(sc[6], "70% CLOUD") == 0 && strcmp(sc[7], "COVER") == 0);
    k = weather::fitScreens("RAIN 0.10 IN + SNOW 0.5 IN  60% CHANCE", 13, sc, 0, weather::MAX_SCREENS);
    CHECK(k == 3);
    CHECK(strcmp(sc[0], "RAIN 0.10 IN") == 0 && strcmp(sc[1], "+ SNOW 0.5 IN") == 0);
    CHECK(strcmp(sc[2], "60% CHANCE") == 0);
    k = weather::fitScreens("FREEZING RAIN 0.05 IN  30% CHANCE", 13, sc, 0, weather::MAX_SCREENS);
    for (size_t i = 0; i < k; ++i) CHECK(strlen(sc[i]) <= 13);
    k = weather::fitScreens("ABCDEFGHIJKLMNOPQRS", 13, sc, 0, weather::MAX_SCREENS);  // one long word: cut
    CHECK(k >= 1 && strlen(sc[0]) == 13);
    k = weather::fitScreens("A  B  C", 13, sc, 0, 2);  // respects maxOut
    CHECK(k == 2);
  }

  // ---- geocoding
  weather::Place place;
  CHECK(weather::parsePlace(GEOCODE, place));
  CHECK(strcmp(place.name, "MILWAUKEE") == 0);
  CHECK(fabs(place.latitude - 43.0389) < 1e-9);
  CHECK(fabs(place.longitude - -87.90647) < 1e-9);

  CHECK(weather::parsePlace(GEOCODE_ACCENTS, place));
  CHECK(strcmp(place.name, "MONTREAL \"QC\"") == 0);
  CHECK(fabs(place.latitude - 45.50884) < 1e-9);

  CHECK(!weather::parsePlace(GEOCODE_EMPTY, place));

  // ---- URLs
  char url[512];
  weather::geocodeUrl(url, sizeof(url), "New York");
  CHECK(strstr(url, "&name=New%20York") != nullptr);
  CHECK(weather::forecastUrl(url, sizeof(url), -42.0308, -193.6319, true));  // longest case
  CHECK(!weather::forecastUrl(url, 300, -42.0308, -193.6319, true));         // too small
  CHECK(weather::forecastUrl(url, sizeof(url), 43.0389, -87.90647, true));
  CHECK(strstr(url, "latitude=43.0389&longitude=-87.9065") != nullptr);
  CHECK(strstr(url, "temperature_unit=fahrenheit") != nullptr);
  CHECK(strstr(url, "precipitation_unit=inch") != nullptr);
  CHECK(strstr(url, "rain_sum,showers_sum,snowfall_sum,precipitation_probability_max") != nullptr);
  CHECK(strstr(url, "&hourly=cloud_cover") != nullptr);
  CHECK(strstr(url, "humidity") == nullptr);
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
