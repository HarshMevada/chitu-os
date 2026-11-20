#pragma once

#include <esp_event.h>

#include <atomic>
#include <cstdint>

// Keeps Chitu on Wi-Fi (on top of xiaozhi's own Wi-Fi handling).
//
//  - Uses xiaozhi's saved networks (set up once through the "Chitu" setup hotspot).
//    The Wi-Fi library connects to the strongest saved network and reconnects by itself.
//  - Watchdog: if a working connection is lost and does not come back within
//    WIFI_WATCHDOG_SECONDS, the Wi-Fi station is restarted (fresh scan, no reboot).
//  - Radio power saving is off (it caused dropouts on some routers).
//  - Logs why the connection dropped, for the serial monitor.
class NetManager {
public:
    static NetManager& GetInstance() {
        static NetManager instance;
        return instance;
    }

    void Start();
    // Called by the board for every network event
    void OnConnected();
    void OnDisconnected();

private:
    NetManager() = default;
    std::atomic<int64_t> lost_since_us_{0};
    std::atomic<bool> was_connected_{false};
    std::atomic<int> restarts_{0};

    void Loop();
    static void OnWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
    static const char* ReasonText(int reason);
};
