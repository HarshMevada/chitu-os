#include "chat_bridge.h"

#include "application.h"
#include "device_state_machine.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <atomic>
#include <cstring>

#define TAG "ChatBridge"

namespace chat_bridge {
namespace {

struct Request {
    Kind kind;
    bool interrupt;
    char text[520];
};

QueueHandle_t queue = nullptr;
std::atomic<int> pending{0};

std::string Instruction(const Request& r) {
    std::string t = r.text;
    if (r.kind == Kind::kReadAloud) {
        return "(Please read this message out loud exactly as written, word for word, without "
               "changing or summarising it. Then react to it in one short sentence. The message: \"" +
               t + "\")";
    }
    return t;
}

void Deliver(const Request& r) {
    auto& app = Application::GetInstance();
    int64_t deadline = esp_timer_get_time() + 12LL * 1000 * 1000;
    bool asked_to_stop = false;
    // Wait until Chitu is free (stopping it if asked)
    while (esp_timer_get_time() < deadline) {
        auto state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            break;
        }
        if (!asked_to_stop) {
            if (state == kDeviceStateSpeaking && r.interrupt) {
                app.Schedule([]() { Application::GetInstance().AbortSpeaking(kAbortReasonNone); });
                asked_to_stop = true;
            } else if (state == kDeviceStateListening) {
                // a conversation is open but nobody is talking: close it first
                app.StopListening();
                asked_to_stop = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (app.GetDeviceState() != kDeviceStateIdle) {
        // still busy (e.g. a long answer): try once more by stopping it
        app.Schedule([]() { Application::GetInstance().AbortSpeaking(kAbortReasonNone); });
        vTaskDelay(pdMS_TO_TICKS(800));
        if (app.GetDeviceState() == kDeviceStateListening) {
            app.StopListening();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGW(TAG, "Chitu stayed busy - request dropped");
        return;
    }
    ESP_LOGI(TAG, "Delivering %s request (%u chars)", r.kind == Kind::kReadAloud ? "read-aloud" : "prompt",
             (unsigned)strlen(r.text));
    // The text goes to the server as the conversation opener (same path as a wake word)
    app.WakeWordInvoke(Instruction(r));
}

void Worker(void*) {
    Request r;
    while (true) {
        if (xQueueReceive(queue, &r, portMAX_DELAY) == pdTRUE) {
            Deliver(r);
            pending--;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

bool Ensure() {
    if (queue != nullptr) {
        return true;
    }
    queue = xQueueCreate(3, sizeof(Request));
    if (queue == nullptr) {
        return false;
    }
    return xTaskCreate(Worker, "chitu_say", 4096, nullptr, 3, nullptr) == pdPASS;
}

}  // namespace

bool Say(Kind kind, const std::string& text, bool interrupt) {
    if (!WifiManager::GetInstance().IsConnected() || text.empty() || !Ensure()) {
        return false;
    }
    Request r = {};
    r.kind = kind;
    r.interrupt = interrupt;
    strncpy(r.text, text.c_str(), sizeof(r.text) - 1);
    if (xQueueSend(queue, &r, 0) != pdTRUE) {
        return false;  // a few requests are already waiting
    }
    pending++;
    return true;
}

void StopTalking() {
    auto& app = Application::GetInstance();
    app.Schedule([]() {
        auto& a = Application::GetInstance();
        auto state = a.GetDeviceState();
        if (state == kDeviceStateSpeaking) {
            a.AbortSpeaking(kAbortReasonNone);
        }
    });
}

bool Pending() { return pending.load() > 0; }

}  // namespace chat_bridge
