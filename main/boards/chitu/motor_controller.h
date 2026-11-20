#pragma once

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <string>

// Drives a TB6612FNG with one shared PWM line (PWMA+PWMB tied together)
// and four direction pins.
//
// Safety:
//  - Initialize() puts every pin in the "stopped" state before anything else runs.
//  - Commands are queued and executed by one task; the newest command wins.
//  - Every command stops by itself when its time is up (max MOTOR_MAX_DURATION_MS).
//  - Speed ramps down before a wheel changes direction.
class MotorController {
public:
    enum class Dir : int8_t { kBackward = -1, kStop = 0, kForward = 1 };

    MotorController() = default;

    void Initialize();

    // speed 0-100 (%), duration in ms (clamped to MOTOR_MAX_DURATION_MS)
    bool Drive(Dir left, Dir right, int speed, int duration_ms);
    // Named directions: forward, backward, left, right. Returns false if unknown.
    bool DriveNamed(const std::string& direction, int speed, int duration_ms);
    void Stop();

    bool moving() const { return moving_.load(); }
    int speed() const { return speed_.load(); }
    Dir left_dir() const { return static_cast<Dir>(left_dir_.load()); }
    Dir right_dir() const { return static_cast<Dir>(right_dir_.load()); }

    static const char* DirName(Dir d);
    std::string StatusJson() const;

    // Registers the self.chitu.drive / stop / get_status MCP tools (voice control)
    void RegisterMcpTools();

private:
    struct Command {
        Dir left;
        Dir right;
        int speed;
        int duration_ms;
    };

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;

    std::atomic<bool> moving_{false};
    std::atomic<int> speed_{0};
    std::atomic<int> left_dir_{0};
    std::atomic<int> right_dir_{0};
    uint32_t current_duty_ = 0;

    static void TaskEntry(void* arg);
    void TaskLoop();
    void Apply(const Command& cmd);
    void ApplyStop();
    void SetWheel(bool left, Dir dir);
    void SetDuty(uint32_t duty);
    void RampTo(uint32_t target_duty);
    uint32_t SpeedToDuty(int speed) const;
};
