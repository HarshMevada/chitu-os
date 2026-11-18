#pragma once

#include <esp_event.h>
#include <esp_timer.h>

#include <atomic>
#include <cstdint>
#include <string>

// Keeps Chitu on Wi-Fi.
//
//  - Several saved networks (xiaozhi's list, up to 10). The first one is the preferred
//    network; when the robot is on another one for a while it tries to go back (only
//    when idle, and not again for an hour if that fails).
//  - Watchdog: if the connection is lost and nothing has been happening for a while,
//    the station is restarted (no reboot needed).
//  - Power saving is kept low so the console and camera stay smooth.
//  - The owner can switch Wi-Fi off from the console for a while ("offline"); the BOOT
//    button or the timer brings it back. A deliberate disconnect is never "repaired".
//  - Remembers disconnect reasons for the console.
class NetManager {
public:
    static NetManager& GetInstance() {
        static NetManager instance;
        return instance;
    }

    void Start();
    // Called by the board for every network event (connected / disconnected)
    void OnConnected();
    void OnDisconnected();

    // Owner controls
    void GoOffline(int minutes);  // 0 = until the BOOT button / console
    void GoOnline();
    bool offline_by_owner() const { return offline_.load(); }

    // Saved networks
    std::string ListJson();  // never contains passwords
    bool Add(const std::string& ssid, const std::string& password, std::string* error);
    bool Remove(int index);
    bool MakePreferred(int index);

    std::string StatusJson();
    bool connected() const;
    // While true the radio never sleeps (phone console or live view in use)
    void SetBusy(bool busy) { busy_ = busy; }
    void SetIdleCheck(bool (*idle)()) { is_idle_ = idle; }

private:
    NetManager() = default;
    std::atomic<bool> offline_{false};
    std::atomic<bool> busy_{false};
    std::atomic<int64_t> offline_until_us_{0};
    std::atomic<int64_t> connected_since_us_{0};
    std::atomic<int64_t> lost_since_us_{0};
    std::atomic<int> disconnects_{0};
    std::atomic<int> last_reason_{0};
    std::atomic<int> restarts_{0};
    int64_t next_preferred_try_us_ = 0;
    bool trying_preferred_ = false;
    bool (*is_idle_)() = nullptr;

    void Loop();
    void TryPreferred();
    std::string CurrentSsid();
    static void OnWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
    static const char* ReasonText(int reason);
};
