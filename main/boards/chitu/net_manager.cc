#include "net_manager.h"

#include "config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#define TAG "NetManager"

namespace {
constexpr int64_t kSec = 1000LL * 1000;
}

const char* NetManager::ReasonText(int reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "wrong password or router refused";
        case WIFI_REASON_NO_AP_FOUND: return "network not found";
        case WIFI_REASON_BEACON_TIMEOUT: return "signal lost (weak signal or router busy)";
        case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
        case 3:
        case 4: return "router dropped the robot";
        case 8: return "left the network";
        default: return "other";
    }
}

void NetManager::Start() {
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, OnWifiEvent, this,
                                        nullptr);
    xTaskCreate([](void* arg) { static_cast<NetManager*>(arg)->Loop(); }, "chitu_net", 3072, this, 3,
                nullptr);
}

void NetManager::OnWifiEvent(void* arg, esp_event_base_t, int32_t, void* data) {
    auto* self = static_cast<NetManager*>(arg);
    auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
    if (self->was_connected_) {
        ESP_LOGW(TAG, "Wi-Fi lost (reason %d: %s, RSSI was %d)", ev->reason, ReasonText(ev->reason),
                 ev->rssi);
    }
}

void NetManager::OnConnected() {
    was_connected_ = true;
    lost_since_us_ = 0;
    esp_wifi_set_ps(WIFI_PS_NONE);  // radio always awake: most stable connection
}

void NetManager::OnDisconnected() {
    if (lost_since_us_ == 0) {
        lost_since_us_ = esp_timer_get_time();
    }
}

void NetManager::Loop() {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        auto& wifi = WifiManager::GetInstance();
        if (wifi.IsConfigMode() || wifi.IsConnected()) {
            lost_since_us_ = 0;
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (lost_since_us_ == 0) {
            lost_since_us_ = now;
            continue;
        }
        // The library normally reconnects by itself. Still offline after the watchdog
        // time: restart the station (fresh scan of all saved networks).
        if (now - lost_since_us_ > (int64_t)WIFI_WATCHDOG_SECONDS * kSec &&
            !SsidManager::GetInstance().GetSsidList().empty()) {
            restarts_++;
            ESP_LOGW(TAG, "No Wi-Fi for %d s - restarting the Wi-Fi station (%d)", WIFI_WATCHDOG_SECONDS,
                     restarts_.load());
            wifi.StopStation();
            vTaskDelay(pdMS_TO_TICKS(1000));
            wifi.StartStation();
            lost_since_us_ = esp_timer_get_time();
        }
    }
}
