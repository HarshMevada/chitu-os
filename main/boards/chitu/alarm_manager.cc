#include "alarm_manager.h"

#include "application.h"
#include "assets/lang_config.h"
#include "chat_bridge.h"
#include "config.h"
#include "device_state_machine.h"
#include "mcp_server.h"
#include "motor_controller.h"
#include "net_manager.h"
#include "settings.h"
#include "time_location.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#define TAG "Alarm"

namespace {
constexpr const char* kNs = "alarms";
const char* const kDayNames[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

int64_t NowUs() { return esp_timer_get_time(); }

std::string Clean(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '|' || c == '\n' || c == '\r' || c == '"' || c == '\\') c = ' ';
        out += c;
    }
    if (out.size() > 40) out.resize(40);
    return out;
}

std::string Hhmm(int h, int m) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", h % 24, m % 60);
    return buf;
}

std::string ErrorJson(const std::string& message) {
    return "{\"ok\":false,\"error\":\"" + message + "\"}";
}

// What Chitu says while an alarm rings (the AI words it in Chitu's own style)
const char* const kWakeLines[] = {
    "Hello! It's time for your alarm. Please wake up!",
    "Rise and shine! Your alarm is ringing.",
    "Wakey wakey! It's alarm time.",
    "Good morning! Time to get up, your alarm is going off.",
};
}  // namespace

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
void AlarmManager::Load() {
    Settings settings(kNs);
    std::string list = settings.GetString("list");
    alarms_.clear();
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos) end = list.size();
        std::string line = list.substr(pos, end - pos);
        pos = end + 1;
        // id|hour|minute|days|enabled|last_fired_min|label
        Alarm a;
        char label[48] = {0};
        long long last = 0;
        int en = 1, days = 0;
        int n = sscanf(line.c_str(), "%d|%d|%d|%d|%d|%lld|%47[^\n]", &a.id, &a.hour, &a.minute, &days,
                       &en, &last, label);
        if (n < 6 || a.hour < 0 || a.hour > 23 || a.minute < 0 || a.minute > 59) {
            continue;
        }
        a.days = (uint8_t)(days & 0x7F);
        a.enabled = en != 0;
        a.last_fired_min = last;
        a.label = n >= 7 ? label : "";
        alarms_.push_back(a);
        if ((int)alarms_.size() >= ALARM_MAX) break;
    }
    ESP_LOGI(TAG, "%u alarm(s) loaded", (unsigned)alarms_.size());
}

void AlarmManager::SaveAll() {
    std::string list;
    for (const auto& a : alarms_) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%d|%d|%d|%d|%d|%lld|", a.id, a.hour, a.minute, a.days, a.enabled ? 1 : 0,
                 (long long)a.last_fired_min);
        list += buf;
        list += Clean(a.label);
        list += '\n';
    }
    Settings(kNs, true).SetString("list", list);
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------
bool AlarmManager::Save(int id, int hour, int minute, uint8_t days, const std::string& label, bool enabled,
                        int* id_out, std::string* error) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        if (error) *error = "Invalid time";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Alarm* target = nullptr;
    for (auto& a : alarms_) {
        if (a.id == id) target = &a;
    }
    if (target == nullptr) {
        if ((int)alarms_.size() >= ALARM_MAX) {
            if (error) *error = "All " + std::to_string(ALARM_MAX) + " alarm places are used - delete one first";
            return false;
        }
        int next_id = 1;
        for (const auto& a : alarms_) next_id = std::max(next_id, a.id + 1);
        alarms_.push_back(Alarm());
        target = &alarms_.back();
        target->id = next_id;
    }
    target->hour = hour;
    target->minute = minute;
    target->days = days & 0x7F;
    target->label = Clean(label);
    target->enabled = enabled;
    target->last_fired_min = 0;  // a changed alarm may ring again
    if (id_out) *id_out = target->id;
    SaveAll();
    ESP_LOGI(TAG, "Alarm %d set: %s %s%s", target->id, Hhmm(hour, minute).c_str(), DaysText(target->days).c_str(),
             enabled ? "" : " (off)");
    return true;
}

bool AlarmManager::Remove(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::remove_if(alarms_.begin(), alarms_.end(), [id](const Alarm& a) { return a.id == id; });
    if (it == alarms_.end()) return false;
    alarms_.erase(it, alarms_.end());
    SaveAll();
    ESP_LOGI(TAG, "Alarm %d deleted", id);
    return true;
}

bool AlarmManager::SetEnabled(int id, bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& a : alarms_) {
        if (a.id == id) {
            a.enabled = on;
            a.last_fired_min = 0;
            SaveAll();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Days and times
// ---------------------------------------------------------------------------
bool AlarmManager::ParseDays(const std::string& text, uint8_t* days) {
    std::string t;
    for (char c : text) t += (char)tolower((unsigned char)c);
    if (t.empty() || t == "once" || t == "one time" || t == "none") {
        *days = 0;
        return true;
    }
    if (t == "daily" || t == "every day" || t == "everyday" || t == "all") {
        *days = 0x7F;
        return true;
    }
    if (t == "weekdays" || t == "weekday" || t == "mon-fri") {
        *days = 0x3E;
        return true;
    }
    if (t == "weekends" || t == "weekend") {
        *days = 0x41;
        return true;
    }
    uint8_t mask = 0;
    for (int d = 0; d < 7; d++) {
        if (t.find(kDayNames[d]) != std::string::npos) mask |= (1 << d);
    }
    if (mask == 0) return false;
    *days = mask;
    return true;
}

std::string AlarmManager::DaysText(uint8_t days) {
    days &= 0x7F;
    if (days == 0) return "once";
    if (days == 0x7F) return "daily";
    if (days == 0x3E) return "weekdays";
    if (days == 0x41) return "weekends";
    std::string out;
    for (int d = 1; d <= 7; d++) {  // Monday first
        int b = d % 7;
        if (days & (1 << b)) {
            if (!out.empty()) out += ",";
            out += kDayNames[b];
        }
    }
    return out;
}

bool AlarmManager::Matches(const Alarm& a, int64_t epoch_min) {
    time_t t = (time_t)(epoch_min * 60);
    struct tm tm_local = {};
    localtime_r(&t, &tm_local);
    if (tm_local.tm_hour != a.hour || tm_local.tm_min != a.minute) return false;
    return a.days == 0 || (a.days & (1 << tm_local.tm_wday)) != 0;
}

std::string AlarmManager::NextText(const Alarm& a) {
    if (!a.enabled) return "off";
    if (!LocationService::GetInstance().time_valid()) return "waiting for the clock";
    time_t now = time(nullptr);
    struct tm today = {};
    localtime_r(&now, &today);
    for (int d = 0; d <= 7; d++) {
        struct tm c = today;
        c.tm_mday += d;
        c.tm_hour = a.hour;
        c.tm_min = a.minute;
        c.tm_sec = 0;
        c.tm_isdst = -1;
        time_t when = mktime(&c);
        if (when <= now) continue;
        struct tm w = {};
        localtime_r(&when, &w);
        if (a.days != 0 && !(a.days & (1 << w.tm_wday))) continue;
        static const char* kNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        std::string day = d == 0 ? "Today" : (d == 1 ? "Tomorrow" : kNames[w.tm_wday]);
        return day + " " + Hhmm(a.hour, a.minute);
    }
    return "";
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------
void AlarmManager::Start(Hooks hooks) {
    if (started_) return;
    started_ = true;
    hooks_ = std::move(hooks);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Load();
    }
    xTaskCreate([](void* arg) { static_cast<AlarmManager*>(arg)->Loop(); }, "chitu_alarm", 4096, this, 3,
                nullptr);
}

void AlarmManager::Loop() {
    bool boot_checked = false;
    bool warned = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!LocationService::GetInstance().time_valid()) {
            if (!warned) {
                ESP_LOGW(TAG, "Clock not set yet (no internet time) - alarms wait for it");
                warned = true;
            }
            continue;
        }
        int64_t now_min = (int64_t)time(nullptr) / 60;
        if (!boot_checked) {
            boot_checked = true;
            // An alarm that was ringing when the power went: ring again if it was recent
            Settings settings(kNs);
            std::string ring = settings.GetString("ring");
            if (!ring.empty()) {
                long long start = 0;
                int id = 0;
                char label[48] = {0};
                int n = sscanf(ring.c_str(), "%d|%lld|%47[^\n]", &id, &start, label);
                long long age_s = (long long)time(nullptr) - start;
                if (n >= 2 && age_s >= 0 && age_s < (long long)ALARM_RESUME_MINUTES * 60) {
                    ESP_LOGW(TAG, "Alarm %d was ringing at power-off %lld s ago - ringing again", id, age_s);
                    Alarm a;
                    a.id = id;
                    a.label = n >= 3 ? label : "";
                    Fire(a);
                } else {
                    Settings(kNs, true).EraseKey("ring");
                }
            }
            // Alarms missed while switched off (only recent ones)
            std::lock_guard<std::mutex> lock(mutex_);
            for (int64_t m = now_min - ALARM_CATCH_UP_MINUTES; m < now_min; m++) {
                CheckMinute(m, true);
            }
            last_checked_min_ = now_min - 1;
        }
        if (now_min != last_checked_min_) {
            std::lock_guard<std::mutex> lock(mutex_);
            // normal step is one minute; after a big clock correction only check "now"
            if (now_min < last_checked_min_ || now_min - last_checked_min_ > 3) {
                last_checked_min_ = now_min - 1;
            }
            for (int64_t m = last_checked_min_ + 1; m <= now_min; m++) {
                CheckMinute(m, false);
            }
            last_checked_min_ = now_min;
        }
    }
}

void AlarmManager::CheckMinute(int64_t epoch_min, bool catching_up) {
    bool changed = false;
    for (auto& a : alarms_) {
        if (!a.enabled || a.last_fired_min >= epoch_min || !Matches(a, epoch_min)) continue;
        a.last_fired_min = epoch_min;
        if (a.days == 0) a.enabled = false;  // one-time alarm: done
        changed = true;
        ESP_LOGI(TAG, "Alarm %d (%s) %s", a.id, Hhmm(a.hour, a.minute).c_str(),
                 catching_up ? "was missed while off - ringing now" : "is due");
        Fire(a);
    }
    if (changed) SaveAll();
}

void AlarmManager::Fire(const Alarm& alarm) {
    std::string clean = Clean(alarm.label);
    {
        std::lock_guard<std::mutex> lock(label_mutex_);
        ring_label_ = clean;
    }
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "%d|%lld|", alarm.id, (long long)time(nullptr));
        Settings(kNs, true).SetString("ring", std::string(buf) + clean);
    }
    if (ringing_.exchange(true)) {
        return;  // already ringing: carries on
    }
    stop_ = false;
    ring_start_us_ = NowUs();
    if (xTaskCreate([](void* arg) {
            static_cast<AlarmManager*>(arg)->Ring();
            vTaskDelete(nullptr);
        },
                    "alarm_ring", 4096, this, 3, nullptr) != pdPASS) {
        ringing_ = false;
        ESP_LOGE(TAG, "Could not start ringing");
    }
}

// ---------------------------------------------------------------------------
// Ringing
// ---------------------------------------------------------------------------
void AlarmManager::Ring() {
    auto& app = Application::GetInstance();
    const int64_t kS = 1000 * 1000;
    int64_t t0 = NowUs();
    int prev_volume = hooks_.get_volume ? hooks_.get_volume() : -1;
    if (prev_volume >= 0 && prev_volume < ALARM_MIN_VOLUME && hooks_.set_volume) {
        hooks_.set_volume(ALARM_MIN_VOLUME);
    }
    // Stop anything Chitu is doing on its own (dancing, roaming)
    if (hooks_.motors != nullptr && !(hooks_.owner_driving && hooks_.owner_driving())) {
        hooks_.motors->StopDance();
        hooks_.motors->StopRoam();
    }
    ESP_LOGW(TAG, "ALARM RINGING%s%s - say \"stop alarm\", press STOP ALARM in the console or the BOOT button",
             label().empty() ? "" : ": ", label().c_str());

    int64_t next_tone = t0, next_voice = t0 + 5 * kS, next_move = t0 + 2 * kS, next_face = t0;
    int face_i = 0, line_i = (int)(esp_random() % 4);
    static const char* kFaces[] = {"surprised", "excited", "shocked", "excited"};

    while (!stop_) {
        int64_t now = NowUs();
        int elapsed = (int)((now - t0) / kS);
        // Calmer the longer it rings (it never gives up by itself)
        int tone_every = elapsed < 300 ? 9 : (elapsed < 900 ? 20 : 40);
        int voice_every = elapsed < 300 ? 60 : (elapsed < 900 ? 120 : 300);
        int move_every = elapsed < 300 ? 15 : (elapsed < 900 ? 45 : 0);
        bool idle = app.GetDeviceState() == kDeviceStateIdle && !chat_bridge::Pending();

        if (!idle) {
            // A conversation is going on (maybe "stop alarm"): stay quiet and still,
            // and give the listener a few seconds after it ends
            next_tone = std::max(next_tone, now + 6 * kS);
            next_move = std::max(next_move, now + 6 * kS);
        } else {
            if (now >= next_face && hooks_.face) {
                hooks_.face(kFaces[face_i++ % 4]);
                next_face = now + 3 * kS;
            }
            if (now >= next_voice && NetManager::GetInstance().connected()) {
                std::string name = label();
                std::string about = name.empty() ? "" : " The alarm is labelled \"" + name + "\".";
                std::string prompt =
                    std::string("(An alarm the user set is ringing right now.") + about +
                    " Say ONE short, cheerful wake-up line in your own words, for example: \"" +
                    kWakeLines[line_i++ % 4] +
                    "\" Then tell them to say 'stop alarm' to turn it off. Only if they ask to stop or "
                    "dismiss the alarm, call self.chitu.alarm_stop.)";
                chat_bridge::Say(chat_bridge::Kind::kPrompt, prompt, false);
                next_voice = now + (int64_t)voice_every * kS;
                next_tone = std::max(next_tone, now + 10 * kS);  // let it speak first
            } else if (now >= next_tone) {
                // ~1.8 s tone, twice, then a gap so the wake word can still be heard
                app.PlaySound(Lang::Sounds::OGG_ALARM);
                app.PlaySound(Lang::Sounds::OGG_ALARM);
                next_tone = now + (int64_t)tone_every * kS;
            }
            if (ALARM_MOVES && move_every > 0 && now >= next_move && hooks_.motors != nullptr &&
                !hooks_.motors->moving() && !(hooks_.owner_driving && hooks_.owner_driving())) {
                // Equal left and right spins: lively, but Chitu stays where it is
                using D = MotorController::Dir;
                for (int i = 0; i < 2 && !stop_; i++) {
                    hooks_.motors->Drive(D::kBackward, D::kForward, 55, 220);
                    vTaskDelay(pdMS_TO_TICKS(280));
                    hooks_.motors->Drive(D::kForward, D::kBackward, 55, 220);
                    vTaskDelay(pdMS_TO_TICKS(280));
                }
                next_move = now + (int64_t)move_every * kS;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (prev_volume >= 0 && prev_volume < ALARM_MIN_VOLUME && hooks_.set_volume) {
        hooks_.set_volume(prev_volume);
    }
    if (hooks_.face) hooks_.face("happy");
    ringing_ = false;
    ESP_LOGI(TAG, "Alarm stopped after %d s", (int)((NowUs() - t0) / kS));
}

void AlarmManager::Stop(const char* how, bool by_voice) {
    if (!ringing_.load() || stop_.exchange(true)) {
        return;
    }
    ESP_LOGI(TAG, "Alarm dismissed (%s)", how);
    Settings(kNs, true).EraseKey("ring");
    if (hooks_.motors != nullptr && !(hooks_.owner_driving && hooks_.owner_driving())) {
        hooks_.motors->Stop();
    }
    if (!by_voice) {
        chat_bridge::StopTalking();  // cut the wake-up line short
    }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
std::string AlarmManager::AlarmJson(const Alarm& a) {
    return "{\"id\":" + std::to_string(a.id) + ",\"h\":" + std::to_string(a.hour) + ",\"m\":" +
           std::to_string(a.minute) + ",\"time\":\"" + Hhmm(a.hour, a.minute) + "\",\"days\":" +
           std::to_string(a.days) + ",\"repeat\":\"" + DaysText(a.days) + "\",\"en\":" +
           (a.enabled ? "true" : "false") + ",\"label\":\"" + Clean(a.label) + "\",\"next\":\"" + NextText(a) +
           "\"}";
}

std::string AlarmManager::RingJson() {
    int since = ringing_ ? (int)((NowUs() - ring_start_us_.load()) / 1000000) : 0;
    return std::string("{\"ringing\":") + (ringing_ ? "true" : "false") + ",\"label\":\"" +
           (ringing_ ? label() : "") + "\",\"since_s\":" + std::to_string(since) + "}";
}

std::string AlarmManager::ListJson() {
    bool ok = LocationService::GetInstance().time_valid();
    std::string now = "--:--";
    if (ok) {
        time_t t = time(nullptr);
        struct tm tm_local = {};
        localtime_r(&t, &tm_local);
        now = Hhmm(tm_local.tm_hour, tm_local.tm_min);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::string json = std::string("{\"time_ok\":") + (ok ? "true" : "false") + ",\"now\":\"" + now +
                       "\",\"max\":" + std::to_string(ALARM_MAX) + ",\"ring\":" + RingJson() + ",\"alarms\":[";
    for (size_t i = 0; i < alarms_.size(); i++) {
        if (i) json += ",";
        json += AlarmJson(alarms_[i]);
    }
    return json + "]}";
}

// ---------------------------------------------------------------------------
// AI tools
// ---------------------------------------------------------------------------
void AlarmManager::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.chitu.alarm_set",
        "Set an alarm on Chitu (this robot). Use when the user asks to wake them up, set an alarm or "
        "be reminded at a clock time. time: 24-hour local time HH:MM (e.g. 06:30, 19:05) - work it out "
        "from self.chitu.get_time_and_place for relative requests like 'in 20 minutes'. repeat: once, "
        "daily, weekdays, weekends, or days like 'mon,wed,fri' (default once). label: optional short "
        "name. When it goes off Chitu plays an alarm tone, speaks, wiggles and keeps ringing until the "
        "user says 'stop alarm'.",
        PropertyList({
            Property("time", kPropertyTypeString, std::string("07:00")),
            Property("repeat", kPropertyTypeString, std::string("once")),
            Property("label", kPropertyTypeString, std::string("")),
        }),
        [this](const PropertyList& p) -> ReturnValue {
            std::string t = p["time"].value<std::string>();
            int h = -1, m = -1;
            if (sscanf(t.c_str(), "%d:%d", &h, &m) != 2) {
                return ErrorJson("Use the time as HH:MM in 24-hour format, e.g. 06:30");
            }
            uint8_t days = 0;
            if (!ParseDays(p["repeat"].value<std::string>(), &days)) {
                return ErrorJson("repeat must be once, daily, weekdays, weekends or day names like mon,wed");
            }
            int id = 0;
            std::string error;
            if (!Save(-1, h, m, days, p["label"].value<std::string>(), true, &id, &error)) {
                return ErrorJson(error);
            }
            Alarm a;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& x : alarms_) if (x.id == id) a = x;
            }
            std::string note = LocationService::GetInstance().time_valid()
                                   ? ""
                                   : ",\"note\":\"My clock is not set yet; the alarm works once I have internet time.\"";
            return "{\"ok\":true,\"alarm\":" + AlarmJson(a) + note + "}";
        });

    mcp.AddTool("self.chitu.alarm_list", "List Chitu's alarms with their id, time, repeat days, on/off and next ring time.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue { return ListJson(); });

    mcp.AddTool("self.chitu.alarm_delete",
                "Delete one of Chitu's alarms by id (get the id from self.chitu.alarm_list).",
                PropertyList({Property("id", kPropertyTypeInteger, 1, 1, 1000)}),
                [this](const PropertyList& p) -> ReturnValue {
                    return Remove(p["id"].value<int>()) ? std::string("{\"ok\":true}")
                                                        : ErrorJson("No alarm with that id");
                });

    mcp.AddTool("self.chitu.alarm_enable", "Switch one of Chitu's alarms on or off without deleting it.",
                PropertyList({Property("id", kPropertyTypeInteger, 1, 1, 1000),
                              Property("enabled", kPropertyTypeBoolean, true)}),
                [this](const PropertyList& p) -> ReturnValue {
                    return SetEnabled(p["id"].value<int>(), p["enabled"].value<bool>())
                               ? std::string("{\"ok\":true}")
                               : ErrorJson("No alarm with that id");
                });

    mcp.AddTool(
        "self.chitu.alarm_stop",
        "Stop (dismiss) the alarm that is ringing right now. Call this ONLY when the user clearly asks "
        "to stop, turn off, silence or dismiss the alarm (e.g. 'stop alarm', 'turn off the alarm', "
        "'dismiss alarm', 'I'm awake, stop it'). Do not call it for other requests.",
        PropertyList(), [this](const PropertyList&) -> ReturnValue {
            if (!ringing()) {
                return std::string("{\"ok\":true,\"note\":\"No alarm is ringing.\"}");
            }
            Stop("voice", true);
            return std::string("{\"ok\":true,\"stopped\":true}");
        });
}
