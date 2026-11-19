#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class MotorController;

// Alarms: set from the phone console or by voice, kept in flash (NVS), checked against
// the local time once a second.
//
// When an alarm goes off Chitu, all at the same time and until someone stops it:
//   - plays an alarm tone through the speaker (with pauses so it can still hear you),
//   - says a wake-up line now and then (needs Wi-Fi; tones and moves work without it),
//   - wiggles on the spot (spins left/right in equal amounts, never drives away),
//   - shows wide-awake / excited eyes.
// The longer it rings, the calmer the pattern gets (it keeps ringing until dismissed).
//
// Stop it with: "Chitu ... stop alarm" (the AI calls self.chitu.alarm_stop), the STOP ALARM
// button in the phone console, or the BOOT button on the robot.
class AlarmManager {
public:
    struct Alarm {
        int id = 0;
        int hour = 7;
        int minute = 0;
        uint8_t days = 0;      // bit 0 = Sunday ... bit 6 = Saturday; 0 = once
        bool enabled = true;
        std::string label;
        int64_t last_fired_min = 0;  // epoch minute it last rang (no double ringing)
    };

    struct Hooks {
        MotorController* motors = nullptr;
        std::function<void(const char* emotion)> face;
        std::function<bool()> owner_driving;
        std::function<int()> get_volume;
        std::function<void(int)> set_volume;
    };

    static AlarmManager& GetInstance() {
        static AlarmManager instance;
        return instance;
    }

    void Start(Hooks hooks);

    // Console / voice editing. id < 0 in Save() = new alarm. Returns false + error text.
    bool Save(int id, int hour, int minute, uint8_t days, const std::string& label, bool enabled,
              int* id_out, std::string* error);
    bool Remove(int id);
    bool SetEnabled(int id, bool on);

    bool ringing() const { return ringing_.load(); }
    // Dismiss the ringing alarm. by_voice = the user said so (Chitu may answer, so its
    // speech is not cut off).
    void Stop(const char* how, bool by_voice);

    std::string ListJson();
    std::string RingJson();  // {"ringing":..,"label":..,"since_s":..} for the status poll
    void RegisterMcpTools();

    static bool ParseDays(const std::string& text, uint8_t* days);
    static std::string DaysText(uint8_t days);

private:
    AlarmManager() = default;

    std::mutex mutex_;
    std::vector<Alarm> alarms_;
    Hooks hooks_;
    std::atomic<bool> ringing_{false};
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> ring_start_us_{0};
    std::mutex label_mutex_;  // guards ring_label_ (separate: RingJson runs under mutex_)
    std::string ring_label_;
    std::string label() {
        std::lock_guard<std::mutex> lock(label_mutex_);
        return ring_label_;
    }
    int64_t last_checked_min_ = 0;
    bool started_ = false;

    void Load();
    void SaveAll();  // caller holds mutex_
    void Loop();
    void CheckMinute(int64_t epoch_min, bool catching_up);
    void Fire(const Alarm& alarm);
    void Ring();
    static bool Matches(const Alarm& a, int64_t epoch_min);
    static std::string NextText(const Alarm& a);
    std::string AlarmJson(const Alarm& a);
};
