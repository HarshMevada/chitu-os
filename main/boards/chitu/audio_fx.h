#pragma once
// Small audio helpers that run inside Chitu's audio codec:
//   VoiceFx    - voice profile on everything Chitu plays (pitch up/down, robot ring)
//   SoundSense - listens to the room while Chitu is quiet: sudden loud sounds and
//                sustained rhythmic sound (music, with a rough tempo estimate)
// Both are plain math on the sample stream (no allocation, no locks).
#include <esp_timer.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace audio_fx {

// When Chitu last played something (so its own voice is not mistaken for the room)
inline std::atomic<int64_t> last_play_us{0};

// ---------------------------------------------------------------------------
// Voice profile
// ---------------------------------------------------------------------------
enum Profile { kNatural = 0, kCute = 1, kTiny = 2, kDeep = 3, kRobot = 4, kProfileCount };

inline const char* ProfileName(int p) {
    static const char* kNames[] = {"natural", "cute", "tiny", "deep", "robot"};
    return (p >= 0 && p < kProfileCount) ? kNames[p] : "natural";
}

// Pitch shifter with two crossfaded read taps on a delay line: changes the pitch
// without changing the speed, so it works on a live stream.
class PitchShifter {
public:
    void SetSemitones(float st) { ratio_ = powf(2.0f, st / 12.0f); }
    float Process(float x) {
        buf_[w_] = x;
        // the read taps drift relative to the write point at (1 - ratio) samples per sample
        phase_ += 1.0f - ratio_;
        if (phase_ >= kWin) phase_ -= kWin;
        if (phase_ < 0) phase_ += kWin;
        float d2 = phase_ + kWin * 0.5f;
        if (d2 >= kWin) d2 -= kWin;
        float y = Tap(phase_) * Window(phase_) + Tap(d2) * Window(d2);
        w_ = (w_ + 1) & (kSize - 1);
        return y;
    }

private:
    static constexpr int kSize = 2048;   // power of two
    static constexpr float kWin = 960;   // 40 ms at 24 kHz
    float buf_[kSize] = {};
    int w_ = 0;
    float phase_ = 0;
    float ratio_ = 1.0f;
    float Tap(float delay) const {
        float pos = (float)w_ - delay - 1.0f;
        while (pos < 0) pos += kSize;
        int i = (int)pos;
        float f = pos - i;
        float a = buf_[i & (kSize - 1)], b = buf_[(i + 1) & (kSize - 1)];
        return a + (b - a) * f;
    }
    static float Window(float d) {
        float s = sinf((float)M_PI * d / kWin);
        return s * s;  // two taps half a window apart always sum to 1
    }
};

class VoiceFx {
public:
    void SetProfile(int p, int sample_rate) {
        profile_ = (p >= 0 && p < kProfileCount) ? p : kNatural;
        rate_ = sample_rate > 0 ? sample_rate : 24000;
        switch (profile_) {
            case kCute: shifter_.SetSemitones(4); break;
            case kTiny: shifter_.SetSemitones(7); break;
            case kDeep: shifter_.SetSemitones(-4); break;
            case kRobot: shifter_.SetSemitones(1); break;
            default: shifter_.SetSemitones(0); break;
        }
    }
    int profile() const { return profile_; }
    // In-place on int16 samples
    void Process(int16_t* s, int n) {
        if (profile_ == kNatural) return;
        const float step = 2.0f * (float)M_PI * 55.0f / rate_;
        for (int i = 0; i < n; i++) {
            float x = s[i];
            float y = shifter_.Process(x);
            if (profile_ == kRobot) {
                ring_ += step;
                if (ring_ > 2.0f * (float)M_PI) ring_ -= 2.0f * (float)M_PI;
                y = y * (0.45f + 0.55f * sinf(ring_));
                y *= 1.25f;
            }
            if (y > 32767.0f) y = 32767.0f;
            if (y < -32767.0f) y = -32767.0f;
            s[i] = (int16_t)y;
        }
    }

private:
    int profile_ = kNatural;
    int rate_ = 24000;
    float ring_ = 0;
    PitchShifter shifter_;
};

// ---------------------------------------------------------------------------
// Room sound sensing (fed from the microphone at 16 kHz)
// ---------------------------------------------------------------------------
class SoundSense {
public:
    // Results, read from other tasks
    std::atomic<uint32_t> loud_count{0};       // increments on each sudden loud sound
    std::atomic<bool> music{false};            // sustained rhythmic sound right now
    std::atomic<int> beat_ms{0};               // estimated beat period while music
    std::atomic<int> level{0};                 // smoothed room level (rms)
    std::atomic<bool> enabled{true};

    void Feed(const int16_t* s, int n) {
        for (int i = 0; i < n; i++) {
            acc_ += (float)s[i] * s[i];
            if (++cnt_ >= kFrame) {
                Frame(sqrtf(acc_ / cnt_));
                acc_ = 0;
                cnt_ = 0;
            }
        }
    }

private:
    static constexpr int kFrame = 320;        // 20 ms at 16 kHz
    static constexpr int kHist = 300;         // 6 s of frames
    float acc_ = 0;
    int cnt_ = 0;
    float bg_ = 300.0f;                       // background level
    float prev_ = 0;
    float onset_[kHist] = {};
    float energy_[kHist] = {};
    int pos_ = 0;
    int frames_ = 0;
    int64_t last_loud_us_ = 0;
    int rhythmic_seconds_ = 0;
    int quiet_seconds_ = 0;

    void Frame(float rms) {
        int64_t now = esp_timer_get_time();
        bool own_sound = now - last_play_us.load() < 700 * 1000;
        level = (int)(level * 0.9f + rms * 0.1f);
        if (!enabled || own_sound) {
            prev_ = rms;
            onset_[pos_] = 0;
            energy_[pos_] = 0;
            Advance();
            return;
        }
        // Sudden loud sound: far above the background and a sharp jump from the last frame
        if (rms > 6000.0f && rms > bg_ * 7.0f && rms > prev_ * 3.0f &&
            now - last_loud_us_ > 6LL * 1000 * 1000) {
            last_loud_us_ = now;
            loud_count++;
        }
        // Background follows quiet levels slowly, and loud levels even more slowly
        bg_ += (rms - bg_) * (rms < bg_ ? 0.05f : 0.002f);
        if (bg_ < 80.0f) bg_ = 80.0f;
        onset_[pos_] = rms > prev_ ? rms - prev_ : 0;
        energy_[pos_] = rms;
        prev_ = rms;
        Advance();
    }

    void Advance() {
        pos_ = (pos_ + 1) % kHist;
        if (++frames_ % 50 == 0) {
            Analyse();  // once a second
        }
    }

    // Music = loud enough most of the time AND a repeating onset pattern (a beat)
    void Analyse() {
        int loud = 0;
        float mean = 0;
        for (int i = 0; i < kHist; i++) {
            if (energy_[i] > bg_ * 2.0f + 300.0f) loud++;
            mean += onset_[i];
        }
        mean /= kHist;
        bool sustained = loud > kHist * 7 / 10;
        float best = 0;
        int best_lag = 0;
        if (sustained) {
            float var = 0;
            for (int i = 0; i < kHist; i++) var += (onset_[i] - mean) * (onset_[i] - mean);
            if (var > 1.0f) {
                // beat between 60 and 180 per minute: lag 17..50 frames
                for (int lag = 17; lag <= 50; lag++) {
                    float ac = 0;
                    for (int i = 0; i < kHist - lag; i++) {
                        ac += (onset_[(pos_ + i) % kHist] - mean) * (onset_[(pos_ + i + lag) % kHist] - mean);
                    }
                    ac /= var;
                    if (ac > best) {
                        best = ac;
                        best_lag = lag;
                    }
                }
            }
        }
        if (sustained && best > 0.25f) {
            quiet_seconds_ = 0;
            if (++rhythmic_seconds_ >= 4) {
                music = true;
                beat_ms = best_lag * 20;
            }
        } else {
            rhythmic_seconds_ = 0;
            if (++quiet_seconds_ >= 5) {
                music = false;
                beat_ms = 0;
            }
        }
    }
};

inline SoundSense sense;

}  // namespace audio_fx
