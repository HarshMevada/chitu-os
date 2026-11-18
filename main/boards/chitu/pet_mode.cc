#include "pet_mode.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_fx.h"
#include "chat_bridge.h"
#include "config.h"
#include "display.h"
#include "mcp_server.h"
#include "motor_controller.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <ctime>
#include <string>

#define TAG "Personality"

namespace {
constexpr int64_t kSec = 1000LL * 1000;
constexpr int64_t kMin = 60 * kSec;
int64_t Now() { return esp_timer_get_time(); }
uint32_t Rand(uint32_t n) { return n ? esp_random() % n : 0; }
int64_t RandSec(int lo, int hi) { return (lo + (int)Rand((uint32_t)(hi - lo + 1))) * kSec; }

using Mood = PetMode::Mood;

const char* MoodName(Mood m) {
    switch (m) {
        case Mood::kCalm: return "calm";
        case Mood::kHappy: return "happy";
        case Mood::kCurious: return "curious";
        case Mood::kPlayful: return "playful";
        case Mood::kBored: return "bored";
        case Mood::kSleepy: return "sleepy";
        case Mood::kAsleep: return "asleep";
        case Mood::kExcited: return "excited";
        case Mood::kSurprised: return "surprised";
        case Mood::kCautious: return "cautious";
    }
    return "calm";
}

// Face shown while a mood lasts (eyes shapes in chitu_eyes_display.cc)
const char* BaseFace(Mood m) {
    switch (m) {
        case Mood::kCurious: return "curious";
        case Mood::kBored: return "bored";
        case Mood::kSleepy: return "sleepy";
        case Mood::kAsleep: return "pet_sleep";
        case Mood::kExcited: return "happy";
        case Mood::kSurprised: return "alert";
        case Mood::kCautious: return "cautious";
        default: return "neutral";
    }
}

// What Chitu says on its own (through the AI, so the words vary every time)
const char* const kBoredPrompts[] = {
    "(You have been alone for a few minutes and feel a bit bored. Say one short, cute line to "
    "whoever is nearby, like a little pet robot asking for attention. One sentence.)",
    "(You are bored. Say one short funny thing about being a tiny robot on a desk. One sentence.)",
    "(You are bored. Suggest one tiny game or activity you could do together, in one short sentence.)",
    "(You are idle. Tell one tiny surprising fun fact, excitedly, in one short sentence.)",
    "(Nobody is talking to you. Ask the people nearby one short, friendly question about their day.)",
    "(You are bored. Hum a tiny tune with sounds like 'beep boop' and say you could dance. Keep it short.)",
};
const char* const kPlayfulPrompts[] = {
    "(You feel playful. Say one short cheeky line, like a playful pet robot. One sentence.)",
    "(You feel playful. Make up a two-line rhyme about yourself. Keep it very short.)",
    "(You feel playful. Ask if anyone wants to see you dance, in one short excited sentence.)",
    "(You feel playful. Tell a very short robot joke.)",
};
const char* const kCuriousPrompts[] = {
    "(Something just appeared right in front of you. Say one short curious line asking what it "
    "is. You cannot see it, only sense it, so do not guess what it is.)",
    "(Something is very close in front of you. Ask in one short, curious sentence who or what is there.)",
};
const char* const kStartlePrompts[] = {
    "(You just heard a sudden loud sound. React in a few startled words, like 'What was that?')",
    "(A loud noise just made you jump. Say one short, surprised sentence about it.)",
};
const char* const kWakePrompts[] = {
    "(You just woke up from a nap because someone came back. Say one short, happy, sleepy line.)",
};
const char* const kSleepyPrompts[] = {
    "(You are getting very sleepy. Yawn and say one short sleepy line before your nap.)",
};

// Behaviours (indexes are remembered so the same one is not repeated back to back)
enum Behaviour {
    kGlance, kFace, kWiggle, kSpinLook, kNod, kPeek, kChirp, kSigh, kYawn, kStretch, kTalk, kClock,
    kBehaviourCount
};
const char* const kBehaviourNames[] = {"glance", "face", "wiggle", "spin-look", "nod", "peek",
                                       "chirp", "sigh", "yawn", "stretch", "talk", "clock"};
}  // namespace

// ---------------------------------------------------------------------------
// Setup and settings
// ---------------------------------------------------------------------------
void PetMode::Start(MotorController* motors, Display* display, Hooks hooks) {
    motors_ = motors;
    display_ = display;
    hooks_ = std::move(hooks);
    {
        Settings settings("chitu");
        enabled_ = settings.GetInt("pet_on", 1) != 0;
        chatty_ = settings.GetInt("pet_chat", 1) != 0;
        moves_ = settings.GetInt("pet_moves", PET_MOVES) != 0;
    }
    int64_t now = Now();
    last_active_us_ = now;
    next_behaviour_us_ = now + 20 * kSec;
    next_chat_us_ = now + (int64_t)PET_CHAT_MIN_MINUTES * kMin;
    next_clock_us_ = now + 60 * kSec;
    xTaskCreate([](void* arg) { static_cast<PetMode*>(arg)->Loop(); }, "chitu_pet", 4096, this, 2,
                nullptr);
}

void PetMode::SetEnabled(bool on) {
    enabled_ = on;
    Settings("chitu", true).SetInt("pet_on", on ? 1 : 0);
    Poke();
}

void PetMode::SetChatty(bool on) {
    chatty_ = on;
    Settings("chitu", true).SetInt("pet_chat", on ? 1 : 0);
}

void PetMode::SetMoves(bool on) {
    moves_ = on;
    Settings("chitu", true).SetInt("pet_moves", on ? 1 : 0);
}

void PetMode::Poke() {
    last_active_us_ = Now();
    poked_ = true;
}

const char* PetMode::mood_name() const { return MoodName(mood_.load()); }

std::string PetMode::StatusJson() const {
    int idle_s = (int)((Now() - last_active_us_.load()) / kSec);
    return std::string("{\"on\":") + (enabled_ ? "true" : "false") + ",\"chatty\":" +
           (chatty_ ? "true" : "false") + ",\"moves\":" + (moves_ ? "true" : "false") +
           ",\"mood\":\"" + mood_name() + "\",\"idle_s\":" + std::to_string(idle_s) +
           ",\"last\":\"" + last_behaviour_name_ + "\",\"music\":" +
           (audio_fx::sense.music ? "true" : "false") + ",\"beat_ms\":" +
           std::to_string(audio_fx::sense.beat_ms.load()) + ",\"room\":" +
           std::to_string(audio_fx::sense.level.load()) + "}";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void PetMode::Pause(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

bool PetMode::QuietHours() const {
    time_t t = time(nullptr);
    if (t < 1700000000) {
        return false;  // clock not set yet
    }
    struct tm local;
    localtime_r(&t, &local);
    int h = local.tm_hour;
    if (PET_QUIET_FROM_HOUR > PET_QUIET_TO_HOUR) {
        return h >= PET_QUIET_FROM_HOUR || h < PET_QUIET_TO_HOUR;
    }
    return h >= PET_QUIET_FROM_HOUR && h < PET_QUIET_TO_HOUR;
}

bool PetMode::Idle() const {
    auto& app = Application::GetInstance();
    return app.GetDeviceState() == kDeviceStateIdle && !motors_->moving() && !motors_->roaming() &&
           !(motors_->dancing() && music_dance_until_us_ == 0) && !chat_bridge::Pending();
}

bool PetMode::CanMove(bool forward) const {
    if (!PET_MOVES || !moves_ || Now() < next_move_us_ || motors_->moving() || motors_->roaming()) {
        return false;
    }
    if (hooks_.owner_driving && hooks_.owner_driving()) {
        return false;
    }
    if (forward && hooks_.distance_mm) {
        int d = hooks_.distance_mm();
        if (d >= 0 && d < 300) {
            return false;
        }
    }
    return true;
}

bool PetMode::CanTalk() const { return chatty_ && !QuietHours(); }

void PetMode::ShowFace(const char* face) {
    if (display_ != nullptr && !(hooks_.overlay_visible && hooks_.overlay_visible())) {
        display_->SetEmotion(face);
    }
}

void PetMode::ShowBaseFace() {
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        ShowFace(BaseFace(mood_.load()));
    }
}

void PetMode::FaceFor(const char* face, int ms) {
    ShowFace(face);
    Pause(ms);
    ShowBaseFace();
}

void PetMode::SetMood(Mood m, int seconds) {
    Mood old = mood_.exchange(m);
    mood_until_us_ = seconds > 0 ? Now() + seconds * kSec : 0;
    if (old != m) {
        ESP_LOGI(TAG, "Mood: %s -> %s", MoodName(old), MoodName(m));
        ShowBaseFace();
        // something new to do soon after a change of mood
        int64_t soon = Now() + RandSec(3, 8);
        if (next_behaviour_us_ > soon) {
            next_behaviour_us_ = soon;
        }
    }
}

void PetMode::PlaySound(const char* which) {
    if (QuietHours()) {
        return;
    }
    auto& app = Application::GetInstance();
    std::string w = which;
    if (w == "chirp") app.PlaySound(Lang::Sounds::OGG_CHIRP);
    else if (w == "giggle") app.PlaySound(Lang::Sounds::OGG_GIGGLE);
    else if (w == "yawn") app.PlaySound(Lang::Sounds::OGG_YAWN);
    else if (w == "huh") app.PlaySound(Lang::Sounds::OGG_HUH);
    else if (w == "happy") app.PlaySound(Lang::Sounds::OGG_SUCCESS);
    else app.PlaySound(Lang::Sounds::OGG_POPUP);
}

void PetMode::Talk(const char* const* prompts, int count) {
    // pick one that was not used recently
    int pick = (int)Rand((uint32_t)count);
    for (int tries = 0; tries < 6; tries++) {
        int key = (int)(reinterpret_cast<uintptr_t>(prompts) & 0xFFF) * 16 + pick;
        bool recent = false;
        for (int r : last_prompts_) {
            if (r == key) recent = true;
        }
        if (!recent) {
            for (int i = 5; i > 0; i--) last_prompts_[i] = last_prompts_[i - 1];
            last_prompts_[0] = key;
            break;
        }
        pick = (pick + 1) % count;
    }
    if (chat_bridge::Say(chat_bridge::Kind::kPrompt, prompts[pick], false)) {
        ESP_LOGI(TAG, "Speaks up on its own");
    }
}

void PetMode::Wiggle(int times, int speed) {
    using D = MotorController::Dir;
    next_move_us_ = Now() + 45 * kSec;
    for (int i = 0; i < times; i++) {
        motors_->Drive(D::kBackward, D::kForward, speed, 150);
        Pause(200);
        motors_->Drive(D::kForward, D::kBackward, speed, 150);
        Pause(200);
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void PetMode::Loop() {
    DeviceState last_state = kDeviceStateUnknown;
    while (true) {
        Pause(200);
        auto& app = Application::GetInstance();
        DeviceState state = app.GetDeviceState();
        int64_t now = Now();

        // Anything happening with Chitu counts as attention
        bool active = state != kDeviceStateIdle || motors_->moving() || motors_->roaming() ||
                      (motors_->dancing() && music_dance_until_us_ == 0) ||
                      (hooks_.owner_driving && hooks_.owner_driving());
        if (active) {
            last_active_us_ = now;
        }
        bool was_asleep = mood_.load() == Mood::kAsleep;
        if (poked_.exchange(false) || (active && state != kDeviceStateIdle)) {
            if (was_asleep) {
                // woken up: surprised, then happy
                SetMood(Mood::kExcited, 20);
                if (enabled_ && state == kDeviceStateIdle) {
                    FaceFor("surprised", 700);
                    PlaySound("happy");
                    if (CanTalk() && Rand(3) == 0) Talk(kWakePrompts, 1);
                }
            } else if (mood_.load() == Mood::kBored || mood_.load() == Mood::kSleepy) {
                SetMood(Mood::kHappy, 30);
            }
        }
        if (state != kDeviceStateIdle) {
            last_state = state;
            continue;  // the conversation owns the face now
        }
        if (last_state != kDeviceStateIdle) {
            last_state = state;
            next_behaviour_us_ = now + RandSec(8, 15);  // a moment of calm after talking
        }
        if (!enabled_) {
            // Pet mode off: only the clock now and then
            if (CLOCK_EVERY_SECONDS > 0 && now >= next_clock_us_ && hooks_.show_clock) {
                next_clock_us_ = now + (int64_t)CLOCK_EVERY_SECONDS * kSec;
                hooks_.show_clock();
            }
            continue;
        }

        HandleSound(now);
        HandleMusic(now);
        HandleDistance(now);
        UpdateMoodFromIdle(now - last_active_us_.load());
        if (now >= next_behaviour_us_ && Idle() && !(hooks_.overlay_visible && hooks_.overlay_visible())) {
            DoBehaviour(now);
        }
    }
}

void PetMode::UpdateMoodFromIdle(int64_t idle) {
    Mood m = mood_.load();
    int64_t now = Now();
    if (mood_until_us_ != 0 && now < mood_until_us_) {
        return;  // a reaction (surprised, excited...) is still going on
    }
    if (mood_until_us_ != 0 && now >= mood_until_us_) {
        mood_until_us_ = 0;
        if (m == Mood::kSurprised) {
            SetMood(Mood::kCautious, 15);  // a little wary after a fright
            return;
        }
    }
    // Night: everything goes faster towards sleep
    int64_t bored = (int64_t)PET_BORED_AFTER_MINUTES * kMin;
    int64_t sleepy = (int64_t)PET_SLEEPY_AFTER_MINUTES * kMin;
    int64_t asleep = (int64_t)PET_SLEEP_AFTER_MINUTES * kMin;
    if (QuietHours()) {
        bored /= 3;
        sleepy /= 3;
        asleep /= 3;
    }
    Mood want;
    if (idle >= asleep) {
        want = Mood::kAsleep;
    } else if (idle >= sleepy) {
        want = Mood::kSleepy;
    } else if (idle >= bored) {
        // bored or playful: decided once per stretch, playful more often when it is day
        want = (m == Mood::kBored || m == Mood::kPlayful) ? m : (Rand(3) == 0 ? Mood::kPlayful : Mood::kBored);
    } else {
        want = (m == Mood::kHappy || m == Mood::kCurious || m == Mood::kCalm) ? m : Mood::kCalm;
    }
    if (want == m) {
        return;
    }
    if (want == Mood::kAsleep) {
        // falling asleep: a yawn, droopy eyes, then eyes shut
        ShowFace("yawn");
        PlaySound("yawn");
        Pause(1500);
        ShowFace("sleepy");
        Pause(2500);
        if (Now() - last_active_us_.load() < asleep) {
            return;  // someone came back meanwhile
        }
    } else if (want == Mood::kSleepy && CanTalk() && Rand(2) == 0) {
        Talk(kSleepyPrompts, 1);
    }
    SetMood(want);
}

// Sudden loud sound -> startled
void PetMode::HandleSound(int64_t now) {
#if PET_LOUD_SOUND_REACT
    uint32_t count = audio_fx::sense.loud_count.load();
    if (count == last_loud_count_) {
        return;
    }
    last_loud_count_ = count;
    if (!Idle()) {
        return;
    }
    ESP_LOGI(TAG, "Loud sound!");
    last_active_us_ = now;  // it woke us up
    SetMood(Mood::kSurprised, 4);
    last_behaviour_name_ = "startled";
    ShowFace("alert");
    if (hooks_.look) {
        hooks_.look(Rand(2) ? -0.9f : 0.9f, -0.3f);
        Pause(500);
        hooks_.look(Rand(2) ? 0.8f : -0.8f, 0.0f);
        Pause(500);
        hooks_.look(0, 0);
    }
    if (CanMove(false) && Rand(2) == 0) {
        using D = MotorController::Dir;
        next_move_us_ = now + 45 * kSec;
        motors_->Drive(D::kBackward, D::kBackward, 50, 150);  // a little jump back
    }
    if (CanTalk() && now >= next_startle_talk_us_) {
        next_startle_talk_us_ = now + 10 * kMin;
        Talk(kStartlePrompts, 2);
    } else {
        PlaySound("huh");
    }
#else
    (void)now;
#endif
}

// Music nearby -> dance to it
void PetMode::HandleMusic(int64_t now) {
    bool music = audio_fx::sense.music.load();
    if (music_dance_until_us_ != 0) {
        // dancing to the room's music: stop when it stops (or after a while)
        if (!music || now > music_dance_until_us_ || !motors_->dancing()) {
            if (motors_->dancing()) motors_->StopDance();
            music_dance_until_us_ = 0;
            next_music_dance_us_ = now + 2 * kMin;
            SetMood(Mood::kHappy, 20);
        }
        return;
    }
#if PET_MUSIC_DANCE
    if (!music || now < next_music_dance_us_ || !Idle() || mood_.load() == Mood::kAsleep) {
        return;
    }
    int beat = audio_fx::sense.beat_ms.load();
    ESP_LOGI(TAG, "Music detected (beat ~%d ms) - dancing", beat);
    last_active_us_ = now;
    last_behaviour_name_ = "dance to music";
    SetMood(Mood::kExcited, 40);
    ShowFace("excited");
    if (CanMove(false)) {
        music_dance_until_us_ = now + 40 * kSec;
        motors_->StartDance(40, beat);
    } else {
        // no moves allowed: dance with the eyes only
        next_music_dance_us_ = now + 30 * kSec;
        for (int i = 0; i < 6; i++) {
            ShowFace(i % 2 ? "happy" : "excited");
            Pause(beat > 0 ? beat * 2 : 800);
        }
        ShowBaseFace();
    }
#endif
}

// Something in front of the distance sensor
void PetMode::HandleDistance(int64_t now) {
    if (!hooks_.distance_mm || motors_->moving()) {
        return;
    }
    int d = hooks_.distance_mm();
    if (d < 0) {
        return;
    }
    // A hand held very close for a moment = a pat
    if (d < 70) {
        if (pat_start_us_ == 0) pat_start_us_ = now;
        if (now - pat_start_us_ > 400 * 1000 && now - last_pat_us_ > 20 * kSec && Idle()) {
            last_pat_us_ = now;
            last_active_us_ = now;
            ESP_LOGI(TAG, "Pat!");
            last_behaviour_name_ = "pat";
            SetMood(Mood::kHappy, 30);
            FaceFor("loving", 2200);
            PlaySound("giggle");
        }
    } else {
        pat_start_us_ = 0;
    }
    // Something new appears in front -> curious
    if (!object_near_ && d < 250) {
        object_near_ = true;
        if (Idle() && mood_.load() != Mood::kAsleep && now - last_pat_us_ > 5 * kSec) {
            last_behaviour_name_ = "notices something";
            SetMood(Mood::kCurious, 20);
            if (hooks_.look) hooks_.look(0, 0.2f);
            FaceFor("curious", 1500);
            if (CanTalk() && now >= next_curious_talk_us_ && Rand(2) == 0) {
                next_curious_talk_us_ = now + 15 * kMin;
                Talk(kCuriousPrompts, 2);
            } else {
                PlaySound("huh");
            }
        }
    } else if (object_near_ && d > 450) {
        object_near_ = false;
        if (Idle() && mood_.load() == Mood::kCurious) {
            // it went away
            if (hooks_.look) hooks_.look(Rand(2) ? -0.7f : 0.7f, 0);
            Pause(600);
            if (hooks_.look) hooks_.look(0, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Behaviours
// ---------------------------------------------------------------------------
void PetMode::DoBehaviour(int64_t now) {
    Mood m = mood_.load();
    if (m == Mood::kAsleep) {
        next_behaviour_us_ = now + 30 * kSec;
        return;  // asleep: the eyes show "z z" by themselves
    }
    // Weights per mood for: glance face wiggle spin nod peek chirp sigh yawn stretch talk clock
    static const uint8_t kWeights[][kBehaviourCount] = {
        /* calm     */ {30, 18, 4, 4, 12, 6, 6, 0, 0, 0, 0, 14},
        /* happy    */ {18, 22, 12, 6, 10, 4, 14, 0, 0, 0, 0, 6},
        /* curious  */ {26, 12, 0, 16, 4, 22, 6, 0, 0, 0, 6, 0},
        /* playful  */ {10, 20, 22, 14, 4, 4, 12, 0, 0, 0, 10, 4},
        /* bored    */ {18, 10, 4, 6, 0, 4, 2, 22, 6, 0, 16, 12},
        /* sleepy   */ {8, 6, 0, 0, 0, 0, 0, 10, 36, 14, 4, 8},
        /* asleep   */ {0},
        /* excited  */ {10, 24, 26, 14, 6, 0, 20, 0, 0, 0, 0, 0},
        /* surprised*/ {50, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        /* cautious */ {50, 30, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0},
    };
    const uint8_t* w = kWeights[(int)m];
    int weights[kBehaviourCount];
    int total = 0;
    for (int i = 0; i < kBehaviourCount; i++) {
        int x = w[i];
        for (int r : last_behaviours_) {
            if (r == i) x = 0;  // no repeats back to back
        }
        if ((i == kWiggle || i == kSpinLook || i == kStretch) && !CanMove(false)) x = 0;
        if (i == kPeek && !CanMove(true)) x = x / 3;  // peek with eyes only
        if (i == kTalk && (!CanTalk() || now < next_chat_us_)) x = 0;
        if ((i == kChirp || i == kYawn) && QuietHours()) x /= 4;
        if (i == kClock && (CLOCK_EVERY_SECONDS == 0 || now < next_clock_us_)) x = 0;
        weights[i] = x;
        total += x;
    }
    // Pace per mood (seconds until the next behaviour)
    static const int kPace[][2] = {{8, 20}, {6, 15}, {5, 12}, {5, 12}, {10, 25},
                                   {15, 30}, {30, 30}, {3, 7}, {3, 6}, {6, 12}};
    next_behaviour_us_ = now + RandSec(kPace[(int)m][0], kPace[(int)m][1]);
    if (total == 0) {
        return;
    }
    int pick = (int)Rand((uint32_t)total);
    int b = 0;
    for (; b < kBehaviourCount; b++) {
        if (pick < weights[b]) break;
        pick -= weights[b];
    }
    for (int i = 2; i > 0; i--) last_behaviours_[i] = last_behaviours_[i - 1];
    last_behaviours_[0] = b;
    last_behaviour_name_ = kBehaviourNames[b];
    using D = MotorController::Dir;

    switch (b) {
        case kGlance: {
            if (!hooks_.look) break;
            static const float kSpots[][2] = {{-1, 0}, {1, 0}, {-0.7f, -0.7f}, {0.7f, -0.6f},
                                              {0, 0.8f}, {-0.4f, 0.5f}, {0.5f, 0.4f}};
            int n = 2 + (int)Rand(3);
            int hold = m == Mood::kBored ? 1400 : (m == Mood::kCurious ? 900 : 650);
            for (int i = 0; i < n; i++) {
                const auto& p = kSpots[Rand(7)];
                hooks_.look(p[0], p[1]);
                Pause(hold + (int)Rand(500));
            }
            hooks_.look(0, 0);
            break;
        }
        case kFace: {
            static const char* kHappyFaces[] = {"happy", "loving", "winking", "laughing", "cool", "silly"};
            static const char* kCuriousFaces[] = {"curious", "thinking", "surprised", "confused"};
            static const char* kPlayFaces[] = {"silly", "winking", "playful", "laughing", "cool"};
            static const char* kBoredFaces[] = {"bored", "suspicious", "sleepy", "confused", "sad"};
            static const char* kCalmFaces[] = {"happy", "thinking", "curious", "winking", "relaxed"};
            const char* face;
            switch (m) {
                case Mood::kHappy:
                case Mood::kExcited: face = kHappyFaces[Rand(6)]; break;
                case Mood::kCurious:
                case Mood::kCautious:
                case Mood::kSurprised: face = kCuriousFaces[Rand(4)]; break;
                case Mood::kPlayful: face = kPlayFaces[Rand(5)]; break;
                case Mood::kBored:
                case Mood::kSleepy: face = kBoredFaces[Rand(5)]; break;
                default: face = kCalmFaces[Rand(5)]; break;
            }
            FaceFor(face, 1800 + (int)Rand(1500));
            break;
        }
        case kWiggle:
            ShowFace(m == Mood::kPlayful ? "playful" : "excited");
            Wiggle(m == Mood::kExcited ? 3 : 2, m == Mood::kExcited ? 65 : 55);
            Pause(300);
            ShowBaseFace();
            break;
        case kSpinLook: {
            // turn a little to look somewhere, then back
            next_move_us_ = now + 45 * kSec;
            bool left = Rand(2) == 0;
            if (hooks_.look) hooks_.look(left ? -1.0f : 1.0f, 0);
            motors_->Drive(left ? D::kBackward : D::kForward, left ? D::kForward : D::kBackward, 55, 300);
            Pause(1600);
            motors_->Drive(left ? D::kForward : D::kBackward, left ? D::kBackward : D::kForward, 55, 300);
            Pause(400);
            if (hooks_.look) hooks_.look(0, 0);
            break;
        }
        case kNod:
            if (hooks_.look) {
                for (int i = 0; i < 2; i++) {
                    hooks_.look(0, 0.7f);
                    Pause(250);
                    hooks_.look(0, -0.3f);
                    Pause(250);
                }
                hooks_.look(0, 0);
            }
            break;
        case kPeek:
            ShowFace("curious");
            if (hooks_.look) hooks_.look(0, 0.3f);
            if (CanMove(true)) {
                next_move_us_ = now + 45 * kSec;
                motors_->Drive(D::kForward, D::kForward, 45, 220);  // a tiny step forward
                Pause(900);
                motors_->Drive(D::kBackward, D::kBackward, 45, 220);
            }
            Pause(1200);
            if (hooks_.look) hooks_.look(0, 0);
            ShowBaseFace();
            break;
        case kChirp:
            PlaySound(Rand(2) ? "chirp" : "popup");
            FaceFor(m == Mood::kCurious ? "surprised" : "happy", 1200);
            break;
        case kSigh:
            ShowFace("bored");
            if (hooks_.look) hooks_.look(Rand(2) ? -0.5f : 0.5f, 0.8f);
            Pause(2500);
            if (hooks_.look) hooks_.look(0, 0);
            ShowBaseFace();
            break;
        case kYawn:
            ShowFace("yawn");
            PlaySound("yawn");
            Pause(1800);
            ShowBaseFace();
            break;
        case kStretch:
            ShowFace("yawn");
            Wiggle(1, 45);
            Pause(600);
            ShowBaseFace();
            break;
        case kTalk:
            next_chat_us_ = now + RandSec(PET_CHAT_MIN_MINUTES * 60, PET_CHAT_MAX_MINUTES * 60);
            if (m == Mood::kPlayful) {
                Talk(kPlayfulPrompts, 4);
            } else {
                Talk(kBoredPrompts, 6);
            }
            break;
        case kClock:
            next_clock_us_ = now + (int64_t)CLOCK_EVERY_SECONDS * kSec;
            if (hooks_.show_clock) hooks_.show_clock();
            break;
    }
}

// ---------------------------------------------------------------------------
// AI tool
// ---------------------------------------------------------------------------
void PetMode::RegisterMcpTools() {
    McpServer::GetInstance().AddTool(
        "self.chitu.pet_mode",
        "Turn Chitu's own personality on or off. When on, Chitu acts alive by itself: looks "
        "around, makes faces, chirps, wiggles, gets bored, naps, startles at loud sounds, "
        "dances to music and sometimes talks on its own. chatty: whether it talks on its own "
        "(never at night). moves: whether it may wiggle and turn on the spot by itself. "
        "Use when the user asks Chitu to be quiet, calm down, stop moving on its own, or be "
        "more lively.",
        PropertyList({
            Property("enabled", kPropertyTypeBoolean, true),
            Property("chatty", kPropertyTypeBoolean, true),
            Property("moves", kPropertyTypeBoolean, true),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            SetEnabled(properties["enabled"].value<bool>());
            SetChatty(properties["chatty"].value<bool>());
            SetMoves(properties["moves"].value<bool>());
            return StatusJson();
        });
}
