// Chitu robot: Seeed XIAO ESP32-S3 Sense + MAX98357 + TB6612FNG + SH1106 OLED
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "config.h"
#include "audio_diag.h"
#include "remote_audio.h"
#include "device_state_machine.h"
#include "chitu_camera.h"
#include "led/gpio_led.h"
#include "mcp_server.h"
#include "motor_controller.h"
#include "obstacle_sensor.h"
#include "pet_mode.h"
#include "web_console.h"
#include "chitu_eyes_display.h"
#include "audio_fx.h"
#include "chat_bridge.h"
#include "net_manager.h"
#include "settings.h"
#include "time_location.h"

#include <atomic>

#include <ssid_manager.h>

#include <driver/i2c_master.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <cmath>
#include <cstring>
#include <vector>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_sh1106.h>
#include <esp_log.h>

#define TAG "ChituBoard"

// Audio codec tuned for this robot:
//  - microphone: multiplies the quiet PDM mic by MIC_GAIN
//  - speaker: lifts speech towards SPEAKER_TARGET_RMS (up to SPEAKER_BOOST times) with a
//    soft limiter on the peaks, and starts at DEFAULT_OUTPUT_VOLUME on a fresh install.
class ChituAudioCodec : public NoAudioCodecSimplexPdm {
public:
    ChituAudioCodec(int input_rate, int output_rate, gpio_num_t bclk, gpio_num_t ws,
                    gpio_num_t dout, gpio_num_t mic_clk, gpio_num_t mic_din)
        : NoAudioCodecSimplexPdm(input_rate, output_rate, bclk, ws, dout, mic_clk, mic_din) {
        output_volume_ = DEFAULT_OUTPUT_VOLUME;  // used unless a volume was saved before
        input_gain_ = 0;  // gain is applied in Read() after removing the mic's DC offset
    }

    int Read(int16_t* dest, int samples) override {
        int n = NoAudioCodecSimplexPdm::Read(dest, samples);
        for (int i = 0; i < n; i++) {
            // The Sense PDM mic has a large constant offset. A DC-blocking high-pass
            // filter removes it; only then is the quiet voice signal amplified.
            float x = dest[i];
            float y = x - hp_prev_x_ + 0.995f * hp_prev_y_;
            hp_prev_x_ = x;
            hp_prev_y_ = y;
            win_dc_sum_ += (int64_t)dest[i];
            float v = y * MIC_GAIN;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32767.0f) v = -32767.0f;
            dest[i] = (int16_t)v;
            int a = abs((int)dest[i]);
            if (a > win_peak_) win_peak_ = a;
            if (a >= 32760) win_clipped_++;
            win_sum_sq_ += (int64_t)dest[i] * dest[i];
        }
        win_samples_ += n;
        RouteRemoteMic(dest, n);
        if (!remote_audio::PhoneMicActive()) {
            audio_fx::sense.Feed(dest, n);  // loud sounds / music for the personality
        }
        if (win_samples_ >= input_sample_rate_) {  // publish about once per second
            audio_diag::mic_rms = (int)sqrt((double)win_sum_sq_ / win_samples_);
            audio_diag::mic_peak = win_peak_;
            audio_diag::mic_clipped = win_clipped_;
            audio_diag::mic_dc = (int)(win_dc_sum_ / win_samples_);
            win_dc_sum_ = 0;
            win_samples_ = 0;
            win_sum_sq_ = 0;
            win_peak_ = 0;
            win_clipped_ = 0;
        }
        return n;
    }

protected:
    // Phone console audio: the phone mic replaces the robot mic while it is streaming;
    // otherwise the room sound can be sent to the phone.
    void RouteRemoteMic(int16_t* dest, int n) {
        if (remote_audio::PhoneMicActive()) {
            size_t want = (size_t)n * sizeof(int16_t);
            // Push-to-talk is opening the line: keep the phone audio until Chitu listens
            if (WebConsole::HoldPhoneAudio() &&
                Application::GetInstance().GetDeviceState() != kDeviceStateListening) {
                memset(dest, 0, want);
                return;
            }
            // small jitter cushion: after running dry, wait for ~120 ms of audio
            if (phone_starved_ && remote_audio::phone_mic.Available() < 1920 * sizeof(int16_t)) {
                memset(dest, 0, want);
                return;
            }
            size_t got = remote_audio::phone_mic.Pop(dest, want);
            if (got < want) {
                memset(reinterpret_cast<uint8_t*>(dest) + got, 0, want - got);
                phone_starved_ = true;
            } else {
                phone_starved_ = false;
            }
        } else {
            phone_starved_ = true;
            if (remote_audio::room_listen) {
                remote_audio::room_out.Push(dest, (size_t)n * sizeof(int16_t));
            }
        }
    }

public:
    // Voice profile (0 natural .. 4 robot), saved
    void SetVoiceProfile(int p) {
        voice_profile_ = p;
        fx_pending_ = true;
        Settings settings("chitu", true);
        settings.SetInt("voice_fx", p);
    }
    int voice_profile() const { return voice_profile_.load(); }
    void LoadVoiceProfile() {
        Settings settings("chitu");
        voice_profile_ = settings.GetInt("voice_fx", VOICE_PROFILE);
        fx_pending_ = true;
    }

protected:
    int Write(const int16_t* in, int samples) override {
        // Voice effect first (it changes what the phone hears too)
        if (fx_pending_.exchange(false)) {
            fx_.SetProfile(voice_profile_, output_sample_rate_);
        }
        const int16_t* data = in;
        if (fx_.profile() != audio_fx::kNatural) {
            fx_buf_.assign(in, in + samples);
            fx_.Process(fx_buf_.data(), samples);
            data = fx_buf_.data();
        }
        if (remote_audio::phone_listen) {
            remote_audio::voice_out.Push(data, (size_t)samples * sizeof(int16_t));
            if (!remote_audio::robot_speaker) {
                // keep the I2S timing (it paces playback) but stay silent on the robot
                boosted_.assign(samples, 0);
                return NoAudioCodecSimplexPdm::Write(boosted_.data(), samples);
            }
        }
        // Loudness: lift speech towards SPEAKER_TARGET_RMS (up to SPEAKER_BOOST times),
        // then a soft limiter rounds off peaks instead of clipping them harshly.
        double sum_sq = 0;
        for (int i = 0; i < samples; i++) {
            sum_sq += (double)data[i] * data[i];
        }
        float rms = sqrtf((float)(sum_sq / (samples > 0 ? samples : 1)));
        if (rms > 200.0f) {
            audio_fx::last_play_us = esp_timer_get_time();  // so Chitu ignores its own voice
        }
        float wanted = rms < 200.0f ? gain_ : fminf(SPEAKER_BOOST, SPEAKER_TARGET_RMS / rms);
        if (wanted < 1.0f) wanted = 1.0f;
        float start = gain_;
        // fast attack (turn down quickly), slow release (turn up gently)
        float end = (wanted < gain_) ? gain_ + (wanted - gain_) * 0.5f : gain_ + (wanted - gain_) * 0.06f;

        constexpr float kKnee = 20000.0f;
        constexpr float kRoom = 32767.0f - kKnee;
        boosted_.resize(samples);
        for (int i = 0; i < samples; i++) {
            float g = start + (end - start) * i / samples;  // smooth ramp, no clicks
            float x = data[i] * g;
            float ax = fabsf(x);
            if (ax > kKnee) {
                float over = kKnee + kRoom * tanhf((ax - kKnee) / kRoom);
                x = x < 0 ? -over : over;
            }
            boosted_[i] = (int16_t)x;
        }
        gain_ = end;
        return NoAudioCodecSimplexPdm::Write(boosted_.data(), samples);
    }

private:
    std::vector<int16_t> boosted_;
    std::vector<int16_t> fx_buf_;
    audio_fx::VoiceFx fx_;
    std::atomic<int> voice_profile_{0};
    std::atomic<bool> fx_pending_{true};
    float gain_ = 1.0f;
    int64_t win_sum_sq_ = 0;
    int win_samples_ = 0;
    int win_peak_ = 0;
    int win_clipped_ = 0;
    int64_t win_dc_sum_ = 0;
    float hp_prev_x_ = 0.0f;
    float hp_prev_y_ = 0.0f;
    bool phone_starved_ = true;
};

class ChituBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    ChituEyesDisplay* eyes_ = nullptr;
    ChituCamera* camera_ = nullptr;
    MotorController motors_;
    ObstacleSensor obstacle_;
    WebConsole web_;
    PetMode pet_;
    Button boot_button_;
    esp_timer_handle_t wifi_retry_timer_ = nullptr;
    esp_timer_handle_t diag_timer_ = nullptr;

    // Prints mic level + wake words every 30 s (Serial Monitor "DIAG" lines)
    void StartDiagnostics() {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) {
            auto& app = Application::GetInstance();
            // Read the wake word list on the main loop (audio objects belong to it)
            app.Schedule([]() {
                auto words = Application::GetInstance().GetAudioService().GetWakeWords();
                std::string list;
                for (auto& w : words) {
                    list += (list.empty() ? "" : ", ") + w;
                }
                std::lock_guard<std::mutex> lock(audio_diag::wake_words_mutex);
                audio_diag::wake_words = list.empty() ? "none loaded" : list;
            });
            int peak = audio_diag::mic_peak;
            ESP_LOGI("DIAG", "mic level=%d peak=%d raw-offset=%d%s | wake words: %s | state: %s",
                     audio_diag::mic_rms.load(), peak, audio_diag::mic_dc.load(),
                     audio_diag::mic_clipped > 0 ? " (CLIPPING - lower MIC_GAIN)" : "",
                     audio_diag::WakeWords().c_str(),
                     DeviceStateMachine::GetStateName(app.GetDeviceState()));
        };
        args.name = "diag";
        if (esp_timer_create(&args, &diag_timer_) == ESP_OK) {
            esp_timer_start_periodic(diag_timer_, 30ULL * 1000 * 1000);
        }
    }
    int64_t config_mode_since_us_ = 0;

    // Called on every network event (in addition to xiaozhi's own handling)
    void HandleWifiRetry(NetworkEvent event) {
        if (event == NetworkEvent::WifiConfigModeEnter) {
            if (SsidManager::GetInstance().GetSsidList().empty()) {
                return;  // first setup: no saved Wi-Fi, stay in setup mode
            }
            config_mode_since_us_ = esp_timer_get_time();
            if (wifi_retry_timer_ == nullptr) {
                esp_timer_create_args_t args = {};
                args.callback = WifiRetryCheck;
                args.arg = this;
                args.name = "wifi_retry";
                esp_timer_create(&args, &wifi_retry_timer_);
            }
            esp_timer_stop(wifi_retry_timer_);
            esp_timer_start_periodic(wifi_retry_timer_, 15ULL * 1000 * 1000);
            ESP_LOGI(TAG, "Saved Wi-Fi exists: will retry it in %d s unless the setup page is used",
                     WIFI_RETRY_SECONDS);
        } else if (event == NetworkEvent::Connected || event == NetworkEvent::WifiConfigModeExit) {
            if (wifi_retry_timer_ != nullptr) {
                esp_timer_stop(wifi_retry_timer_);
            }
            if (event == NetworkEvent::Connected) {
                NetManager::GetInstance().OnConnected();
                StartWebConsole();
            }
        } else if (event == NetworkEvent::Disconnected) {
            NetManager::GetInstance().OnDisconnected();
        }
    }

    static void WifiRetryCheck(void* arg) {
        auto* self = static_cast<ChituBoard*>(arg);
        int64_t now = esp_timer_get_time();
        wifi_sta_list_t stations = {};
        if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num > 0) {
            self->config_mode_since_us_ = now;  // someone is on the setup hotspot: wait
            return;
        }
        if (now - self->config_mode_since_us_ >= (int64_t)WIFI_RETRY_SECONDS * 1000 * 1000) {
            ESP_LOGW(TAG, "Still in Wi-Fi setup mode with a saved network - restarting to retry");
            esp_restart();
        }
    }

    void InitializeDisplayI2c() {
        // I2C port 0. The esp32-camera driver always creates its SCCB bus on
        // I2C port 1 (CONFIG_SCCB_HARDWARE_I2C_PORT1) when it is given pins, and
        // deletes that port's bus if the camera init fails - so the OLED must NOT use port 1.
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = DISPLAY_I2C_ADDR,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "OLED not found on I2C (SDA=GPIO%d SCL=GPIO%d) - running without display",
                     DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        eyes_ = new ChituEyesDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = eyes_;
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;  // informational: the driver uses I2C port 1 for SCCB
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
        config.ledc_timer = LEDC_TIMER_2;
        config.ledc_channel = LEDC_CHANNEL_2;
        // JPEG straight from the sensor: no software encoding for AI photos,
        // and the phone stream can send frames as they are.
        config.pixel_format = PIXFORMAT_JPEG;
        // Buffers are sized for the photo size; the live view runs smaller (QVGA).
        // The camera is only switched on when needed (see chitu_camera.cc).
        config.frame_size = CAMERA_PHOTO_FRAMESIZE;
        config.jpeg_quality = 12;
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        // Capture only when a buffer is free. GRAB_LATEST kept the camera writing to
        // PSRAM non-stop (cam_hal FB-OVF), competing with the speech model.
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        camera_ = new ChituCamera(config);
    }

    void InitializeMotors() {
        motors_.Initialize();
        // Chitu looks where it is going
        motors_.SetMoveCallback([this](MotorController::Dir left, MotorController::Dir right) {
            if (eyes_ == nullptr) {
                return;
            }
            using D = MotorController::Dir;
            if (left == D::kBackward && right == D::kForward) {
                eyes_->LookAt(-1.0f, 0.0f);  // spinning left
            } else if (left == D::kForward && right == D::kBackward) {
                eyes_->LookAt(1.0f, 0.0f);   // spinning right
            } else if (left == D::kStop && right != D::kStop) {
                eyes_->LookAt(-0.6f, 0.0f);  // pivot on left wheel
            } else if (right == D::kStop && left != D::kStop) {
                eyes_->LookAt(0.6f, 0.0f);
            } else if (left == D::kBackward && right == D::kBackward) {
                eyes_->LookAt(0.0f, 0.6f);
            } else {
                eyes_->LookAt(0.0f, 0.0f);
            }
        });
        motors_.RegisterMcpTools();
        pet_.RegisterMcpTools();
        McpServer::GetInstance().AddTool(
            "self.chitu.get_web_console",
            "Show Chitu's phone control page (live camera, joystick, voice, messages) as a QR "
            "code on Chitu's screen. Use when the user asks for the remote / control page, the "
            "IP address or the web console. Do NOT read out any address: just tell the user to "
            "scan the QR code on your screen with their phone camera (same Wi-Fi).",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                std::string url = web_.Url();
                if (url.empty()) {
                    return std::string("{\"ok\":false,\"error\":\"Not connected to Wi-Fi yet\"}");
                }
                if (eyes_ != nullptr) {
                    eyes_->ShowQrCode(url, QR_SHOW_SECONDS);
                }
                return std::string("{\"ok\":true,\"shown\":\"QR code on screen for ") +
                       std::to_string(QR_SHOW_SECONDS) +
                       " seconds\",\"tell_user\":\"Scan the QR code on my screen with your phone camera.\"}";
            });
    }

    void InitializeObstacleSensor() {
        // TOF10120 shares the OLED's I2C wires (optional)
        if (obstacle_.Initialize(display_i2c_bus_)) {
            motors_.SetDistanceSource([this]() { return obstacle_.distance_mm(); });
        }
    }

    void StartWebConsole() {
        if (web_.running()) {
            return;
        }
        web_.SetMessageHandler([this](const std::string& text, bool say_it) {
            if (eyes_ != nullptr) {
                eyes_->ShowMessage(text, 8);
            }
            auto& app = Application::GetInstance();
            if (say_it) {
                chat_bridge::Say(chat_bridge::Kind::kReadAloud, text, true);
            } else if (app.GetDeviceState() == kDeviceStateIdle) {
                app.PlaySound(Lang::Sounds::OGG_POPUP);
            }
        });
        WebConsole::Parts parts;
        parts.motors = &motors_;
        parts.codec = GetAudioCodec();
        parts.pet = &pet_;
        parts.camera = camera_;
        parts.set_voice = [this](int p) { codec()->SetVoiceProfile(p); };
        parts.get_voice = [this]() { return codec()->voice_profile(); };
        web_.Start(parts);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (NetManager::GetInstance().offline_by_owner()) {
                NetManager::GetInstance().GoOnline();  // Wi-Fi was switched off from the phone
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    ChituBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        // Motors first so the driver is held in a safe stopped state as early as possible
        InitializeMotors();
        InitializeDisplayI2c();
        InitializeDisplay();
        InitializeObstacleSensor();
        InitializeCamera();
        InitializeButtons();
        StartDiagnostics();
        codec()->LoadVoiceProfile();
        NetManager::GetInstance().SetIdleCheck(
            []() { return Application::GetInstance().GetDeviceState() == kDeviceStateIdle; });
        NetManager::GetInstance().Start();
        LocationService::GetInstance().Start([this](const std::string& kind) {
            if (eyes_ != nullptr) eyes_->ShowInfo(kind, INFO_SHOW_SECONDS);
        });
        LocationService::GetInstance().RegisterMcpTools();
        PetMode::Hooks hooks;
        hooks.look = [this](float x, float y) {
            if (eyes_ != nullptr) eyes_->LookAt(x, y);
        };
        hooks.show_clock = [this]() {
            return eyes_ != nullptr && eyes_->ShowClock(CLOCK_SHOW_SECONDS, CLOCK_24H);
        };
        hooks.overlay_visible = [this]() { return eyes_ != nullptr && eyes_->OverlayVisible(); };
        hooks.owner_driving = [this]() { return web_.OwnerDriving(); };
        hooks.distance_mm = [this]() { return obstacle_.present() ? obstacle_.distance_mm() : -1; };
        pet_.Start(&motors_, display_, hooks);
        ESP_LOGI(TAG, "Chitu board initialized");
    }

    Led* GetLed() override {
        static GpioLed led(BUILTIN_LED_GPIO, 1);  // active low
        return &led;
    }

    // Radio power saving only when switched on in config.h (it caused dropouts on
    // some routers); otherwise the radio always stays awake.
    void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (!WIFI_POWER_SAVE || level != PowerSaveLevel::LOW_POWER) {
            level = PowerSaveLevel::PERFORMANCE;
        } else {
            level = PowerSaveLevel::BALANCED;
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    ChituAudioCodec* codec() { return static_cast<ChituAudioCodec*>(GetAudioCodec()); }

    AudioCodec* GetAudioCodec() override {
        static ChituAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_PDM_MIC_GPIO_CLK,
            AUDIO_PDM_MIC_GPIO_DATA);
        return &audio_codec;
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [this, callback](NetworkEvent event, const std::string& data) {
                HandleWifiRetry(event);
                if (callback) {
                    callback(event, data);
                }
            });
    }

    Display* GetDisplay() override { return display_; }

    Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(ChituBoard);
