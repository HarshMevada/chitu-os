#include "chitu_camera.h"

#include "board.h"
#include "config.h"
#include "settings.h"
#include "system_info.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#define TAG "ChituCamera"

ChituCamera::ChituCamera(const camera_config_t& config) : config_(config) {
    Settings settings("chitu");
    allowed_ = settings.GetInt("cam_allowed", 1) != 0;
    esp_timer_create_args_t args = {};
    args.callback = OffTimerCb;
    args.arg = this;
    args.name = "cam_off";
    esp_timer_create(&args, &off_timer_);
    esp_timer_start_periodic(off_timer_, 2ULL * 1000 * 1000);
    ESP_LOGI(TAG, "Camera stays off until needed (%s)", allowed_ ? "allowed" : "blocked by owner");
}

ChituCamera::~ChituCamera() {
    if (off_timer_ != nullptr) {
        esp_timer_stop(off_timer_);
        esp_timer_delete(off_timer_);
    }
    PowerOff();
    if (photo_ != nullptr) {
        heap_caps_free(photo_);
    }
}

const char* ChituCamera::state_name() const {
    if (!allowed_) return "blocked";
    if (stream_users_ > 0) return "live";
    return powered_ ? "on" : "off";
}

bool ChituCamera::PowerOn() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (powered_) {
        return true;
    }
    if (!allowed_) {
        last_error_ = "Camera is switched off by the owner";
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_camera_init(&config_);
    if (err != ESP_OK) {
        last_error_ = "Camera did not start (check the camera ribbon cable)";
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
        return false;
    }
    sensor_t* s = esp_camera_sensor_get();
    if (s != nullptr) {
        Settings settings("chitu");
        s->set_hmirror(s, settings.GetInt("cam_hmirror", CAMERA_HMIRROR) ? 1 : 0);
        s->set_vflip(s, settings.GetInt("cam_vflip", CAMERA_VFLIP) ? 1 : 0);
        s->set_framesize(s, CAMERA_STREAM_FRAMESIZE);
    }
    powered_ = true;
    fail_count_ = 0;
    last_use_us_ = esp_timer_get_time();
    last_error_.clear();
    ESP_LOGI(TAG, "Camera on (%d ms, sensor PID 0x%x, free PSRAM %u KB)",
             (int)((esp_timer_get_time() - t0) / 1000), s ? s->id.PID : 0,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return true;
}

void ChituCamera::PowerOff() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!powered_) {
        return;
    }
    esp_camera_deinit();
    powered_ = false;
    ESP_LOGI(TAG, "Camera off");
}

void ChituCamera::Touch() { last_use_us_ = esp_timer_get_time(); }

void ChituCamera::OffTimerCb(void* arg) {
    auto* self = static_cast<ChituCamera*>(arg);
    if (!self->powered_ || self->stream_users_ > 0) {
        return;
    }
    if (esp_timer_get_time() - self->last_use_us_ > (int64_t)CAMERA_IDLE_OFF_SECONDS * 1000 * 1000 &&
        !self->stopping_.exchange(true)) {
        // switching off takes a while: not on the timer task
        if (xTaskCreate(
                [](void* arg) {
                    auto* cam = static_cast<ChituCamera*>(arg);
                    if (cam->stream_users_ == 0) {
                        cam->PowerOff();
                    }
                    cam->stopping_ = false;
                    vTaskDelete(nullptr);
                },
                "cam_off", 3072, self, 2, nullptr) != pdPASS) {
            self->stopping_ = false;
        }
    }
}

void ChituCamera::SetAllowed(bool allowed) {
    allowed_ = allowed;
    Settings("chitu", true).SetInt("cam_allowed", allowed ? 1 : 0);
    if (!allowed) {
        stream_users_ = 0;
        PowerOff();
    }
}

void ChituCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool ChituCamera::StartStream() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!PowerOn()) {
        return false;
    }
    stream_users_ = 1;
    Touch();
    return true;
}

void ChituCamera::StopStream() {
    stream_users_ = 0;
    Touch();  // the off timer switches it off a little later
}

camera_fb_t* ChituCamera::GetStreamFrame() {
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !powered_) {
        return nullptr;  // a photo is being taken right now
    }
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
        if (++fail_count_ >= 5) {
            ESP_LOGW(TAG, "No frames from the camera - restarting it");
            PowerOff();
            vTaskDelay(pdMS_TO_TICKS(200));
            PowerOn();
        }
        return nullptr;
    }
    fail_count_ = 0;
    Touch();
    return fb;
}

void ChituCamera::ReturnStreamFrame(camera_fb_t* fb) {
    if (fb != nullptr && powered_) {
        esp_camera_fb_return(fb);
    }
}

bool ChituCamera::Capture() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    bool was_on = powered_;
    if (!PowerOn()) {
        ESP_LOGE(TAG, "Photo: %s", last_error_.c_str());
        return false;
    }
    sensor_t* s = esp_camera_sensor_get();
    if (s != nullptr) {
        s->set_framesize(s, CAMERA_PHOTO_FRAMESIZE);
    }
    // Let the exposure settle on a fresh start, and skip frames from before the
    // size change: the photo must show "now"
    if (!was_on) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    camera_fb_t* fb = nullptr;
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        for (int i = 0; i < 3; i++) {
            if (fb != nullptr) {
                esp_camera_fb_return(fb);
            }
            fb = esp_camera_fb_get();
            if (fb == nullptr) {
                break;
            }
        }
        ok = fb != nullptr && fb->format == PIXFORMAT_JPEG && fb->len > 0;
        if (!ok) {
            if (fb != nullptr) {
                esp_camera_fb_return(fb);
                fb = nullptr;
            }
            ESP_LOGW(TAG, "Photo failed, restarting the camera");
            PowerOff();
            vTaskDelay(pdMS_TO_TICKS(200));
            if (!PowerOn()) {
                return false;
            }
            if ((s = esp_camera_sensor_get()) != nullptr) {
                s->set_framesize(s, CAMERA_PHOTO_FRAMESIZE);
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    if (!ok) {
        last_error_ = "Camera gave no picture";
        return false;
    }
    if (photo_cap_ < fb->len) {
        if (photo_ != nullptr) {
            heap_caps_free(photo_);
        }
        photo_cap_ = fb->len + 16 * 1024;
        photo_ = static_cast<uint8_t*>(heap_caps_malloc(photo_cap_, MALLOC_CAP_SPIRAM));
        if (photo_ == nullptr) {
            photo_cap_ = 0;
            photo_len_ = 0;
            esp_camera_fb_return(fb);
            ESP_LOGE(TAG, "No memory for photo");
            return false;
        }
    }
    memcpy(photo_, fb->buf, fb->len);
    photo_len_ = fb->len;
    width_ = fb->width;
    height_ = fb->height;
    esp_camera_fb_return(fb);
    if ((s = esp_camera_sensor_get()) != nullptr) {
        s->set_framesize(s, CAMERA_STREAM_FRAMESIZE);  // back to the live-view size
    }
    Touch();
    ESP_LOGI(TAG, "Photo %dx%d JPEG, %u bytes", width_, height_, (unsigned)photo_len_);
    return true;
}

bool ChituCamera::SetHMirror(bool enabled) {
    Settings("chitu", true).SetInt("cam_hmirror", enabled ? 1 : 0);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sensor_t* s = powered_ ? esp_camera_sensor_get() : nullptr;
    if (s != nullptr) {
        s->set_hmirror(s, enabled ? 1 : 0);
    }
    return true;
}

bool ChituCamera::SetVFlip(bool enabled) {
    Settings("chitu", true).SetInt("cam_vflip", enabled ? 1 : 0);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sensor_t* s = powered_ ? esp_camera_sensor_get() : nullptr;
    if (s != nullptr) {
        s->set_vflip(s, enabled ? 1 : 0);
    }
    return true;
}

std::expected<std::string, std::string> ChituCamera::Explain(const std::string& question) {
    return Explain(question, explain_url_, explain_token_);
}

std::expected<std::string, std::string> ChituCamera::Explain(const std::string& question,
                                                             const std::string& url,
                                                             const std::string& token) {
    if (url.empty()) {
        return std::unexpected("Image explain URL or token is not set");
    }
    if (photo_ == nullptr || photo_len_ == 0) {
        return std::unexpected("No camera frame captured");
    }

    // Same upload format as xiaozhi's Esp32Camera: multipart with an explicit
    // Content-Length (the server rejects chunked uploads), JPEG sent as-is.
    const std::string boundary = "----ESP32_CAMERA_BOUNDARY";
    std::string head;
    head += "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"question\"\r\n\r\n";
    head += question + "\r\n";
    head += "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
    head += "Content-Type: image/jpeg\r\n\r\n";
    const std::string tail = "\r\n--" + boundary + "--\r\n";

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!token.empty()) {
        http->SetHeader("Authorization", "Bearer " + token);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Content-Length", std::to_string(head.size() + photo_len_ + tail.size()));
    if (auto opened = http->Open("POST", url); !opened) {
        ESP_LOGE(TAG, "Failed to upload photo: %s", opened.error().ToString().c_str());
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    auto write_all = [&http](const void* data, size_t size) -> bool {
        const char* ptr = static_cast<const char*>(data);
        while (size > 0) {
            auto written = http->Write(ptr, size);
            if (!written || *written <= 0 || static_cast<size_t>(*written) > size) {
                return false;
            }
            ptr += *written;
            size -= static_cast<size_t>(*written);
        }
        return true;
    };

    bool uploaded = write_all(head.data(), head.size()) && write_all(photo_, photo_len_) &&
                    write_all(tail.data(), tail.size());
    if (!uploaded) {
        ESP_LOGE(TAG, "Failed to upload photo body");
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        http->Close();
        return std::unexpected("Failed to upload photo");
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", *status_code);
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Explain %dx%d jpeg=%u bytes, question=%s\n%s", width_, height_,
             (unsigned)photo_len_, question.c_str(), result.c_str());
    return result;
}
