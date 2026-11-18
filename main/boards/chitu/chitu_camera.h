#pragma once

#include "camera.h"

#include <esp_camera.h>
#include <esp_timer.h>

#include <atomic>
#include <mutex>
#include <string>

// Camera for Chitu, OFF unless something needs it.
//
//  - Nothing runs at power-up: the sensor gets no clock and no memory is used.
//  - A photo question (AI "take photo" tool) switches it on, takes one fresh JPEG and
//    lets it switch off again CAMERA_IDLE_OFF_SECONDS later.
//  - The phone console's live view holds it on (StartStream / StopStream).
//  - The owner can block the camera completely (console switch "Camera allowed").
//  - Errors (no frames) restart the camera once before giving up.
//
// The sensor produces JPEG itself, so AI photos are uploaded as-is.
class ChituCamera : public Camera {
public:
    explicit ChituCamera(const camera_config_t& config);
    ~ChituCamera();

    // Camera interface (used by the AI "take photo" tool)
    void SetExplainUrl(const std::string& url, const std::string& token) override;
    bool Capture() override;
    bool SetHMirror(bool enabled) override;
    bool SetVFlip(bool enabled) override;
    std::expected<std::string, std::string> Explain(const std::string& question) override;
    std::expected<std::string, std::string> Explain(const std::string& question,
                                                    const std::string& url,
                                                    const std::string& token) override;

    // Live view for the phone console
    bool StartStream();
    void StopStream();
    bool streaming() const { return stream_users_.load() > 0; }
    // Next live-view frame (JPEG copied into buf); false if none. Holds the camera on.
    camera_fb_t* GetStreamFrame();
    void ReturnStreamFrame(camera_fb_t* fb);

    // Owner switch: false = camera may not be used at all
    void SetAllowed(bool allowed);
    bool allowed() const { return allowed_.load(); }
    bool powered() const { return powered_.load(); }
    const char* state_name() const;
    const std::string& last_error() const { return last_error_; }

private:
    camera_config_t config_;
    std::recursive_mutex mutex_;
    std::atomic<bool> powered_{false};
    std::atomic<bool> allowed_{true};
    std::atomic<int> stream_users_{0};
    std::atomic<int> fail_count_{0};
    std::atomic<bool> stopping_{false};
    int64_t last_use_us_ = 0;
    esp_timer_handle_t off_timer_ = nullptr;
    std::string last_error_;

    std::string explain_url_;
    std::string explain_token_;
    uint8_t* photo_ = nullptr;  // last captured JPEG (PSRAM)
    size_t photo_len_ = 0;
    size_t photo_cap_ = 0;
    int width_ = 0;
    int height_ = 0;

    bool PowerOn();
    void PowerOff();
    void Touch();
    static void OffTimerCb(void* arg);
};
