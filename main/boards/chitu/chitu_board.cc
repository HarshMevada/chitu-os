// Chitu robot: Seeed XIAO ESP32-S3 Sense + MAX98357 + TB6612FNG + SH1106 OLED
// Stable Baseline: Xiaozhi voice + "Chitu" wake word + Xiaozhi OLED screen + motors + Wi-Fi.
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "display/oled_display.h"
#include "led/gpio_led.h"
#include "motor_controller.h"
#include "net_manager.h"

#include <ssid_manager.h>

#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_sh1106.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <cmath>
#include <vector>

#define TAG "ChituBoard"

// Audio codec tuned for this robot's hardware:
//  - microphone: removes the Sense PDM mic's large DC offset, then multiplies the
//    quiet signal by MIC_GAIN (needed for the wake word to hear you)
//  - speaker: lifts speech towards SPEAKER_TARGET_RMS (up to SPEAKER_BOOST times) with a
//    soft limiter on the peaks, and starts at DEFAULT_OUTPUT_VOLUME
class ChituAudioCodec : public NoAudioCodecSimplexPdm {
public:
    ChituAudioCodec(int input_rate, int output_rate, gpio_num_t bclk, gpio_num_t ws,
                    gpio_num_t dout, gpio_num_t mic_clk, gpio_num_t mic_din)
        : NoAudioCodecSimplexPdm(input_rate, output_rate, bclk, ws, dout, mic_clk, mic_din) {
        output_volume_ = DEFAULT_OUTPUT_VOLUME;
        input_gain_ = 0;  // gain is applied in Read() after removing the DC offset
    }

    void Start() override {
        NoAudioCodecSimplexPdm::Start();
        output_volume_ = DEFAULT_OUTPUT_VOLUME;
        ESP_LOGI(TAG, "Speaker volume %d%% at start-up", output_volume_);
    }

    int Read(int16_t* dest, int samples) override {
        int n = NoAudioCodecSimplexPdm::Read(dest, samples);
        for (int i = 0; i < n; i++) {
            // DC-blocking high-pass filter, then gain
            float x = dest[i];
            float y = x - hp_prev_x_ + 0.995f * hp_prev_y_;
            hp_prev_x_ = x;
            hp_prev_y_ = y;
            float v = y * MIC_GAIN;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32767.0f) v = -32767.0f;
            dest[i] = (int16_t)v;
        }
        return n;
    }

protected:
    int Write(const int16_t* data, int samples) override {
        double sum_sq = 0;
        for (int i = 0; i < samples; i++) {
            sum_sq += (double)data[i] * data[i];
        }
        float rms = sqrtf((float)(sum_sq / (samples > 0 ? samples : 1)));
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
    float gain_ = 1.0f;
    float hp_prev_x_ = 0.0f;
    float hp_prev_y_ = 0.0f;
};

class ChituBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    MotorController motors_;
    Button boot_button_;
    esp_timer_handle_t wifi_retry_timer_ = nullptr;
    int64_t config_mode_since_us_ = 0;

    // ---- Wi-Fi: never get stuck in setup mode when a network is already saved ----
    void HandleNetworkEvent(NetworkEvent event) {
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

    // ---- OLED (SH1106 on I2C port 0) ----
    void InitializeDisplayI2c() {
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

        // Xiaozhi's own OLED screen: status bar, original emotions and chat text
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    // ---- BOOT button: Xiaozhi's standard behaviour ----
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    ChituBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        // Motors first so the driver is held stopped as early as possible
        motors_.Initialize();
        motors_.RegisterMcpTools();
        InitializeDisplayI2c();
        InitializeDisplay();
        InitializeButtons();
        NetManager::GetInstance().Start();
        ESP_LOGI(TAG, "Chitu board initialized");
    }

    Led* GetLed() override {
        static GpioLed led(BUILTIN_LED_GPIO, 1);  // active low
        return &led;
    }

    // Radio power saving off: the most stable connection
    void SetPowerSaveLevel(PowerSaveLevel) override {
        WifiBoard::SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }

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
                HandleNetworkEvent(event);
                if (callback) {
                    callback(event, data);
                }
            });
    }

    Display* GetDisplay() override { return display_; }
};

DECLARE_BOARD(ChituBoard);
