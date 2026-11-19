#include "web_console.h"

#include "application.h"
#include "audio_codec.h"
#include "audio_diag.h"
#include "chat_bridge.h"
#include "chitu_camera.h"
#include "chitu_cert.h"
#include "config.h"
#include "device_state_machine.h"
#include "alarm_manager.h"
#include "motor_controller.h"
#include "net_manager.h"
#include "obstacle_sensor.h"
#include "pet_mode.h"
#include "remote_audio.h"
#include "settings.h"
#include "time_location.h"
#include "web_console_page.h"

#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_https_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define TAG "WebConsole"

namespace {

// WebSocket binary frame types (first byte)
constexpr uint8_t kWsMicIn = 0x01;     // phone -> robot: 16 kHz int16 mono
constexpr uint8_t kWsVideo = 0x10;     // robot -> phone: JPEG frame
constexpr uint8_t kWsVoice = 0x20;     // robot -> phone: Chitu's voice, output sample rate
constexpr uint8_t kWsRoom = 0x21;      // robot -> phone: room sound, 16 kHz
constexpr int kVideoMinIntervalMs = 1000 / VIDEO_FPS_MAX;         // standing still
constexpr int kVideoDrivingIntervalMs = 1000 / VIDEO_FPS_DRIVING;  // while the motors run
constexpr size_t kRxMax = 8192;

// Push-to-talk state (shared with the audio codec)
std::atomic<bool> g_ptt_hold{false};
std::atomic<bool> g_talk_down{false};  // the talk button is pressed right now

bool QueryValue(const char* query, const char* key, char* out, size_t out_len) {
    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

bool QueryOn(const char* query) {
    char on[4] = {0};
    return QueryValue(query, "on", on, sizeof(on)) && on[0] == '1';
}

int QueryInt(const char* query, const char* key, int def) {
    char v[12] = {0};
    return QueryValue(query, key, v, sizeof(v)) ? atoi(v) : def;
}

// %XX and '+' decoding for query / form values
std::string UrlDecode(const char* in) {
    std::string out;
    for (const char* p = in; *p; p++) {
        if (*p == '+') {
            out += ' ';
        } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            out += (char)strtol(hex, nullptr, 16);
            p += 2;
        } else {
            out += *p;
        }
    }
    return out;
}

std::string FormValue(const std::string& body, const char* key) {
    std::string buf(body.size() + 1, '\0');
    if (httpd_query_key_value(body.c_str(), key, buf.data(), buf.size()) != ESP_OK) {
        return "";
    }
    return UrlDecode(buf.c_str());
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

bool ReadBody(httpd_req_t* req, std::string* out, size_t max) {
    int len = std::min<int>(req->content_len, (int)max);
    out->assign(len, '\0');
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, out->data() + got, len - got);
        if (r <= 0) {
            return false;
        }
        got += r;
    }
    return true;
}

esp_err_t SendJson(httpd_req_t* req, const std::string& json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), json.size());
}

// Internet requests (city search) need more stack than the web server task has,
// so they run in a short helper task while the request waits.
struct CityJob {
    std::string query;
    std::string result;
    SemaphoreHandle_t done;
};
}  // namespace

bool WebConsole::HoldPhoneAudio() { return g_ptt_hold.load(); }
bool WebConsole::TalkHeld() { return g_talk_down.load(); }

std::string WebConsole::Url() const {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return "";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "https://" IPSTR "/", IP2STR(&ip.ip));
    return buf;
}

bool WebConsole::OwnerDriving() const {
    return esp_timer_get_time() - last_drive_us_.load() < 20LL * 1000 * 1000;
}

void WebConsole::Start(const Parts& parts) {
    if (secure_server_ != nullptr) {
        return;  // already running (Wi-Fi reconnects keep the same servers)
    }
    parts_ = parts;
    if (!remote_audio::Init()) {
        ESP_LOGE(TAG, "No memory for phone audio buffers");
    }
    rx_buf_ = static_cast<uint8_t*>(heap_caps_malloc(kRxMax, MALLOC_CAP_SPIRAM));

    // Secure server: the page, commands and the camera/audio WebSocket
    httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
    ssl.servercert = reinterpret_cast<const uint8_t*>(kChituCertPem);
    ssl.servercert_len = sizeof(kChituCertPem);
    ssl.prvtkey_pem = reinterpret_cast<const uint8_t*>(kChituKeyPem);
    ssl.prvtkey_len = sizeof(kChituKeyPem);
    ssl.port_secure = WEB_CONSOLE_PORT_SECURE;
    ssl.httpd.ctrl_port = 32780;
    ssl.httpd.max_open_sockets = 7;
    ssl.httpd.max_uri_handlers = 16;
    ssl.httpd.lru_purge_enable = true;
    // One web server task serves everything (page, commands, camera frames). A phone on a
    // weak link could hold it for 10 s per stalled send, which froze the console and let
    // the drive commands time out. Give up on a stalled socket sooner.
    ssl.httpd.recv_wait_timeout = 5;
    ssl.httpd.send_wait_timeout = 3;
    if (httpd_ssl_start(&secure_server_, &ssl) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the secure web console");
        secure_server_ = nullptr;
        return;
    }
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = HandleIndex, .user_ctx = this},
        {.uri = "/cmd", .method = HTTP_GET, .handler = HandleCmd, .user_ctx = this},
        {.uri = "/status", .method = HTTP_GET, .handler = HandleStatus, .user_ctx = this},
        {.uri = "/setup", .method = HTTP_GET, .handler = HandleSetup, .user_ctx = this},
        {.uri = "/msg", .method = HTTP_POST, .handler = HandleMsg, .user_ctx = this},
        {.uri = "/wifi", .method = HTTP_POST, .handler = HandleWifi, .user_ctx = this},
        {.uri = "/city", .method = HTTP_GET, .handler = HandleCity, .user_ctx = this},
        {.uri = "/city", .method = HTTP_POST, .handler = HandleCitySet, .user_ctx = this},
        {.uri = "/alarms", .method = HTTP_GET, .handler = HandleAlarms, .user_ctx = this},
        {.uri = "/alarms", .method = HTTP_POST, .handler = HandleAlarmsSet, .user_ctx = this},
    };
    for (const auto& h : handlers) {
        httpd_register_uri_handler(secure_server_, &h);
    }
    httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = HandleWs, .user_ctx = this};
    ws.is_websocket = true;
    httpd_register_uri_handler(secure_server_, &ws);

    // Plain http://<ip>/ just sends the phone to the secure page
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_CONSOLE_PORT;
    cfg.ctrl_port = 32781;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 3584;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&redirect_server_, &cfg) == ESP_OK) {
        httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = HandleRedirect, .user_ctx = this};
        httpd_register_uri_handler(redirect_server_, &any);
    } else {
        redirect_server_ = nullptr;
    }

    xTaskCreate([](void* arg) { static_cast<WebConsole*>(arg)->MediaLoop(); }, "chitu_media", 4096,
                this, 4, nullptr);
    ESP_LOGI(TAG, "Web console running at %s", Url().c_str());
}

esp_err_t WebConsole::HandleRedirect(httpd_req_t* req) {
    char host[64] = {0};
    std::string location = "https://";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        char* colon = strchr(host, ':');
        if (colon != nullptr) *colon = '\0';
        location += host;
        location += "/";
    } else {
        location = static_cast<WebConsole*>(req->user_ctx)->Url();
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location.c_str());
    httpd_resp_set_type(req, "text/html");
    std::string body = "<a href=\"" + location + "\">Open Chitu console</a>";
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t WebConsole::HandleIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, web_page::kIndexHtml, sizeof(web_page::kIndexHtml) - 1);
}

// ---------------------------------------------------------------------------
// Live view and push-to-talk
// ---------------------------------------------------------------------------
void WebConsole::SetVideo(bool on) {
    if (on == video_on_.load()) {
        return;
    }
    video_on_ = on;
    if (parts_.camera == nullptr) {
        return;
    }
    if (on) {
        if (!parts_.camera->StartStream()) {
            video_on_ = false;
        }
    } else {
        parts_.camera->StopStream();
    }
}

// Hold to talk: Chitu needs about a second to open the line to the AI. The phone's
// audio is kept (not thrown away) until Chitu is really listening, and on release the
// stop is sent only after everything that was said has been passed on.
void WebConsole::PushToTalk(bool on) {
    auto& app = Application::GetInstance();
    g_talk_down = on;
    if (on) {
        remote_audio::phone_mic.Clear();
        g_ptt_hold = true;
        app.StartListening();
        // hand the buffered audio over as soon as Chitu listens
        xTaskCreate(
            [](void*) {
                auto& a = Application::GetInstance();
                for (int i = 0; i < 60 && g_ptt_hold; i++) {  // up to 6 s
                    if (a.GetDeviceState() == kDeviceStateListening) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                g_ptt_hold = false;
                vTaskDelete(nullptr);
            },
            "ptt_open", 2560, nullptr, 4, nullptr);
        return;
    }
    // released
    xTaskCreate(
        [](void*) {
            auto& a = Application::GetInstance();
            // wait (max 6 s) until Chitu listens and the phone audio has been passed on
            for (int i = 0; i < 60; i++) {
                bool listening = a.GetDeviceState() == kDeviceStateListening;
                if (listening && !g_ptt_hold && remote_audio::phone_mic.Available() < 640) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(250));  // the last words
            if (a.GetDeviceState() == kDeviceStateListening) {
                a.StopListening();
            }
            vTaskDelete(nullptr);
        },
        "ptt_close", 2560, nullptr, 4, nullptr);
}

// Joystick: x/y -100..100, max_speed = top speed. Both wheels share one speed line on
// this board, so the stick picks one of 8 moves (forward, curves, spins, back) and the
// distance from the centre sets the speed. Each command lasts 600 ms: the page repeats
// it while the stick is held, so the robot stops by itself if the phone goes away.
bool WebConsole::DriveJoy(int xi, int yi, int max_speed_in) {
    auto* motors = parts_.motors;
    if (motors == nullptr) {
        return false;
    }
    int max_speed = std::clamp(max_speed_in, 35, 100);
    float x = std::clamp(xi, -100, 100) / 100.0f;
    float y = std::clamp(yi, -100, 100) / 100.0f;
    float mag = std::min(1.0f, sqrtf(x * x + y * y));
    last_drive_us_ = esp_timer_get_time();
    motors->StopRoam();
    motors->StopDance();
    if (parts_.obstacle != nullptr) {
        parts_.obstacle->LogFor(3000);  // TOF readings in the serial log while driving
    }
    if (mag < 0.25f) {
        motors->Stop();
        return true;
    }
    using D = MotorController::Dir;
    int sector = (int)lroundf(atan2f(y, x) / (float)(M_PI / 4)) & 7;
    static const D kMoves[8][2] = {
        {D::kForward, D::kBackward},   // right: spin right
        {D::kForward, D::kStop},       // forward-right: curve
        {D::kForward, D::kForward},    // forward
        {D::kStop, D::kForward},       // forward-left: curve
        {D::kBackward, D::kForward},   // left: spin left
        {D::kStop, D::kBackward},      // back-left
        {D::kBackward, D::kBackward},  // back
        {D::kBackward, D::kStop},      // back-right
    };
    int speed = 35 + (int)lroundf((max_speed - 35) * (mag - 0.25f) / 0.75f);
    return motors->Drive(kMoves[sector][0], kMoves[sector][1], speed, 600);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
esp_err_t WebConsole::HandleCmd(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    char query[256] = {0};
    char action[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        !QueryValue(query, "a", action, sizeof(action))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing a=");
        return ESP_FAIL;
    }
    auto* motors = self->parts_.motors;
    auto* pet = self->parts_.pet;
    if (pet != nullptr && strcmp(action, "video") != 0) {
        pet->Poke();  // the owner is here
    }

    bool ok = true;
    if (strcmp(action, "drive") == 0) {
        char dir[12] = {0};
        QueryValue(query, "d", dir, sizeof(dir));
        int speed = std::clamp(QueryInt(query, "s", 70), 1, 100);
        self->last_drive_us_ = esp_timer_get_time();
        motors->StopRoam();
        motors->StopDance();
        // Short duration: the page re-sends while a button is held, so the robot
        // stops by itself if the phone disconnects (dead-man switch).
        ok = motors->DriveNamed(dir, speed, 600);
    } else if (strcmp(action, "joy") == 0) {
        ok = self->DriveJoy(QueryInt(query, "x", 0), QueryInt(query, "y", 0), QueryInt(query, "s", 75));
    } else if (strcmp(action, "stop") == 0) {
        motors->Stop();  // motors first: safety
    } else if (strcmp(action, "roam") == 0) {
        if (QueryOn(query)) {
            ok = motors->StartRoam(ROAM_MAX_MINUTES);
        } else {
            motors->Stop();
        }
    } else if (strcmp(action, "hush") == 0) {
        // Stop talking / music right now (and any dance that goes with it)
        motors->StopDance();
        chat_bridge::StopTalking();
    } else if (strcmp(action, "volume") == 0) {
        if (self->parts_.codec != nullptr) {
            self->parts_.codec->SetOutputVolume(std::clamp(QueryInt(query, "v", 80), 0, 100));
        }
    } else if (strcmp(action, "voice") == 0) {
        if (self->parts_.set_voice) self->parts_.set_voice(std::clamp(QueryInt(query, "p", 0), 0, 4));
    } else if (strcmp(action, "talk") == 0) {
        self->PushToTalk(QueryOn(query));
    } else if (strcmp(action, "audio") == 0) {
        remote_audio::phone_listen = QueryOn(query);
    } else if (strcmp(action, "spk") == 0) {
        remote_audio::robot_speaker = QueryOn(query);
    } else if (strcmp(action, "room") == 0) {
        remote_audio::room_listen = QueryOn(query);
    } else if (strcmp(action, "mic") == 0) {
        // "Phone mic always": nothing to switch here, the phone simply keeps streaming
    } else if (strcmp(action, "video") == 0) {
        bool on = QueryOn(query);
        self->SetVideo(on);
        ok = !on || self->video_on_;
    } else if (strcmp(action, "alarm_stop") == 0) {
        AlarmManager::GetInstance().Stop("console", false);
    } else if (strcmp(action, "chatty") == 0) {
        if (pet != nullptr) pet->SetChatty(QueryOn(query));
    } else if (strcmp(action, "moves") == 0) {
        if (pet != nullptr) pet->SetMoves(QueryOn(query));
    } else if (strcmp(action, "cam_allow") == 0) {
        if (self->parts_.camera != nullptr) {
            bool on = QueryOn(query);
            if (!on) self->SetVideo(false);
            self->parts_.camera->SetAllowed(on);
        }
    } else if (strcmp(action, "cam") == 0) {
        // Camera orientation: v=vertical flip, h=horizontal mirror (0/1), saved
        auto* cam = self->parts_.camera;
        if (cam == nullptr) {
            ok = false;
        } else {
            char v[4] = {0}, h[4] = {0};
            if (QueryValue(query, "v", v, sizeof(v))) cam->SetVFlip(v[0] == '1');
            if (QueryValue(query, "h", h, sizeof(h))) cam->SetHMirror(h[0] == '1');
        }
    } else if (strcmp(action, "wifi_off") == 0) {
        NetManager::GetInstance().GoOffline(std::clamp(QueryInt(query, "min", 5), 0, 240));
    } else if (strcmp(action, "wifi_pref") == 0) {
        ok = NetManager::GetInstance().MakePreferred(QueryInt(query, "i", -1));
    } else if (strcmp(action, "wifi_del") == 0) {
        ok = NetManager::GetInstance().Remove(QueryInt(query, "i", -1));
    } else if (strcmp(action, "loc_auto") == 0) {
        LocationService::GetInstance().UseAutomatic();
    } else if (strcmp(action, "tz") == 0) {
        char p[96] = {0}, l[96] = {0};
        QueryValue(query, "p", p, sizeof(p));
        QueryValue(query, "l", l, sizeof(l));
        LocationService::GetInstance().SetTimezone(UrlDecode(p), UrlDecode(l));
    } else {
        ok = false;
    }
    return SendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

esp_err_t WebConsole::HandleMsg(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    char query[32] = {0};
    char say[4] = {0};
    bool read_aloud = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                      QueryValue(query, "say", say, sizeof(say)) && say[0] == '1';
    std::string text;
    if (!ReadBody(req, &text, 480) || text.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no text");
        return ESP_FAIL;
    }
    if (self->parts_.pet != nullptr) {
        self->parts_.pet->Poke();
    }
    if (self->on_message_) {
        self->on_message_(text, read_aloud);
    }
    const char* note = "Sent to Chitu \xE2\x9C\x93";
    if (read_aloud && !NetManager::GetInstance().connected()) {
        note = "Shown on screen - reading aloud needs internet";
    } else if (read_aloud) {
        note = "Sent - Chitu will read it out";
    }
    return SendJson(req, std::string("{\"ok\":true,\"note\":\"") + note + "\"}");
}

esp_err_t WebConsole::HandleWifi(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, &body, 300)) {
        return SendJson(req, "{\"ok\":false,\"error\":\"No data\"}");
    }
    std::string error;
    bool ok = NetManager::GetInstance().Add(FormValue(body, "ssid"), FormValue(body, "pw"), &error);
    return SendJson(req, std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" +
                             JsonEscape(error) + "\"}");
}

esp_err_t WebConsole::HandleCity(httpd_req_t* req) {
    char query[160] = {0}, q[120] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        !QueryValue(query, "q", q, sizeof(q))) {
        return SendJson(req, "[]");
    }
    auto* job = new CityJob();
    job->query = UrlDecode(q);
    job->done = xSemaphoreCreateBinary();
    if (job->done == nullptr ||
        xTaskCreate(
            [](void* arg) {
                auto* j = static_cast<CityJob*>(arg);
                j->result = LocationService::GetInstance().SearchCities(j->query);
                xSemaphoreGive(j->done);
                vTaskDelete(nullptr);
            },
            "city_find", 8192, job, 3, nullptr) != pdPASS) {
        if (job->done) vSemaphoreDelete(job->done);
        delete job;
        return SendJson(req, "[]");
    }
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        // the helper still owns the job; it is small, let it leak rather than crash
        return SendJson(req, "[]");
    }
    std::string result = job->result;
    vSemaphoreDelete(job->done);
    delete job;
    return SendJson(req, result);
}

esp_err_t WebConsole::HandleCitySet(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, &body, 400)) {
        return SendJson(req, "{\"ok\":false}");
    }
    std::string name = FormValue(body, "name");
    if (name.empty()) {
        return SendJson(req, "{\"ok\":false}");
    }
    LocationService::GetInstance().SetManual(name, FormValue(body, "region"), FormValue(body, "country"),
                                             atof(FormValue(body, "lat").c_str()),
                                             atof(FormValue(body, "lon").c_str()), FormValue(body, "tz"));
    return SendJson(req, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Alarms
// ---------------------------------------------------------------------------
esp_err_t WebConsole::HandleAlarms(httpd_req_t* req) {
    return SendJson(req, AlarmManager::GetInstance().ListJson());
}

// POST op=save&id=&h=&m=&days=&en=&label=  |  op=del&id=  |  op=en&id=&on=
esp_err_t WebConsole::HandleAlarmsSet(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, &body, 400)) {
        return SendJson(req, "{\"ok\":false,\"error\":\"No data\"}");
    }
    auto& alarms = AlarmManager::GetInstance();
    std::string op = FormValue(body, "op");
    int id = atoi(FormValue(body, "id").c_str());
    bool ok = false;
    std::string error;
    if (op == "save") {
        std::string idv = FormValue(body, "id");
        int new_id = 0;
        ok = alarms.Save(idv.empty() ? -1 : id, atoi(FormValue(body, "h").c_str()),
                         atoi(FormValue(body, "m").c_str()), (uint8_t)atoi(FormValue(body, "days").c_str()),
                         FormValue(body, "label"), FormValue(body, "en") != "0", &new_id, &error);
    } else if (op == "del") {
        ok = alarms.Remove(id);
    } else if (op == "en") {
        ok = alarms.SetEnabled(id, FormValue(body, "on") == "1");
    } else {
        error = "Unknown operation";
    }
    if (!ok && error.empty()) error = "Alarm not found";
    return SendJson(req, std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" +
                             JsonEscape(error) + "\"}");
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
esp_err_t WebConsole::HandleStatus(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    int rssi = 0;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    auto* codec = self->parts_.codec;
    auto* cam = self->parts_.camera;
    int volume = codec ? codec->output_volume() : -1;
    Settings settings("chitu");
    auto state = Application::GetInstance().GetDeviceState();
    std::string json =
        "{\"m\":" + self->parts_.motors->StatusJson() + ",\"rssi\":" + std::to_string(rssi) +
        ",\"volume\":" + std::to_string(volume) + ",\"cam\":\"" + (cam ? cam->state_name() : "none") +
        "\",\"cam_v\":" + std::to_string(settings.GetInt("cam_vflip", CAMERA_VFLIP)) +
        ",\"cam_h\":" + std::to_string(settings.GetInt("cam_hmirror", CAMERA_HMIRROR)) +
        ",\"mic\":" + std::to_string(audio_diag::mic_rms.load()) +
        ",\"mic_clip\":" + (audio_diag::mic_clipped > 0 ? "true" : "false") +
        ",\"phone_mic\":" + (remote_audio::PhoneMicActive() ? "true" : "false") +
        ",\"ir\":" + std::to_string(codec ? codec->input_sample_rate() : 16000) +
        ",\"or\":" + std::to_string(codec ? codec->output_sample_rate() : 24000) +
        ",\"pet\":" + (self->parts_.pet ? self->parts_.pet->StatusJson() : std::string("{}")) +
        ",\"tof\":" + (self->parts_.obstacle ? self->parts_.obstacle->StatusJson() : std::string("{\"st\":\"none\"}")) +
        ",\"alarm\":" + AlarmManager::GetInstance().RingJson() +
        ",\"voice\":" + std::to_string(self->parts_.get_voice ? self->parts_.get_voice() : 0) +
        ",\"net_ok\":" + (NetManager::GetInstance().connected() ? "true" : "false") +
        ",\"st\":\"" + JsonEscape(DeviceStateMachine::GetStateName(state)) + "\"" +
        ",\"wake\":\"" + JsonEscape(audio_diag::WakeWords()) + "\"}";
    return SendJson(req, json);
}

esp_err_t WebConsole::HandleSetup(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    auto* cam = self->parts_.camera;
    std::string json = "{\"net\":" + NetManager::GetInstance().StatusJson() +
                       ",\"loc\":" + LocationService::GetInstance().StatusJson() +
                       ",\"zones\":" + LocationService::ZoneChoicesJson() + ",\"cam\":\"" +
                       (cam ? cam->state_name() : "none") + "\",\"heap_int\":" +
                       std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) +
                       ",\"heap_ps\":" + std::to_string(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024) +
                       ",\"up_min\":" + std::to_string((int)(esp_timer_get_time() / 60000000LL)) + "}";
    return SendJson(req, json);
}

// ---------------------------------------------------------------------------
// WebSocket: camera + audio
// ---------------------------------------------------------------------------
esp_err_t WebConsole::HandleWs(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        return ESP_OK;  // (IDF 6 does the handshake itself and doesn't call us for it)
    }
    // The first frame from a new phone makes it the active one (the newest wins).
    // The page sends a "p" right after connecting and then every second.
    if (fd != self->client_fd_.load()) {
        int old = self->client_fd_.exchange(fd);
        if (old >= 0 && old != fd &&
            httpd_ws_get_fd_info(self->secure_server_, old) == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_sess_trigger_close(self->secure_server_, old);
        }
        self->last_rx_us_ = esp_timer_get_time();
        NetManager::GetInstance().SetBusy(true);
        ESP_LOGI(TAG, "Phone connected (socket %d)", fd);
    }

    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.len > kRxMax || self->rx_buf_ == nullptr) {
        return ESP_FAIL;
    }
    frame.payload = self->rx_buf_;
    if (frame.len > 0 && httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (fd == self->client_fd_.load()) {
        self->last_rx_us_ = esp_timer_get_time();
    }
    if (frame.type == HTTPD_WS_TYPE_BINARY && frame.len > 1 && frame.payload[0] == kWsMicIn) {
        remote_audio::phone_mic.Push(frame.payload + 1, (frame.len - 1) & ~1u);
        remote_audio::last_phone_mic_us = esp_timer_get_time();
    } else if (frame.type == HTTPD_WS_TYPE_TEXT && frame.len > 0 && frame.len < 64 &&
               fd == self->client_fd_.load()) {
        // Driving over the open WebSocket: no new HTTPS request per joystick update,
        // so the stick stays responsive while the camera stream is running.
        //   "j <x> <y> <max speed>"   joystick      "x"   stop now
        char text[64];
        memcpy(text, frame.payload, frame.len);
        text[frame.len] = '\0';
        if (text[0] == 'j') {
            int x = 0, y = 0, sp = 75;
            if (sscanf(text + 1, "%d %d %d", &x, &y, &sp) >= 2) {
                self->DriveJoy(x, y, sp);
            }
        } else if (text[0] == 'x' && self->parts_.motors != nullptr) {
            self->parts_.motors->Stop();
        }
    }
    return ESP_OK;
}

bool WebConsole::EnsureTx(size_t len) {
    if (tx_buf_ != nullptr && tx_cap_ >= len + 1) {
        return true;
    }
    if (tx_buf_ != nullptr) heap_caps_free(tx_buf_);
    tx_cap_ = len + 1 + 8 * 1024;
    tx_buf_ = static_cast<uint8_t*>(heap_caps_malloc(tx_cap_, MALLOC_CAP_SPIRAM));
    if (tx_buf_ == nullptr) {
        tx_cap_ = 0;
        return false;
    }
    return true;
}

bool WebConsole::SendTx(int fd, size_t len) {
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = tx_buf_;
    frame.len = len + 1;
    // Queued to the server task (TLS is not shared between tasks) and waits until sent
    return httpd_ws_send_data(secure_server_, fd, &frame) == ESP_OK;
}

bool WebConsole::SendBinary(int fd, uint8_t type, const void* data, size_t len) {
    if (!EnsureTx(len)) {
        return false;
    }
    tx_buf_[0] = type;
    memcpy(tx_buf_ + 1, data, len);
    return SendTx(fd, len);
}

void WebConsole::DropClient(const char* why) {
    int fd = client_fd_.exchange(-1);
    if (fd >= 0) {
        ESP_LOGI(TAG, "Phone disconnected (%s)", why);
        // only close it if that socket is still our WebSocket (fd numbers get reused)
        if (httpd_ws_get_fd_info(secure_server_, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_sess_trigger_close(secure_server_, fd);
        }
    }
    g_ptt_hold = false;
    remote_audio::ResetRouting();  // robot mic + speaker back to normal
    SetVideo(false);               // nobody watching: camera off soon
    NetManager::GetInstance().SetBusy(false);
}

void WebConsole::MediaLoop() {
    // up to ~330 ms of audio per message so audio keeps up while video frames are sent
    constexpr size_t kPcmBytes = 16000;
    auto* pcm = static_cast<int16_t*>(heap_caps_malloc(kPcmBytes, MALLOC_CAP_SPIRAM));
    if (pcm == nullptr) {
        ESP_LOGE(TAG, "No memory for media task");
        vTaskDelete(nullptr);
        return;
    }
    int64_t last_frame_us = 0;
    int frame_interval_ms = kVideoMinIntervalMs;
    while (true) {
        int fd = client_fd_.load();
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (httpd_ws_get_fd_info(secure_server_, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            DropClient("closed");
            continue;
        }
        // the phone pings every second; allow for a slow phone / busy Wi-Fi
        if (now - last_rx_us_.load() > 8LL * 1000 * 1000) {
            DropClient("no heartbeat");
            continue;
        }
        bool sent = false;
        bool ok = true;

        // Audio first: it is small and latency matters
        size_t n = remote_audio::voice_out.Pop(pcm, kPcmBytes);
        if (n > 0) {
            ok = SendBinary(fd, kWsVoice, pcm, n);
            sent = true;
        }
        n = remote_audio::room_out.Pop(pcm, kPcmBytes);
        if (ok && n > 0) {
            ok = SendBinary(fd, kWsRoom, pcm, n);
            sent = true;
        }

        // Video: the frame rate adapts to how fast Wi-Fi takes the frames, so a slow
        // link gets fewer frames instead of a frozen picture and a dropped connection.
        // While driving it is capped lower so commands and the motors come first.
        bool driving = OwnerDriving() && (parts_.motors == nullptr || parts_.motors->moving() ||
                                          now - last_drive_us_.load() < 1500LL * 1000);
        int min_interval = driving ? kVideoDrivingIntervalMs : kVideoMinIntervalMs;
        if (ok && video_on_ && parts_.camera != nullptr &&
            (now - last_frame_us) / 1000 >= std::max(frame_interval_ms, min_interval)) {
            camera_fb_t* fb = parts_.camera->GetStreamFrame();
            if (fb != nullptr) {
                last_frame_us = now;
                size_t len = fb->len;
                bool fits = fb->format == PIXFORMAT_JPEG && len > 0 && EnsureTx(len);
                if (fits) {
                    tx_buf_[0] = kWsVideo;
                    memcpy(tx_buf_ + 1, fb->buf, len);
                }
                parts_.camera->ReturnStreamFrame(fb);  // copy made: give it back at once
                if (fits) {
                    int64_t t0 = esp_timer_get_time();
                    ok = SendTx(fd, len);
                    int send_ms = (int)((esp_timer_get_time() - t0) / 1000);
                    frame_interval_ms = std::clamp(send_ms * 2, kVideoMinIntervalMs, 1000);
                }
                sent = true;
            } else if (!parts_.camera->allowed()) {
                video_on_ = false;  // blocked by the owner meanwhile
            }
        }
        if (!ok) {
            DropClient("send failed");
            continue;
        }
        if (!sent) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}
