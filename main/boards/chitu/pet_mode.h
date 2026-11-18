#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

class MotorController;
class Display;

// Chitu's personality: what it does on its own when nobody is talking to it.
//
// A small mood engine. The mood comes from how long Chitu has been left alone, the time
// of day and what happens around it; each mood has its own face, its own little
// behaviours (glances, faces, chirps, wiggles, spins, sighs, yawns) and its own pace.
//
//   attention just now  -> calm / happy / curious (blinks, glances, small faces)
//   a few minutes alone -> bored / playful (sighs, wiggles, talks to you now and then)
//   longer alone        -> sleepy (yawns, droopy eyes) -> asleep ("z z", eyes closed)
//   someone comes back  -> wakes up, excited
//
// It also reacts to the world:
//   sudden loud sound   -> startled (wide eyes, looks around, "What was that?")
//   music nearby        -> excited, dances to the beat (rough tempo from the mic)
//   something in front  -> curious, asks what it is (ToF distance sensor only: it never
//                          guesses WHAT it is; the camera is not switched on for this)
//   hand held very close-> counts as a pat on the head: happy, hearts
//
// Behaviours never repeat back to back, speech has long cooldowns, there are quiet
// hours, moves are tiny and only on the spot, and anything the owner does (voice,
// console, joystick) always wins.
class PetMode {
public:
    struct Hooks {
        std::function<void(float x, float y)> look;   // eyes look direction
        std::function<bool()> show_clock;             // big clock for a few seconds
        std::function<bool()> overlay_visible;        // message / QR / info page on screen
        std::function<bool()> owner_driving;          // joystick used in the last seconds
        std::function<int()> distance_mm;             // -1 when there is no sensor
    };

    void Start(MotorController* motors, Display* display, Hooks hooks);

    bool enabled() const { return enabled_.load(); }
    bool chatty() const { return chatty_.load(); }
    bool moves() const { return moves_.load(); }
    void SetEnabled(bool on);
    void SetChatty(bool on);
    void SetMoves(bool on);

    // Someone paid attention (voice, console, message): wake up, be happy
    void Poke();

    const char* mood_name() const;
    std::string StatusJson() const;
    void RegisterMcpTools();

    enum class Mood : int8_t {
        kCalm, kHappy, kCurious, kPlayful, kBored, kSleepy, kAsleep, kExcited, kSurprised, kCautious
    };

private:
    MotorController* motors_ = nullptr;
    Display* display_ = nullptr;
    Hooks hooks_;

    std::atomic<bool> enabled_{true};
    std::atomic<bool> chatty_{true};
    std::atomic<bool> moves_{true};
    std::atomic<int64_t> last_active_us_{0};
    std::atomic<bool> poked_{false};
    std::atomic<Mood> mood_{Mood::kCalm};
    int64_t mood_until_us_ = 0;      // temporary moods (surprised, excited...) end here
    int64_t next_behaviour_us_ = 0;
    int64_t next_chat_us_ = 0;
    int64_t next_move_us_ = 0;
    int64_t next_clock_us_ = 0;
    int64_t next_curious_talk_us_ = 0;
    int64_t next_startle_talk_us_ = 0;
    int64_t music_dance_until_us_ = 0;
    int64_t next_music_dance_us_ = 0;
    uint32_t last_loud_count_ = 0;
    int last_behaviours_[3] = {-1, -1, -1};
    int last_prompts_[6] = {-1, -1, -1, -1, -1, -1};
    const char* last_behaviour_name_ = "";

    // distance tracking
    int near_since_ms_ = 0;
    bool object_near_ = false;
    int64_t pat_start_us_ = 0;
    int64_t last_pat_us_ = 0;

    void Loop();
    void SetMood(Mood m, int seconds = 0);
    void ShowFace(const char* face);
    void ShowBaseFace();
    void FaceFor(const char* face, int ms);
    bool QuietHours() const;
    bool CanMove(bool forward) const;
    bool CanTalk() const;
    bool Idle() const;
    void Pause(int ms);

    void UpdateMoodFromIdle(int64_t idle_us);
    void HandleSound(int64_t now);
    void HandleDistance(int64_t now);
    void HandleMusic(int64_t now);
    void DoBehaviour(int64_t now);
    void Talk(const char* const* prompts, int count);
    void PlaySound(const char* which);
    void Wiggle(int times, int speed);
};
