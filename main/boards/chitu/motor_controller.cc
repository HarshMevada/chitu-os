#include "motor_controller.h"

#include "config.h"
#include "mcp_server.h"

#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <string>

#define TAG "ChituMotor"

namespace {
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_3;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_5;
constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kMaxDuty = (1u << 10) - 1;
constexpr int kRampSteps = 8;
constexpr int kRampStepMs = 15;

#if MOTOR_LEFT_IS_A
constexpr gpio_num_t kLeftIn1 = MOTOR_A_IN1_GPIO;
constexpr gpio_num_t kLeftIn2 = MOTOR_A_IN2_GPIO;
constexpr gpio_num_t kRightIn1 = MOTOR_B_IN1_GPIO;
constexpr gpio_num_t kRightIn2 = MOTOR_B_IN2_GPIO;
#else
constexpr gpio_num_t kLeftIn1 = MOTOR_B_IN1_GPIO;
constexpr gpio_num_t kLeftIn2 = MOTOR_B_IN2_GPIO;
constexpr gpio_num_t kRightIn1 = MOTOR_A_IN1_GPIO;
constexpr gpio_num_t kRightIn2 = MOTOR_A_IN2_GPIO;
#endif

// MCP tool error reply (builds without C++ exceptions)
std::string ErrorJson(const std::string& message) {
    return "{\"ok\":false,\"error\":\"" + message + "\"}";
}
}  // namespace

const char* MotorController::DirName(Dir d) {
    switch (d) {
        case Dir::kForward:
            return "forward";
        case Dir::kBackward:
            return "backward";
        default:
            return "stopped";
    }
}

void MotorController::Initialize() {
    // PWM low first so the driver stays off while the direction pins are set up
    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = kSpeedMode;
    timer_config.duty_resolution = kResolution;
    timer_config.timer_num = kTimer;
    timer_config.freq_hz = MOTOR_PWM_FREQ_HZ;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    ledc_channel_config_t channel_config = {};
    channel_config.gpio_num = MOTOR_PWM_GPIO;
    channel_config.speed_mode = kSpeedMode;
    channel_config.channel = kChannel;
    channel_config.timer_sel = kTimer;
    channel_config.duty = 0;
    channel_config.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << MOTOR_A_IN1_GPIO) | (1ULL << MOTOR_A_IN2_GPIO) |
                      (1ULL << MOTOR_B_IN1_GPIO) | (1ULL << MOTOR_B_IN2_GPIO);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(MOTOR_A_IN1_GPIO, 0);
    gpio_set_level(MOTOR_A_IN2_GPIO, 0);
    gpio_set_level(MOTOR_B_IN1_GPIO, 0);
    gpio_set_level(MOTOR_B_IN2_GPIO, 0);

    queue_ = xQueueCreate(4, sizeof(Command));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create command queue - motors disabled");
        return;
    }
    if (xTaskCreate(TaskEntry, "chitu_motor", 3072, this, 6, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create motor task - motors disabled");
        vQueueDelete(queue_);
        queue_ = nullptr;
        task_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "Motors ready and stopped (PWM GPIO%d @ %d Hz)", MOTOR_PWM_GPIO, MOTOR_PWM_FREQ_HZ);
}

bool MotorController::Drive(Dir left, Dir right, int speed, int duration_ms) {
    if (queue_ == nullptr) {
        return false;
    }
    Command cmd = {
        .left = left,
        .right = right,
        .speed = std::clamp(speed, 0, 100),
        .duration_ms = std::clamp(duration_ms, 0, MOTOR_MAX_DURATION_MS),
    };
    // Newest command wins: drop anything not yet started
    xQueueReset(queue_);
    return xQueueSend(queue_, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
}

bool MotorController::DriveNamed(const std::string& direction, int speed, int duration_ms) {
    if (direction == "forward") {
        return Drive(Dir::kForward, Dir::kForward, speed, duration_ms);
    } else if (direction == "backward") {
        return Drive(Dir::kBackward, Dir::kBackward, speed, duration_ms);
    } else if (direction == "left") {
        return Drive(Dir::kBackward, Dir::kForward, speed, duration_ms);
    } else if (direction == "right") {
        return Drive(Dir::kForward, Dir::kBackward, speed, duration_ms);
    }
    return false;
}

void MotorController::Stop() { Drive(Dir::kStop, Dir::kStop, 0, 0); }

void MotorController::TaskEntry(void* arg) { static_cast<MotorController*>(arg)->TaskLoop(); }

void MotorController::TaskLoop() {
    Command cmd;
    while (true) {
        if (xQueueReceive(queue_, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // Keep executing as long as new commands arrive before the current one expires
        bool have_cmd = true;
        while (have_cmd) {
            bool is_stop = (cmd.left == Dir::kStop && cmd.right == Dir::kStop) || cmd.speed == 0 ||
                           cmd.duration_ms == 0;
            if (is_stop) {
                ApplyStop();
                break;
            }
            Apply(cmd);
            // Wait for the duration; a newer command replaces this one immediately
            have_cmd = xQueueReceive(queue_, &cmd, pdMS_TO_TICKS(cmd.duration_ms)) == pdTRUE;
            if (!have_cmd) {
                ApplyStop();  // time is up
            }
        }
    }
}

uint32_t MotorController::SpeedToDuty(int speed) const {
    if (speed <= 0) {
        return 0;
    }
    int pct = MOTOR_MIN_DUTY_PERCENT + (100 - MOTOR_MIN_DUTY_PERCENT) * speed / 100;
    return kMaxDuty * std::clamp(pct, 0, 100) / 100;
}

void MotorController::SetDuty(uint32_t duty) {
    ledc_set_duty(kSpeedMode, kChannel, duty);
    ledc_update_duty(kSpeedMode, kChannel);
    current_duty_ = duty;
}

void MotorController::RampTo(uint32_t target) {
    if (target == current_duty_) {
        return;
    }
    int32_t start = static_cast<int32_t>(current_duty_);
    int32_t delta = static_cast<int32_t>(target) - start;
    for (int i = 1; i <= kRampSteps; i++) {
        SetDuty(static_cast<uint32_t>(start + delta * i / kRampSteps));
        vTaskDelay(pdMS_TO_TICKS(kRampStepMs));
    }
}

void MotorController::SetWheel(bool left, Dir dir) {
    gpio_num_t in1 = left ? kLeftIn1 : kRightIn1;
    gpio_num_t in2 = left ? kLeftIn2 : kRightIn2;
    bool invert = left ? MOTOR_LEFT_INVERT : MOTOR_RIGHT_INVERT;
    if (invert && dir != Dir::kStop) {
        dir = (dir == Dir::kForward) ? Dir::kBackward : Dir::kForward;
    }
    // TB6612: IN1=H IN2=L -> CW, IN1=L IN2=H -> CCW, L/L -> stop (coast)
    gpio_set_level(in1, dir == Dir::kForward ? 1 : 0);
    gpio_set_level(in2, dir == Dir::kBackward ? 1 : 0);
}

void MotorController::Apply(const Command& cmd) {
    bool direction_changed = static_cast<int>(cmd.left) != left_dir_.load() ||
                             static_cast<int>(cmd.right) != right_dir_.load();
    if (direction_changed && current_duty_ > 0) {
        RampTo(0);  // never reverse a spinning motor at full power
    }
    if (direction_changed) {
        SetWheel(true, cmd.left);
        SetWheel(false, cmd.right);
        left_dir_ = static_cast<int>(cmd.left);
        right_dir_ = static_cast<int>(cmd.right);
        ESP_LOGI(TAG, "Move: left=%s right=%s speed=%d%% duration=%dms", DirName(cmd.left),
                 DirName(cmd.right), cmd.speed, cmd.duration_ms);
    }
    RampTo(SpeedToDuty(cmd.speed));
    speed_ = cmd.speed;
    moving_ = true;
}

void MotorController::ApplyStop() {
    if (current_duty_ > 0) {
        RampTo(0);
    }
    SetDuty(0);
    SetWheel(true, Dir::kStop);
    SetWheel(false, Dir::kStop);
    bool was_moving = moving_.exchange(false);
    speed_ = 0;
    left_dir_ = 0;
    right_dir_ = 0;
    if (was_moving) {
        ESP_LOGI(TAG, "Stopped");
    }
}

std::string MotorController::StatusJson() const {
    return std::string("{\"moving\":") + (moving() ? "true" : "false") + ",\"left\":\"" +
           DirName(left_dir()) + "\",\"right\":\"" + DirName(right_dir()) +
           "\",\"speed\":" + std::to_string(speed()) + "}";
}

// ---------------------------------------------------------------------------
// MCP tools (lets the Xiaozhi AI move the robot by voice)
// ---------------------------------------------------------------------------
void MotorController::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.chitu.drive",
        "Move Chitu's wheeled base (Chitu is this robot). "
        "direction: forward, backward, left or right (left/right spin the robot in place). "
        "speed: 1-100 percent (default 70). duration_ms: how long to move, 100-10000 ms. "
        "About 1000 ms is a short step; a 90 degree turn is roughly 400-600 ms. "
        "The robot stops by itself when the time is up.",
        PropertyList({
            Property("direction", kPropertyTypeString, std::string("forward")),
            Property("speed", kPropertyTypeInteger, 70, 1, 100),
            Property("duration_ms", kPropertyTypeInteger, 1000, 100, MOTOR_MAX_DURATION_MS),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto direction = properties["direction"].value<std::string>();
            int speed = properties["speed"].value<int>();
            int duration = properties["duration_ms"].value<int>();
            if (!DriveNamed(direction, speed, duration)) {
                return ErrorJson("Invalid direction. Use forward, backward, left or right.");
            }
            return std::string("{\"ok\":true,\"direction\":\"") + direction +
                   "\",\"speed\":" + std::to_string(speed) +
                   ",\"duration_ms\":" + std::to_string(duration) + "}";
        });

    mcp.AddTool("self.chitu.stop", "Stop Chitu's motors immediately.", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    Stop();
                    return true;
                });

    mcp.AddTool("self.chitu.get_status",
                "Get Chitu's motor status: moving, wheel directions and speed.", PropertyList(),
                [this](const PropertyList&) -> ReturnValue { return StatusJson(); });
}
