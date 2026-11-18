#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

// Where Chitu is, what time it is there and what the weather is like.
//
//  - Clock: real UTC from internet time servers (NTP), shown in the local time zone.
//  - Place: found automatically from the internet address (approximate, often only the
//    nearest big city) or set by hand in the phone console. A hand-set place wins.
//  - Weather: open-meteo.com for the place's coordinates (no account needed), refreshed
//    every WEATHER_REFRESH_MINUTES. Weather older than WEATHER_MAX_AGE_MINUTES is never
//    shown as current: the robot says it is unavailable instead.
class LocationService {
public:
    struct Place {
        std::string city;
        std::string region;
        std::string country;
        double lat = 0;
        double lon = 0;
        std::string tz;        // POSIX TZ string, e.g. "IST-5:30"
        std::string tz_name;   // IANA name for display, e.g. "Asia/Kolkata"
        bool manual = false;   // set by the owner (true) or detected (false)
        bool known = false;
    };
    struct Weather {
        bool ok = false;
        float temp = 0;
        float feels = 0;
        float high = 0;
        float low = 0;
        int humidity = 0;
        float wind_kmh = 0;
        int code = -1;
        bool is_day = true;
        int64_t fetched_us = 0;
        std::string city;  // the place it belongs to
    };
    // Full-screen info page request: kind = "time", "date", "month", "weather", "temperature"
    using ShowFn = std::function<void(const std::string& kind)>;

    static LocationService& GetInstance() {
        static LocationService instance;
        return instance;
    }

    void Start(ShowFn show);

    Place place();
    Weather weather();  // last good weather (check ok / age)
    bool weather_fresh();
    bool time_valid() const;
    bool time_synced() const { return ntp_synced_.load(); }

    // Owner settings (from the phone console)
    // Searches the open-meteo city list; returns a JSON array for the console.
    std::string SearchCities(const std::string& query);
    // Sets a city by hand (tz_name = IANA name from the search, may be empty)
    void SetManual(const std::string& city, const std::string& region, const std::string& country,
                   double lat, double lon, const std::string& tz_name);
    // Overrides only the time zone (POSIX string), keeps the place
    void SetTimezone(const std::string& posix, const std::string& label);
    void UseAutomatic();
    void RequestRefresh();  // re-detect / refresh weather soon

    std::string StatusJson();
    static std::string WeatherText(int code);
    static const char* ZoneChoicesJson();

    void RegisterMcpTools();

private:
    LocationService() = default;

    std::mutex mutex_;
    Place place_;
    Weather weather_;
    ShowFn show_;
    std::atomic<bool> ntp_started_{false};
    std::atomic<bool> ntp_synced_{false};
    std::atomic<bool> refresh_{true};
    std::atomic<bool> geo_tried_{false};
    int64_t ntp_started_us_ = 0;
    int ntp_server_ = 0;
    std::string last_error_;

    void Load();
    void Save();
    void ApplyTimezone();
    void Loop();
    void StartNtp();
    bool DetectFromIp();
    bool FetchWeather();
    static std::string PosixForZone(const std::string& iana, int offset_seconds, bool have_offset);
    static std::string HttpGet(const std::string& url, int* status);
};
