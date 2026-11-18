#include "time_location.h"

#include "board.h"
#include "config.h"
#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define TAG "Location"

namespace {
constexpr int64_t kSec = 1000LL * 1000;
constexpr const char* kNs = "chitu_loc";

// IANA time zone -> POSIX TZ rule (with daylight saving where it applies).
// Zones not listed fall back to a fixed offset (correct outside daylight saving).
struct Zone {
    const char* iana;
    const char* posix;
};
const Zone kZones[] = {
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Calcutta", "IST-5:30"},
    {"Asia/Colombo", "<+0530>-5:30"},
    {"Asia/Kathmandu", "<+0545>-5:45"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Dhaka", "<+06>-6"},
    {"Asia/Dubai", "<+04>-4"},
    {"Asia/Muscat", "<+04>-4"},
    {"Asia/Riyadh", "<+03>-3"},
    {"Asia/Qatar", "<+03>-3"},
    {"Asia/Bangkok", "<+07>-7"},
    {"Asia/Jakarta", "WIB-7"},
    {"Asia/Singapore", "<+08>-8"},
    {"Asia/Kuala_Lumpur", "<+08>-8"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Taipei", "CST-8"},
    {"Asia/Manila", "PST-8"},
    {"Asia/Seoul", "KST-9"},
    {"Asia/Tokyo", "JST-9"},
    {"Australia/Perth", "AWST-8"},
    {"Australia/Brisbane", "AEST-10"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "IST-1GMT0,M10.5.0,M3.5.0/1"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul", "<+03>-3"},
    {"Europe/Moscow", "MSK-3"},
    {"Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa/Lagos", "WAT-1"},
    {"Africa/Nairobi", "EAT-3"},
    {"Africa/Johannesburg", "SAST-2"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "CST6"},
    {"America/Sao_Paulo", "<-03>3"},
    {"America/Argentina/Buenos_Aires", "<-03>3"},
    {"UTC", "UTC0"},
};

std::string UrlEncode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c >= 0x20) {
            out += c;
        }
    }
    return out;
}

std::string JStr(cJSON* obj, const char* key) {
    cJSON* it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(it) ? it->valuestring : "";
}

double JNum(cJSON* obj, const char* key, double def = 0) {
    cJSON* it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

bool NetworkUp() { return WifiManager::GetInstance().IsConnected(); }

int Round(float v) { return (int)lroundf(v); }
}  // namespace

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
void LocationService::Load() {
    Settings s(kNs);
    std::lock_guard<std::mutex> lock(mutex_);
    place_.manual = s.GetInt("manual", 0) != 0;
    place_.city = s.GetString("city");
    place_.region = s.GetString("region");
    place_.country = s.GetString("country");
    place_.lat = atof(s.GetString("lat", "0").c_str());
    place_.lon = atof(s.GetString("lon", "0").c_str());
    place_.tz = s.GetString("tz", DEFAULT_TIMEZONE);
    place_.tz_name = s.GetString("tz_name");
    place_.known = !place_.city.empty();
}

void LocationService::Save() {
    Settings s(kNs, true);
    std::lock_guard<std::mutex> lock(mutex_);
    char buf[24];
    s.SetInt("manual", place_.manual ? 1 : 0);
    s.SetString("city", place_.city);
    s.SetString("region", place_.region);
    s.SetString("country", place_.country);
    snprintf(buf, sizeof(buf), "%.4f", place_.lat);
    s.SetString("lat", buf);
    snprintf(buf, sizeof(buf), "%.4f", place_.lon);
    s.SetString("lon", buf);
    s.SetString("tz", place_.tz);
    s.SetString("tz_name", place_.tz_name);
}

void LocationService::ApplyTimezone() {
    std::string tz;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tz = place_.tz.empty() ? DEFAULT_TIMEZONE : place_.tz;
    }
    setenv("TZ", tz.c_str(), 1);
    tzset();
    ESP_LOGI(TAG, "Time zone: %s", tz.c_str());
}

std::string LocationService::PosixForZone(const std::string& iana, int offset_seconds,
                                          bool have_offset) {
    for (const auto& z : kZones) {
        if (iana == z.iana) {
            return z.posix;
        }
    }
    if (!have_offset) {
        return "";
    }
    // Fixed offset. POSIX counts the other way round: UTC+5:30 is "<+0530>-5:30".
    int off = offset_seconds;
    char sign = off >= 0 ? '+' : '-';
    int a = abs(off);
    int h = a / 3600, m = (a % 3600) / 60;
    char buf[32];
    if (m) {
        snprintf(buf, sizeof(buf), "<%c%02d%02d>%c%d:%02d", sign, h, m, off >= 0 ? '-' : '+', h, m);
    } else {
        snprintf(buf, sizeof(buf), "<%c%02d>%c%d", sign, h, off >= 0 ? '-' : '+', h);
    }
    return buf;
}

const char* LocationService::ZoneChoicesJson() {
    // Offered in the console's time zone menu
    return "[[\"Auto (from city)\",\"\"],[\"India (IST, UTC+5:30)\",\"IST-5:30\"],"
           "[\"Nepal (UTC+5:45)\",\"<+0545>-5:45\"],[\"Pakistan (UTC+5)\",\"PKT-5\"],"
           "[\"Bangladesh (UTC+6)\",\"<+06>-6\"],[\"Gulf (UTC+4)\",\"<+04>-4\"],"
           "[\"Saudi / Qatar (UTC+3)\",\"<+03>-3\"],[\"Thailand / Jakarta (UTC+7)\",\"<+07>-7\"],"
           "[\"Singapore / China (UTC+8)\",\"<+08>-8\"],[\"Japan / Korea (UTC+9)\",\"<+09>-9\"],"
           "[\"Sydney (AEST/AEDT)\",\"AEST-10AEDT,M10.1.0,M4.1.0/3\"],"
           "[\"UK (GMT/BST)\",\"GMT0BST,M3.5.0/1,M10.5.0\"],"
           "[\"Central Europe (CET/CEST)\",\"CET-1CEST,M3.5.0,M10.5.0/3\"],"
           "[\"East Africa (UTC+3)\",\"EAT-3\"],[\"US Eastern\",\"EST5EDT,M3.2.0,M11.1.0\"],"
           "[\"US Central\",\"CST6CDT,M3.2.0,M11.1.0\"],[\"US Mountain\",\"MST7MDT,M3.2.0,M11.1.0\"],"
           "[\"US Pacific\",\"PST8PDT,M3.2.0,M11.1.0\"],[\"UTC\",\"UTC0\"]]";
}

// ---------------------------------------------------------------------------
// Start + background loop
// ---------------------------------------------------------------------------
void LocationService::Start(ShowFn show) {
    show_ = std::move(show);
    Load();
    ApplyTimezone();
    xTaskCreate([](void* arg) { static_cast<LocationService*>(arg)->Loop(); }, "chitu_loc", 6144,
                this, 2, nullptr);
}

bool LocationService::time_valid() const { return time(nullptr) > 1700000000; }

void LocationService::RequestRefresh() {
    geo_tried_ = false;
    refresh_ = true;
}

void LocationService::StartNtp() {
    static const char* kServers[] = {"pool.ntp.org", "time.google.com", "time.cloudflare.com"};
    if (ntp_started_) {
        esp_netif_sntp_deinit();
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(kServers[ntp_server_ % 3]);
    config.sync_cb = [](struct timeval*) {
        LocationService::GetInstance().ntp_synced_ = true;
        ESP_LOGI(TAG, "Clock synchronised with internet time");
    };
    if (esp_netif_sntp_init(&config) == ESP_OK) {
        ntp_started_ = true;
        ntp_started_us_ = esp_timer_get_time();
        ESP_LOGI(TAG, "Internet time from %s", kServers[ntp_server_ % 3]);
    }
}

void LocationService::Loop() {
    int64_t next_weather_us = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!NetworkUp()) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        // 1. Clock: NTP (retry with another server if no answer within a minute)
        if (!ntp_started_) {
            StartNtp();
        } else if (!ntp_synced_ && now - ntp_started_us_ > 60 * kSec) {
            ntp_server_++;
            StartNtp();
        }
        // 2. Place: detect from the internet address unless set by hand
        bool manual;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            manual = place_.manual;
        }
        if (!manual && !geo_tried_) {
            geo_tried_ = true;
            if (DetectFromIp()) {
                refresh_ = true;
            } else {
                geo_tried_ = false;  // try again later
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
        }
        // 3. Weather
        bool known;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            known = place_.known;
        }
        if (known && (refresh_ || now >= next_weather_us)) {
            refresh_ = false;
            bool ok = FetchWeather();
            next_weather_us = now + (ok ? (int64_t)WEATHER_REFRESH_MINUTES * 60 * kSec : 2 * 60 * kSec);
        }
    }
}

std::string LocationService::HttpGet(const std::string& url, int* status) {
    *status = -1;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(4);
    http->SetHeader("User-Agent", "Chitu-robot/1.0");
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGW(TAG, "GET %s failed: %s", url.c_str(), opened.error().ToString().c_str());
        http->Close();
        return "";
    }
    auto code = http->GetStatusCode();
    if (!code) {
        http->Close();
        return "";
    }
    *status = *code;
    std::string body = http->ReadAll();
    http->Close();
    return body;
}

bool LocationService::DetectFromIp() {
    int status = 0;
    // ip-api.com: free, no key, plain HTTP only. Accuracy is usually the city of the
    // internet provider's exchange, which can be far away - hence the manual setting.
    std::string body = HttpGet(
        "http://ip-api.com/json/?fields=status,country,regionName,city,lat,lon,offset,timezone", &status);
    if (status != 200 || body.empty()) {
        last_error_ = "Could not detect the city from the internet address";
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    bool ok = JStr(root, "status") == "success";
    if (ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!place_.manual) {  // the owner may have set a city meanwhile
            place_.city = JStr(root, "city");
            place_.region = JStr(root, "regionName");
            place_.country = JStr(root, "country");
            place_.lat = JNum(root, "lat");
            place_.lon = JNum(root, "lon");
            place_.tz_name = JStr(root, "timezone");
            place_.tz = PosixForZone(place_.tz_name, (int)JNum(root, "offset"), true);
            place_.known = !place_.city.empty();
            ESP_LOGI(TAG, "Detected (approximate): %s, %s, %s  TZ %s", place_.city.c_str(),
                     place_.region.c_str(), place_.country.c_str(), place_.tz.c_str());
        }
    }
    cJSON_Delete(root);
    if (ok) {
        Save();
        ApplyTimezone();
    }
    return ok;
}

bool LocationService::FetchWeather() {
    Place p = place();
    char url[360];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,"
             "wind_speed_10m,is_day&daily=temperature_2m_max,temperature_2m_min"
             "&timezone=auto&forecast_days=1%s",
             p.lat, p.lon, TEMP_FAHRENHEIT ? "&temperature_unit=fahrenheit" : "");
    int status = 0;
    std::string body = HttpGet(url, &status);
    if (status != 200 || body.empty()) {
        last_error_ = "Weather service not reachable";
        ESP_LOGW(TAG, "Weather fetch failed (status %d)", status);
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    cJSON* cur = cJSON_GetObjectItem(root, "current");
    cJSON* daily = cJSON_GetObjectItem(root, "daily");
    bool ok = cJSON_IsObject(cur);
    if (ok) {
        Weather w;
        w.ok = true;
        w.temp = JNum(cur, "temperature_2m");
        w.feels = JNum(cur, "apparent_temperature", w.temp);
        w.humidity = (int)JNum(cur, "relative_humidity_2m");
        w.code = (int)JNum(cur, "weather_code", -1);
        w.wind_kmh = JNum(cur, "wind_speed_10m");
        w.is_day = JNum(cur, "is_day", 1) != 0;
        auto first = [daily](const char* key, float def) -> float {
            cJSON* arr = daily ? cJSON_GetObjectItem(daily, key) : nullptr;
            cJSON* v = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, 0) : nullptr;
            return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
        };
        w.high = first("temperature_2m_max", w.temp);
        w.low = first("temperature_2m_min", w.temp);
        w.fetched_us = esp_timer_get_time();
        w.city = p.city;
        // A city picked by hand without a known zone: take the offset open-meteo reports
        int offset = (int)JNum(root, "utc_offset_seconds", 0);
        std::string iana = JStr(root, "timezone");
        bool update_tz = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            weather_ = w;
            if (place_.tz.empty() || (place_.manual && place_.tz_name.empty() && !iana.empty())) {
                place_.tz_name = iana;
                place_.tz = PosixForZone(iana, offset, true);
                update_tz = true;
            }
        }
        if (update_tz) {
            Save();
            ApplyTimezone();
        }
        ESP_LOGI(TAG, "Weather %s: %.1f (feels %.1f), %s, %d%%", p.city.c_str(), w.temp, w.feels,
                 WeatherText(w.code).c_str(), w.humidity);
    }
    cJSON_Delete(root);
    return ok;
}

// ---------------------------------------------------------------------------
// Public getters / setters
// ---------------------------------------------------------------------------
LocationService::Place LocationService::place() {
    std::lock_guard<std::mutex> lock(mutex_);
    return place_;
}

LocationService::Weather LocationService::weather() {
    std::lock_guard<std::mutex> lock(mutex_);
    return weather_;
}

bool LocationService::weather_fresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    return weather_.ok && weather_.city == place_.city &&
           esp_timer_get_time() - weather_.fetched_us < (int64_t)WEATHER_MAX_AGE_MINUTES * 60 * kSec;
}

std::string LocationService::SearchCities(const std::string& query) {
    if (query.empty() || !NetworkUp()) {
        return "[]";
    }
    int status = 0;
    std::string body = HttpGet("https://geocoding-api.open-meteo.com/v1/search?count=6&language=en&format=json&name=" +
                                   UrlEncode(query),
                               &status);
    if (status != 200) {
        return "[]";
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return "[]";
    }
    std::string out = "[";
    cJSON* results = cJSON_GetObjectItem(root, "results");
    cJSON* r = nullptr;
    bool first = true;
    cJSON_ArrayForEach(r, results) {
        char num[64];
        snprintf(num, sizeof(num), "\"lat\":%.4f,\"lon\":%.4f", JNum(r, "latitude"), JNum(r, "longitude"));
        out += std::string(first ? "" : ",") + "{\"name\":\"" + JsonEscape(JStr(r, "name")) +
               "\",\"region\":\"" + JsonEscape(JStr(r, "admin1")) + "\",\"country\":\"" +
               JsonEscape(JStr(r, "country")) + "\",\"tz\":\"" + JsonEscape(JStr(r, "timezone")) +
               "\"," + num + "}";
        first = false;
    }
    out += "]";
    cJSON_Delete(root);
    return out;
}

void LocationService::SetManual(const std::string& city, const std::string& region,
                                const std::string& country, double lat, double lon,
                                const std::string& tz_name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        place_.manual = true;
        place_.city = city;
        place_.region = region;
        place_.country = country;
        place_.lat = lat;
        place_.lon = lon;
        place_.tz_name = tz_name;
        place_.tz = PosixForZone(tz_name, 0, false);  // empty if unknown: weather fills it in
        place_.known = true;
        weather_ = Weather();
    }
    Save();
    ApplyTimezone();
    refresh_ = true;
    ESP_LOGI(TAG, "City set by hand: %s (%s)", city.c_str(), tz_name.c_str());
}

void LocationService::SetTimezone(const std::string& posix, const std::string& label) {
    if (posix.empty()) {
        // back to the zone of the city
        Place p = place();
        std::lock_guard<std::mutex> lock(mutex_);
        place_.tz = PosixForZone(p.tz_name, 0, false);
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        place_.tz = posix;
        place_.tz_name = label;
    }
    Save();
    ApplyTimezone();
    refresh_ = true;
}

void LocationService::UseAutomatic() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        place_.manual = false;
    }
    Save();
    RequestRefresh();
}

std::string LocationService::WeatherText(int code) {
    if (code == 0) return "Clear sky";
    if (code == 1) return "Mostly clear";
    if (code == 2) return "Partly cloudy";
    if (code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Foggy";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Rain showers";
    if (code >= 85 && code <= 86) return "Snow showers";
    if (code >= 95) return "Thunderstorm";
    return "Unknown";
}

std::string LocationService::StatusJson() {
    Place p = place();
    Weather w = weather();
    bool fresh = weather_fresh();
    char buf[64];
    std::string local = "";
    if (time_valid()) {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        strftime(buf, sizeof(buf), "%a %d %b %Y, %H:%M", &t);
        local = buf;
    }
    std::string json = "{\"city\":\"" + JsonEscape(p.city) + "\",\"region\":\"" + JsonEscape(p.region) +
                       "\",\"country\":\"" + JsonEscape(p.country) + "\",\"manual\":" +
                       (p.manual ? "true" : "false") + ",\"tz\":\"" + JsonEscape(p.tz) +
                       "\",\"tz_name\":\"" + JsonEscape(p.tz_name) + "\",\"time\":\"" + local +
                       "\",\"ntp\":" + (ntp_synced_ ? "true" : "false");
    if (fresh) {
        snprintf(buf, sizeof(buf), ",\"temp\":%d,\"wmin\":%d", Round(w.temp),
                 (int)((esp_timer_get_time() - w.fetched_us) / (60 * kSec)));
        json += buf;
        json += ",\"sky\":\"" + WeatherText(w.code) + "\"";
    }
    json += ",\"unit\":\"" + std::string(TEMP_FAHRENHEIT ? "F" : "C") + "\"}";
    return json;
}

// ---------------------------------------------------------------------------
// AI tools
// ---------------------------------------------------------------------------
void LocationService::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.chitu.get_time_and_place",
        "Gets the exact local time, date, weekday and month where Chitu is, and Chitu's city "
        "(set by its owner). ALWAYS call this for any question about the time, date, day, month, "
        "year or where Chitu is - it is more accurate than any time or location you were given, "
        "which can be wrong. It also shows the answer big on Chitu's screen.\n"
        "show: which page to show: time, date, month or none.",
        PropertyList({Property("show", kPropertyTypeString, std::string("time"))}),
        [this](const PropertyList& props) -> ReturnValue {
            std::string show = props["show"].value<std::string>();
            Place p = place();
            if (!time_valid()) {
                return std::string("{\"ok\":false,\"error\":\"The clock is not set yet (no internet time)\"}");
            }
            time_t now = time(nullptr);
            struct tm t;
            localtime_r(&now, &t);
            char date[48], clock[16], month[16], weekday[16];
            strftime(date, sizeof(date), "%A %d %B %Y", &t);
            strftime(clock, sizeof(clock), "%H:%M", &t);
            strftime(month, sizeof(month), "%B", &t);
            strftime(weekday, sizeof(weekday), "%A", &t);
            if (show_ && (show == "time" || show == "date" || show == "month")) {
                show_(show);
            }
            std::string where = p.city.empty() ? "unknown" : p.city;
            if (!p.country.empty()) where += ", " + p.country;
            return "{\"ok\":true,\"local_time_24h\":\"" + std::string(clock) + "\",\"date\":\"" + date +
                   "\",\"weekday\":\"" + weekday + "\",\"month\":\"" + month + "\",\"year\":" +
                   std::to_string(t.tm_year + 1900) + ",\"place\":\"" + JsonEscape(where) +
                   "\",\"place_source\":\"" + (p.manual ? "set by owner" : "approximate, from internet address") +
                   "\",\"time_zone\":\"" + JsonEscape(p.tz_name.empty() ? p.tz : p.tz_name) + "\"}";
        });

    mcp.AddTool(
        "self.chitu.get_weather",
        "Gets the current weather and today's high/low for Chitu's city (set by its owner). "
        "ALWAYS call this for any question about weather, temperature, rain, heat or cold, "
        "instead of any other weather source. It also shows the weather on Chitu's screen. "
        "If it says the weather is unavailable, tell the user that - never guess.\n"
        "show: weather, temperature or none.",
        PropertyList({Property("show", kPropertyTypeString, std::string("weather"))}),
        [this](const PropertyList& props) -> ReturnValue {
            std::string show = props["show"].value<std::string>();
            if (!weather_fresh() && NetworkUp()) {
                FetchWeather();  // try once right now
            }
            Place p = place();
            if (show_ && (show == "weather" || show == "temperature")) {
                show_(show);
            }
            if (!weather_fresh()) {
                return std::string("{\"ok\":false,\"error\":\"Weather is unavailable right now\"}");
            }
            Weather w = weather();
            char buf[256];
            const char* u = TEMP_FAHRENHEIT ? "F" : "C";
            snprintf(buf, sizeof(buf),
                     "{\"ok\":true,\"city\":\"%s\",\"conditions\":\"%s\",\"temperature\":\"%d %s\","
                     "\"feels_like\":\"%d %s\",\"high\":\"%d %s\",\"low\":\"%d %s\",\"humidity\":\"%d%%\","
                     "\"wind_kmh\":%d,\"minutes_old\":%d}",
                     JsonEscape(p.city).c_str(), WeatherText(w.code).c_str(), Round(w.temp), u,
                     Round(w.feels), u, Round(w.high), u, Round(w.low), u, w.humidity, Round(w.wind_kmh),
                     (int)((esp_timer_get_time() - w.fetched_us) / (60 * kSec)));
            return std::string(buf);
        });
}
