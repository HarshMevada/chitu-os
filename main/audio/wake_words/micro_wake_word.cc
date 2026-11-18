#include "micro_wake_word.h"

#if CONFIG_USE_MICRO_WAKE_WORD

#include <algorithm>
#include <cstring>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>

#define TAG "MicroWakeWord"

// The trained model, built into the firmware by main/CMakeLists.txt
extern const uint8_t micro_wake_word_model_start[] asm("_binary_micro_wake_word_tflite_start");

namespace {
using OpResolver = tflite::MicroMutableOpResolver<20>;
using Interpreter = tflite::MicroInterpreter;
Interpreter* Interp(void* p) { return static_cast<Interpreter*>(p); }
constexpr size_t kVarArenaSize = 1024;
constexpr size_t kMaxTensorArena = 96 * 1024;

uint8_t* AllocArena(size_t size) {
    // Internal RAM is much faster for inference; PSRAM is the fallback
    auto* p = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (p == nullptr) {
        p = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM));
    }
    return p;
}

bool RegisterOps(OpResolver& r) {
    return r.AddCallOnce() == kTfLiteOk && r.AddVarHandle() == kTfLiteOk && r.AddReshape() == kTfLiteOk &&
           r.AddReadVariable() == kTfLiteOk && r.AddStridedSlice() == kTfLiteOk &&
           r.AddConcatenation() == kTfLiteOk && r.AddAssignVariable() == kTfLiteOk &&
           r.AddConv2D() == kTfLiteOk && r.AddMul() == kTfLiteOk && r.AddAdd() == kTfLiteOk &&
           r.AddMean() == kTfLiteOk && r.AddFullyConnected() == kTfLiteOk && r.AddLogistic() == kTfLiteOk &&
           r.AddQuantize() == kTfLiteOk && r.AddDepthwiseConv2D() == kTfLiteOk &&
           r.AddAveragePool2D() == kTfLiteOk && r.AddMaxPool2D() == kTfLiteOk && r.AddPad() == kTfLiteOk &&
           r.AddPack() == kTfLiteOk && r.AddSplitV() == kTfLiteOk;
}
}  // namespace

MicroWakeWord::MicroWakeWord() {
    wake_word_ = CONFIG_MICRO_WAKE_WORD_DISPLAY;
    cutoff_ = (uint8_t)std::clamp(CONFIG_MICRO_WAKE_WORD_CUTOFF * 255 / 100, 1, 254);
}

MicroWakeWord::~MicroWakeWord() {
    delete Interp(interpreter_);
    delete static_cast<OpResolver*>(op_resolver_);
    if (tensor_arena_ != nullptr) heap_caps_free(tensor_arena_);
    if (var_arena_ != nullptr) heap_caps_free(var_arena_);
    if (frontend_ready_) FrontendFreeStateContents(&frontend_state_);
}

bool MicroWakeWord::Initialize() {
    // Spectrogram settings: these must match how microWakeWord models are trained
    frontend_config_.window.size_ms = 30;
    frontend_config_.window.step_size_ms = 10;
    frontend_config_.filterbank.num_channels = kFeatures;
    frontend_config_.filterbank.lower_band_limit = 125.0f;
    frontend_config_.filterbank.upper_band_limit = 7500.0f;
    frontend_config_.noise_reduction.smoothing_bits = 10;
    frontend_config_.noise_reduction.even_smoothing = 0.025f;
    frontend_config_.noise_reduction.odd_smoothing = 0.06f;
    frontend_config_.noise_reduction.min_signal_remaining = 0.05f;
    frontend_config_.pcan_gain_control.enable_pcan = 1;
    frontend_config_.pcan_gain_control.strength = 0.95f;
    frontend_config_.pcan_gain_control.offset = 80.0f;
    frontend_config_.pcan_gain_control.gain_bits = 21;
    frontend_config_.log_scale.enable_log = 1;
    frontend_config_.log_scale.scale_shift = 6;
    if (!FrontendPopulateState(&frontend_config_, &frontend_state_, 16000)) {
        ESP_LOGE(TAG, "Feature generator setup failed");
        return false;
    }
    frontend_ready_ = true;
    if (!LoadModel()) {
        return false;
    }
    ESP_LOGI(TAG, "Wake word \"%s\" ready (cutoff %d%%, arena %u bytes)", wake_word_.c_str(),
             CONFIG_MICRO_WAKE_WORD_CUTOFF, (unsigned)tensor_arena_size_);
    return true;
}

bool MicroWakeWord::LoadModel() {
    const tflite::Model* model = tflite::GetModel(micro_wake_word_model_start);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema not supported");
        return false;
    }
    auto* resolver = new (std::nothrow) OpResolver();
    if (resolver == nullptr || !RegisterOps(*resolver)) {
        ESP_LOGE(TAG, "Model operations could not be registered");
        delete resolver;
        return false;
    }
    op_resolver_ = resolver;

    var_arena_ = AllocArena(kVarArenaSize);
    if (var_arena_ == nullptr) {
        return false;
    }
    // Find a tensor arena size that works (it depends on the model)
    for (size_t size = 24 * 1024; size <= kMaxTensorArena; size += 8 * 1024) {
        tensor_arena_ = AllocArena(size);
        if (tensor_arena_ == nullptr) {
            break;
        }
        auto* var_allocator = tflite::MicroAllocator::Create(var_arena_, kVarArenaSize);
        auto* resource_vars = tflite::MicroResourceVariables::Create(var_allocator, 20);
        auto* interp = new (std::nothrow) Interpreter(model, *resolver, tensor_arena_, size, resource_vars);
        if (interp != nullptr && interp->AllocateTensors() == kTfLiteOk) {
            interpreter_ = interp;
            tensor_arena_size_ = size;
            break;
        }
        delete interp;
        heap_caps_free(tensor_arena_);
        tensor_arena_ = nullptr;
    }
    if (interpreter_ == nullptr) {
        ESP_LOGE(TAG, "Not enough memory for the wake word model");
        return false;
    }

    TfLiteTensor* input = Interp(interpreter_)->input(0);
    TfLiteTensor* output = Interp(interpreter_)->output(0);
    if (input->dims->size != 3 || input->dims->data[0] != 1 || input->dims->data[2] != kFeatures ||
        input->type != kTfLiteInt8 || output->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "Unexpected model input/output format");
        return false;
    }
    stride_ = input->dims->data[1];
    return true;
}

void MicroWakeWord::Start() {
    reset_pending_ = true;
    running_ = true;
}

void MicroWakeWord::Stop() { running_ = false; }

void MicroWakeWord::ResetDetection() {
    memset(probabilities_, 0, sizeof(probabilities_));
    ignore_slices_ = -kIgnoreSlicesAfterStart;
    stride_step_ = 0;
    FrontendReset(&frontend_state_);
}

void MicroWakeWord::FeedMono(const int16_t* data, size_t samples) {
    if (!running_ || interpreter_ == nullptr) {
        return;
    }
    if (reset_pending_.exchange(false)) {
        ResetDetection();
    }
    while (samples > 0 && running_) {
        size_t used = 0;
        FrontendOutput out = FrontendProcessSamples(&frontend_state_, data, samples, &used);
        if (used == 0) {
            break;
        }
        data += used;
        samples -= used;
        if (out.size != kFeatures) {
            continue;
        }
        int8_t features[kFeatures];
        for (int i = 0; i < kFeatures; i++) {
            // Same scaling as the training pipeline: (value / 25.6 / 26 * 256) - 128
            int32_t v = ((int32_t)out.values[i] * 256 + 333) / 666 - 128;
            features[i] = (int8_t)std::clamp<int32_t>(v, -128, 127);
        }
        ProcessFeatures(features);
    }
}

void MicroWakeWord::ProcessFeatures(const int8_t features[kFeatures]) {
    Interpreter* interp = Interp(interpreter_);
    TfLiteTensor* input = interp->input(0);
    memcpy(tflite::GetTensorData<int8_t>(input) + kFeatures * stride_step_, features, kFeatures);
    if (++stride_step_ < stride_) {
        return;
    }
    stride_step_ = 0;
    if (interp->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "Inference failed");
        return;
    }
    uint8_t p = interp->output(0)->data.uint8[0];
    last_probability_ = p;
    prob_index_ = (prob_index_ + 1) % kSlidingWindow;
    probabilities_[prob_index_] = p;
    if (p < cutoff_) {
        ignore_slices_ = std::min(ignore_slices_ + stride_, 0);
    }
    if (ignore_slices_ < 0) {
        return;
    }
    uint32_t sum = 0;
    for (uint8_t v : probabilities_) sum += v;
    if (sum > (uint32_t)cutoff_ * kSlidingWindow) {
        ESP_LOGI(TAG, "Wake word \"%s\" detected (average %u/255)", wake_word_.c_str(),
                 (unsigned)(sum / kSlidingWindow));
        memset(probabilities_, 0, sizeof(probabilities_));
        ignore_slices_ = -kIgnoreSlicesAfterStart;
        if (callback_) {
            callback_(wake_word_);
        }
    }
}

#endif  // CONFIG_USE_MICRO_WAKE_WORD
