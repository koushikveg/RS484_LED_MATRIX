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
// weather::forecastUrl(): "*_units" objects repeat every key with string
// values (the parser must read from "current"/"hourly"/"daily"), and hourly
// arrays have one value per hour of today.
struct Hour {
  double temp, chance, rain, showers, snow;
};

static std::string num(double v) {
  if (isnan(v)) return "null";
  char b[32];
  snprintf(b, sizeof(b), "%g", v);
  return b;
}

static std::string forecastJson(const char* now, int code, double hi, double lo, double rainSum,
                                double showersSum, double snowSum, const Hour* hours, size_t count) {
  std::string temp, chance, rain, showers, snow;
  for (size_t i = 0; i < count; ++i) {
    const char* sep = i ? "," : "";
    temp += sep + num(hours[i].temp);
    chance += sep + num(hours[i].chance);
    rain += sep + num(hours[i].rain);
    showers += sep + num(hours[i].showers);
    snow += sep + num(hours[i].snow);
  }
  return std::string("{\"latitude\":42.03,\"longitude\":-93.63,\"timezone\":\"America/Chicago\",") +
         "\"current_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"\\u00b0F\"}," +
         "\"current\":{\"time\":\"2026-10-12T17:00\",\"interval\":900,\"temperature_2m\":" + now + "}," +
         "\"hourly_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"\\u00b0F\",\"precipitation_probability\":\"%\"," +
         "\"rain\":\"inch\",\"showers\":\"inch\",\"snowfall\":\"inch\"}," +
         "\"hourly\":{\"time\":[\"2026-10-12T00:00\"],\"temperature_2m\":[" + temp +
         "],\"precipitation_probability\":[" + chance + "],\"rain\":[" + rain + "],\"showers\":[" +
         showers + "],\"snowfall\":[" + snow + "]}," +
         "\"daily_units\":{\"time\":\"iso8601\",\"weather_code\":\"wmo code\",\"temperature_2m_max\":\"\\u00b0F\"," +
         "\"rain_sum\":\"inch\",\"showers_sum\":\"inch\",\"snowfall_sum\":\"inch\"}," +
         "\"daily\":{\"time\":[\"2026-10-12\"],\"weather_code\":[" + num(code) + "],\"temperature_2m_max\":[" +
         num(hi) + "],\"temperature_2m_min\":[" + num(lo) + "],\"rain_sum\":[" + num(rainSum) +
         "],\"showers_sum\":[" + num(showersSum) + "],\"snowfall_sum\":[" + num(snowSum) + "]}}";
}

static size_t pagesFor(const std::string& json, bool fahrenheit, const char* label,
                       char pages[][weather::PAGE_LEN]) {
  weather::Forecast f;
  if (!weather::parseForecast(json.c_str(), fahrenheit, f)) return 0;
  return weather::formatPages(f, label, fahrenheit, pages, weather::MAX_PAGES);
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

  // ---- rainy afternoon: rain screen, no snow screen, no humidity
  {
    Hour h[24];
    for (int i = 0; i < 24; ++i) h[i] = {60, 0, 0, 0, 0};
    for (int i = 12; i < 18; ++i) h[i] = {58, 40, 0.04, 0.01, 0};
    std::string json = forecastJson("61.6", 61, 66.4, 50.6, 0.25, 0.05, 0, h, 24);

    weather::Forecast f;
    CHECK(weather::parseForecast(json.c_str(), true, f));
    CHECK(fabs(f.tempNow - 61.6) < 1e-9);
    CHECK(f.code == 61);
    CHECK(fabs(f.rainInches - 0.30) < 1e-9);
    CHECK(f.rainChance == 40);
    CHECK(f.snowChance == -1);

    n = pagesFor(json, true, "AMES", pages);
    CHECK(n == 4);
    CHECK(strcmp(pages[0], "AMES") == 0);
    CHECK(strcmp(pages[1], "LIGHT RAIN") == 0);
    CHECK(strcmp(pages[2], "NOW 62F  HIGH 66F  LOW 51F") == 0);
    CHECK(strcmp(pages[3], "RAIN 0.30 IN  40% CHANCE") == 0);
  }

  // ---- snow day: dry cold hours with a chance count as snow too
  {
    Hour h[24];
    for (int i = 0; i < 24; ++i) h[i] = {20, 0, 0, 0, 0};
    for (int i = 6; i < 14; ++i) h[i] = {22, 60, 0, 0, 0.3};
    h[20] = {18, 75, 0, 0, 0};  // no amount forecast, but below freezing
    std::string json = forecastJson("24", 73, 25, 15, 0, 0, 2.4, h, 24);

    n = pagesFor(json, true, "AMES", pages);
    CHECK(n == 4);
    CHECK(strcmp(pages[1], "SNOW") == 0);
    CHECK(strcmp(pages[2], "NOW 24F  HIGH 25F  LOW 15F") == 0);
    CHECK(strcmp(pages[3], "SNOW 2.4 IN  75% CHANCE") == 0);
  }

  // ---- only shown ABOVE 5%: exactly 5% is hidden, 6% is shown
  {
    Hour h[24];
    for (int i = 0; i < 24; ++i) h[i] = {50, 5, 0, 0, 0};
    std::string json = forecastJson("50", 3, 55, 45, 0, 0, 0, h, 24);
    n = pagesFor(json, true, "", pages);
    CHECK(n == 2);  // OVERCAST, temps
    CHECK(strcmp(pages[0], "OVERCAST") == 0);

    h[9].chance = 6;
    json = forecastJson("50", 3, 55, 45, 0, 0, 0, h, 24);
    n = pagesFor(json, true, "", pages);
    CHECK(n == 3);
    CHECK(strcmp(pages[2], "RAIN <0.01 IN  6% CHANCE") == 0);
  }

  // ---- mixed day: both screens, rain first; trace snow
  {
    Hour h[24];
    for (int i = 0; i < 24; ++i) h[i] = {34, 0, 0, 0, 0};
    for (int i = 0; i < 6; ++i) h[i] = {30, 30, 0, 0, 0.02};
    for (int i = 12; i < 18; ++i) h[i] = {38, 50, 0.1, 0, 0};
    std::string json = forecastJson("36", 71, 40, 29, 0.6, 0, 0.04, h, 24);
    n = pagesFor(json, true, "AMES", pages);
    CHECK(n == 5);
    CHECK(strcmp(pages[3], "RAIN 0.60 IN  50% CHANCE") == 0);
    CHECK(strcmp(pages[4], "SNOW <0.1 IN  30% CHANCE") == 0);
  }

  // ---- Celsius: freezing point is 0C; amounts stay in inches
  {
    Hour h[2] = {{-1, 30, 0, 0, 0}, {3, 10, 0, 0, 0}};
    std::string json = forecastJson("-0.4", 3, 2, -3, 0, 0, 0, h, 2);
    n = pagesFor(json, false, "", pages);
    CHECK(n == 4);
    CHECK(strcmp(pages[1], "NOW 0C  HIGH 2C  LOW -3C") == 0);
    CHECK(strcmp(pages[2], "RAIN <0.01 IN  10% CHANCE") == 0);
    CHECK(strcmp(pages[3], "SNOW <0.1 IN  30% CHANCE") == 0);
  }

  // ---- nulls: missing current temp and hourly values are skipped
  {
    Hour h[3] = {{NAN, NAN, NAN, NAN, NAN}, {70, 0, 0, 0, 0}, {70, NAN, 0, 0, 0}};
    std::string json = forecastJson("null", 95, 30.2, 21.9, NAN, NAN, NAN, h, 3);
    n = pagesFor(json, false, "", pages);
    CHECK(n == 2);
    CHECK(strcmp(pages[0], "THUNDERSTORMS") == 0);
    CHECK(strcmp(pages[1], "HIGH 30C  LOW 22C") == 0);
  }

  // ---- bad responses
  {
    weather::Forecast f;
    CHECK(!weather::parseForecast("{\"error\":true,\"reason\":\"bad\"}", true, f));
    CHECK(!weather::parseForecast("", true, f));
    CHECK(!weather::parseForecast("{\"daily\":{\"temperature_2m_max\":[1", true, f));
  }

  // ---- page limit respected
  {
    Hour h[1] = {{40, 50, 0.1, 0, 0}};
    std::string json = forecastJson("40", 61, 45, 35, 0.1, 0, 0, h, 1);
    weather::Forecast f;
    weather::parseForecast(json.c_str(), true, f);
    CHECK(weather::formatPages(f, "AMES", true, pages, 2) == 2);
  }

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
  char url[512];
  weather::geocodeUrl(url, sizeof(url), "New York");
  CHECK(strstr(url, "&name=New%20York") != nullptr);
  CHECK(weather::forecastUrl(url, sizeof(url), -42.0308, -193.6319, true));  // longest case
  CHECK(!weather::forecastUrl(url, 300, -42.0308, -193.6319, true));         // too small
  CHECK(weather::forecastUrl(url, sizeof(url), 43.0389, -87.90647, true));
  CHECK(strstr(url, "latitude=43.0389&longitude=-87.9065") != nullptr);
  CHECK(strstr(url, "temperature_unit=fahrenheit") != nullptr);
  CHECK(strstr(url, "precipitation_unit=inch") != nullptr);
  CHECK(strstr(url, "hourly=temperature_2m,precipitation_probability,rain,showers,snowfall") != nullptr);
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
