#include "net_manager.h"

#include "config.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include <cstring>

#define TAG "NetManager"

namespace {
constexpr int64_t kSec = 1000LL * 1000;

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
}  // namespace

const char* NetManager::ReasonText(int reason) {
    switch (reason) {
        case 0: return "";
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "wrong password or router refused";
        case WIFI_REASON_NO_AP_FOUND: return "network not found";
        case WIFI_REASON_BEACON_TIMEOUT: return "signal lost (weak signal or router busy)";
        case 8: return "left the network";
        case 4:  // disassociated due to inactivity
        case 3:  // deauthenticated, station left
            return "router dropped the robot";
        case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
        default: return "other";
    }
}

void NetManager::Start() {
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, OnWifiEvent, this,
                                        nullptr);
    next_preferred_try_us_ = esp_timer_get_time() + (int64_t)WIFI_PREFERRED_RETRY_MINUTES * 60 * kSec;
    xTaskCreate([](void* arg) { static_cast<NetManager*>(arg)->Loop(); }, "chitu_net", 3584, this, 3,
                nullptr);
}

void NetManager::OnWifiEvent(void* arg, esp_event_base_t, int32_t, void* data) {
    auto* self = static_cast<NetManager*>(arg);
    auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
    self->last_reason_ = ev->reason;
    if (self->connected_since_us_ > 0) {
        self->disconnects_++;
        ESP_LOGW(TAG, "Wi-Fi lost (reason %d: %s, RSSI was %d)", ev->reason, ReasonText(ev->reason),
                 ev->rssi);
    }
}

void NetManager::OnConnected() {
    connected_since_us_ = esp_timer_get_time();
    lost_since_us_ = 0;
    if (trying_preferred_) {
        trying_preferred_ = false;
        ESP_LOGI(TAG, "Now on %s", CurrentSsid().c_str());
    }
    // Radio power saving: none unless the owner asked for it
    esp_wifi_set_ps(WIFI_POWER_SAVE && !busy_ ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void NetManager::OnDisconnected() {
    connected_since_us_ = 0;
    if (lost_since_us_ == 0) {
        lost_since_us_ = esp_timer_get_time();
    }
}

bool NetManager::connected() const { return WifiManager::GetInstance().IsConnected(); }

std::string NetManager::CurrentSsid() {
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return reinterpret_cast<const char*>(ap.ssid);
    }
    return "";
}

void NetManager::GoOffline(int minutes) {
    ESP_LOGI(TAG, "Owner switched Wi-Fi off (%d min)", minutes);
    offline_ = true;
    offline_until_us_ = minutes > 0 ? esp_timer_get_time() + (int64_t)minutes * 60 * kSec : 0;
    // Give the console a moment to show the answer before the link drops
    xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            WifiManager::GetInstance().StopStation();
            vTaskDelete(nullptr);
        },
        "net_off", 3072, nullptr, 3, nullptr);
}

void NetManager::GoOnline() {
    if (!offline_.exchange(false)) {
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi back on");
    lost_since_us_ = esp_timer_get_time();
    WifiManager::GetInstance().StartStation();
}

void NetManager::TryPreferred() {
    auto& list = SsidManager::GetInstance().GetSsidList();
    if (list.empty()) {
        return;
    }
    const auto& pref = list[0];
    ESP_LOGI(TAG, "Trying to go back to the preferred network %s", pref.ssid.c_str());
    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char*>(cfg.sta.ssid), pref.ssid.c_str(), sizeof(cfg.sta.ssid));
    strncpy(reinterpret_cast<char*>(cfg.sta.password), pref.password.c_str(), sizeof(cfg.sta.password));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        trying_preferred_ = true;
        // The Wi-Fi library reconnects with this setting; if the preferred network
        // is not there it falls back to the strongest saved one by itself.
        esp_wifi_disconnect();
    }
}

void NetManager::Loop() {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int64_t now = esp_timer_get_time();
        auto& wifi = WifiManager::GetInstance();

        if (offline_) {
            if (offline_until_us_ > 0 && now > offline_until_us_) {
                GoOnline();
            }
            continue;
        }
        if (wifi.IsConfigMode()) {
            continue;
        }
        if (!wifi.IsConnected()) {
            if (lost_since_us_ == 0) {
                lost_since_us_ = now;
            }
            // Still offline after 90 s: restart the station (fresh scan of all saved networks)
            if (now - lost_since_us_ > 90 * kSec && !SsidManager::GetInstance().GetSsidList().empty()) {
                restarts_++;
                ESP_LOGW(TAG, "No Wi-Fi for 90 s - restarting the Wi-Fi station (%d)", restarts_.load());
                wifi.StopStation();
                vTaskDelay(pdMS_TO_TICKS(1000));
                wifi.StartStation();
                lost_since_us_ = esp_timer_get_time();
            }
            continue;
        }
        lost_since_us_ = 0;
        if (connected_since_us_ == 0) {
            connected_since_us_ = now;
        }
        // Keep the radio awake while the console / live view is used
        static bool last_busy = false;
        bool busy = busy_;
        if (busy != last_busy) {
            last_busy = busy;
            esp_wifi_set_ps(WIFI_POWER_SAVE && !busy ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
        }
        // Back to the preferred network after a while elsewhere (only when idle)
        auto& list = SsidManager::GetInstance().GetSsidList();
        if (list.size() > 1 && now >= next_preferred_try_us_ &&
            now - connected_since_us_ > (int64_t)WIFI_PREFERRED_RETRY_MINUTES * 60 * kSec &&
            !busy && (is_idle_ == nullptr || is_idle_())) {
            std::string current = CurrentSsid();
            if (!current.empty() && current != list[0].ssid) {
                next_preferred_try_us_ = now + 60LL * 60 * kSec;  // at most once an hour
                TryPreferred();
            } else {
                next_preferred_try_us_ = now + 5LL * 60 * kSec;
            }
        }
    }
}

std::string NetManager::ListJson() {
    auto& list = SsidManager::GetInstance().GetSsidList();
    std::string current = connected() ? CurrentSsid() : "";
    std::string out = "[";
    for (size_t i = 0; i < list.size(); i++) {
        out += std::string(i ? "," : "") + "{\"i\":" + std::to_string(i) + ",\"ssid\":\"" +
               JsonEscape(list[i].ssid) + "\",\"pref\":" + (i == 0 ? "true" : "false") +
               ",\"now\":" + (list[i].ssid == current ? "true" : "false") + "}";
    }
    return out + "]";
}

bool NetManager::Add(const std::string& ssid, const std::string& password, std::string* error) {
    if (ssid.empty() || ssid.size() > 32) {
        *error = "Network name must be 1-32 characters";
        return false;
    }
    if (!password.empty() && (password.size() < 8 || password.size() > 63)) {
        *error = "Password must be 8-63 characters (or empty for an open network)";
        return false;
    }
    auto& mgr = SsidManager::GetInstance();
    bool had_pref = !mgr.GetSsidList().empty();
    std::string pref = had_pref ? mgr.GetSsidList()[0].ssid : "";
    mgr.AddSsid(ssid, password);  // new networks go to the front ...
    if (had_pref && pref != ssid) {
        // ... but the preferred network stays the preferred one
        auto& list = mgr.GetSsidList();
        for (size_t i = 0; i < list.size(); i++) {
            if (list[i].ssid == pref) {
                mgr.SetDefaultSsid((int)i);
                break;
            }
        }
    }
    ESP_LOGI(TAG, "Saved network %s", ssid.c_str());
    return true;
}

bool NetManager::Remove(int index) {
    auto& mgr = SsidManager::GetInstance();
    if (index < 0 || index >= (int)mgr.GetSsidList().size() || mgr.GetSsidList().size() <= 1) {
        return false;  // the last network cannot be removed from the console
    }
    mgr.RemoveSsid(index);
    return true;
}

bool NetManager::MakePreferred(int index) {
    auto& mgr = SsidManager::GetInstance();
    if (index < 0 || index >= (int)mgr.GetSsidList().size()) {
        return false;
    }
    mgr.SetDefaultSsid(index);
    next_preferred_try_us_ = esp_timer_get_time() + 30 * kSec;
    return true;
}

std::string NetManager::StatusJson() {
    bool on = connected();
    int rssi = 0;
    std::string ssid, ip;
    if (on) {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
            ssid = reinterpret_cast<const char*>(ap.ssid);
        }
        esp_netif_ip_info_t info = {};
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            char buf[20];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
            ip = buf;
        }
    }
    int64_t now = esp_timer_get_time();
    int up_min = on && connected_since_us_ > 0 ? (int)((now - connected_since_us_) / (60 * kSec)) : 0;
    const char* state = offline_ ? "offline_by_owner" : (on ? "connected" : "searching");
    return std::string("{\"state\":\"") + state + "\",\"ssid\":\"" + JsonEscape(ssid) + "\",\"ip\":\"" +
           ip + "\",\"rssi\":" + std::to_string(rssi) + ",\"up_min\":" + std::to_string(up_min) +
           ",\"drops\":" + std::to_string(disconnects_.load()) + ",\"restarts\":" +
           std::to_string(restarts_.load()) + ",\"last_reason\":\"" + ReasonText(last_reason_) +
           "\",\"saved\":" + ListJson() + "}";
}
