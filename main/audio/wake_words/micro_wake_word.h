#ifndef MICRO_WAKE_WORD_H
#define MICRO_WAKE_WORD_H

// Chitu OS: wake word detector for models trained with microWakeWord
// (https://github.com/kahrendt/microWakeWord). The model (.tflite) is built into the
// firmware; see tools/wakeword/ for how "Chitu" is trained.
//
// Audio path: 16 kHz mono from the AFE -> 40 spectrogram features every 10 ms ->
// streaming TFLite model -> probability, averaged over a short window -> detection.

#include <sdkconfig.h>

#if CONFIG_USE_MICRO_WAKE_WORD

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <frontend.h>
#include <frontend_util.h>

class MicroWakeWord {
public:
    MicroWakeWord();
    ~MicroWakeWord();

    bool Initialize();
    void FeedMono(const int16_t* data, size_t samples);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
        callback_ = std::move(callback);
    }
    void Start();
    void Stop();
    std::vector<std::string> GetWakeWords() const { return {wake_word_}; }
    // Last probabilities (0-255), for diagnostics
    int last_probability() const { return last_probability_.load(); }

private:
    static constexpr int kFeatures = 40;
    static constexpr int kSlidingWindow = 5;
    static constexpr int kIgnoreSlicesAfterStart = 100;  // ~1 s warm-up / cool-down

    std::string wake_word_;
    uint8_t cutoff_ = 240;
    std::atomic<bool> running_{false};
    std::atomic<bool> reset_pending_{false};
    std::atomic<int> last_probability_{0};

    FrontendConfig frontend_config_ = {};
    FrontendState frontend_state_ = {};
    bool frontend_ready_ = false;

    uint8_t* tensor_arena_ = nullptr;
    size_t tensor_arena_size_ = 0;
    uint8_t* var_arena_ = nullptr;
    // TFLite Micro objects (kept opaque so this header needs no TFLite includes)
    void* interpreter_ = nullptr;
    void* op_resolver_ = nullptr;

    int stride_ = 3;
    int stride_step_ = 0;
    uint8_t probabilities_[kSlidingWindow] = {};
    int prob_index_ = 0;
    int ignore_slices_ = -kIgnoreSlicesAfterStart;

    std::function<void(const std::string&)> callback_;

    bool LoadModel();
    void ResetDetection();
    void ProcessFeatures(const int8_t features[kFeatures]);
};

#endif  // CONFIG_USE_MICRO_WAKE_WORD
#endif  // MICRO_WAKE_WORD_H
