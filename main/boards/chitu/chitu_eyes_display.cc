#include "config.h"

#include "chitu_eyes_display.h"
#include "chitu_emoji_font.h"
#include "application.h"
#include "assets/lang_config.h"

#include <ctime>

extern "C" const lv_font_t chitu_clock_font;

#include <esp_log.h>
#include <esp_random.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#define TAG "ChituEyes"

namespace {
// On the mono OLED port "black" pixels are lit and "white" pixels are off.
inline lv_color_t Lit() { return lv_color_black(); }
inline lv_color_t Off() { return lv_color_white(); }

// Expressions only: the eyes use the whole screen (the xiaozhi status bar and
// texts stay underneath and are shown only for Wi-Fi setup and activation).
constexpr int kStatusBarHeight = 0;
constexpr int kEyeW = 40;
constexpr int kEyeH = 42;
constexpr int kEyeRadius = 11;
constexpr int kEyeCenterX[2] = {35, 93};
constexpr float kLookRangeX = 16.0f;
constexpr float kLookRangeY = 9.0f;

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline uint32_t RandRange(uint32_t lo, uint32_t hi) { return lo + esp_random() % (hi - lo + 1); }

lv_obj_t* MakeBox(lv_obj_t* parent, lv_color_t color) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    return obj;
}
}  // namespace

ChituEyesDisplay::ChituEyesDisplay(esp_lcd_panel_io_handle_t panel_io,
                                             esp_lcd_panel_handle_t panel, int width, int height,
                                             bool mirror_x, bool mirror_y)
    : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {}

ChituEyesDisplay::~ChituEyesDisplay() {
    DisplayLockGuard lock(this);
    lv_anim_delete(this, nullptr);
    if (msg_timer_ != nullptr) {
        lv_timer_delete(msg_timer_);
    }
    if (heart_timer_ != nullptr) {
        lv_timer_delete(heart_timer_);
    }
    if (msg_layer_ != nullptr) {
        lv_obj_delete(msg_layer_);
    }
    if (idle_timer_ != nullptr) {
        lv_timer_delete(idle_timer_);
        idle_timer_ = nullptr;
    }
    if (layer_ != nullptr) {
        lv_obj_delete(layer_);
        layer_ = nullptr;
    }
}

void ChituEyesDisplay::SetupUI() {
    OledDisplay::SetupUI();  // status bar, notifications, chat label
    if (layer_ != nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    lv_obj_t* screen = lv_screen_active();

    layer_ = MakeBox(screen, Off());
    lv_obj_set_size(layer_, width_, height_ - kStatusBarHeight);
    lv_obj_set_pos(layer_, 0, kStatusBarHeight);
    // On top of everything xiaozhi draws (status bar, texts)
    lv_obj_move_foreground(layer_);

    for (int i = 0; i < 2; i++) {
        eye_[i] = MakeBox(layer_, Lit());
        lv_obj_set_style_radius(eye_[i], kEyeRadius, 0);
        lv_obj_set_style_clip_corner(eye_[i], true, 0);

        top_lid_[i] = MakeBox(eye_[i], Off());
        lv_obj_add_flag(top_lid_[i], LV_OBJ_FLAG_HIDDEN);

        bottom_lid_[i] = MakeBox(eye_[i], Off());
        lv_obj_set_style_radius(bottom_lid_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(bottom_lid_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Emoji next to normal text everywhere on screen (AI subtitles and messages):
    // the built-in text font falls back to the small emoji font.
    const lv_font_t* base = lv_obj_get_style_text_font(screen, LV_PART_MAIN);
    emoji_font_ = chitu_emoji_font;
    emoji_font_.fallback = base->fallback;
    text_font_ = *base;
    text_font_.fallback = &emoji_font_;
    lv_obj_set_style_text_font(screen, &text_font_, 0);

    // Hearts (loving) and "z z" (sleeping) drawn with the emoji / text font
    const int area_h = height_ - kStatusBarHeight;
    for (int i = 0; i < 2; i++) {
        heart_[i] = lv_label_create(layer_);
        lv_obj_set_style_text_font(heart_[i], &emoji_font_, 0);
        lv_obj_set_style_text_color(heart_[i], Lit(), 0);
        lv_obj_set_size(heart_[i], 16, 16);
        lv_obj_set_style_text_align(heart_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(heart_[i], "\xE2\x9D\xA4");  // U+2764 heart
        lv_obj_set_pos(heart_[i], kEyeCenterX[i] - 8, area_h / 2 - 8);
        lv_obj_set_style_transform_pivot_x(heart_[i], 8, 0);
        lv_obj_set_style_transform_pivot_y(heart_[i], 8, 0);
        lv_obj_add_flag(heart_[i], LV_OBJ_FLAG_HIDDEN);
    }
    zzz_ = lv_label_create(layer_);
    lv_obj_set_style_text_color(zzz_, Lit(), 0);
    lv_obj_set_pos(zzz_, 104, 0);
    lv_label_set_text(zzz_, "");
    lv_obj_add_flag(zzz_, LV_OBJ_FLAG_HIDDEN);

    // Message overlay (texts from the phone console)
    msg_layer_ = MakeBox(screen, Off());
    lv_obj_set_size(msg_layer_, width_, area_h);
    lv_obj_set_pos(msg_layer_, 0, kStatusBarHeight);
    lv_obj_move_foreground(msg_layer_);
    lv_obj_set_style_clip_corner(msg_layer_, false, 0);
    msg_label_ = lv_label_create(msg_layer_);
    lv_obj_set_style_text_color(msg_label_, Lit(), 0);
    lv_obj_set_style_text_font(msg_label_, &text_font_, 0);
    lv_obj_set_style_text_align(msg_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(msg_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(msg_layer_, LV_OBJ_FLAG_HIDDEN);

    // Big clock (shown now and then while idle)
    clock_layer_ = MakeBox(screen, Off());
    lv_obj_set_size(clock_layer_, width_, height_);
    lv_obj_set_pos(clock_layer_, 0, 0);
    lv_obj_move_foreground(clock_layer_);
    clock_label_ = lv_label_create(clock_layer_);
    lv_obj_set_style_text_font(clock_label_, &chitu_clock_font, 0);
    lv_obj_set_style_text_color(clock_label_, Lit(), 0);
    lv_obj_set_style_text_letter_space(clock_label_, -1, 0);
    lv_obj_center(clock_label_);
    lv_obj_add_flag(clock_layer_, LV_OBJ_FLAG_HIDDEN);

    // QR code for the phone console (shown when someone asks for the address)
    qr_layer_ = MakeBox(screen, Off());
    lv_obj_set_size(qr_layer_, width_, height_);
    lv_obj_set_pos(qr_layer_, 0, 0);
    lv_obj_move_foreground(qr_layer_);
    lv_obj_t* quiet = MakeBox(qr_layer_, Lit());  // lit square = the QR's light background
    lv_obj_set_size(quiet, height_, height_);
    lv_obj_center(quiet);
    qr_ = lv_qrcode_create(quiet);
    lv_qrcode_set_size(qr_, height_ - 6);
    lv_qrcode_set_dark_color(qr_, Off());   // dark modules = unlit pixels
    lv_qrcode_set_light_color(qr_, Lit());  // background = lit pixels
    lv_obj_center(qr_);
    lv_obj_add_flag(qr_layer_, LV_OBJ_FLAG_HIDDEN);

    bool wander = true;
    base_ = ShapeFor("neutral", &wander);
    idle_wander_ = wander;
    current_ = base_;
    from_ = base_;
    to_ = base_;
    Render();

    uint32_t now = lv_tick_get();
    next_blink_ms_ = now + 1500;
    next_glance_ms_ = now + 3000;
    idle_timer_ = lv_timer_create(IdleTimerCb, 100, this);
    ESP_LOGI(TAG, "Chitu eyes ready");
}

// ---------------------------------------------------------------------------
// Emotion -> eye shape
// ---------------------------------------------------------------------------
ChituEyesDisplay::Shape ChituEyesDisplay::ShapeFor(const char* emotion, bool* wander) {
    Shape s;
    *wander = false;
    auto both = [&s](float h, float top, float angle, float bottom) {
        for (auto& e : s.eye) {
            e.h_scale = h;
            e.top = top;
            e.angle = angle;
            e.bottom = bottom;
        }
    };
    std::string e = emotion ? emotion : "neutral";

    if (e == "neutral" || e == "relaxed" || e == "standby" || e == "eyes_nomal") {
        *wander = true;
    } else if (e == "happy" || e == "laughing" || e == "funny" || e == "delicious" ||
               e == "kissy" || e == "happy_eyes") {
        both(0.9f, 0.0f, 0.0f, 0.68f);
        s.dy = -2;
    } else if (e == "loving") {
        both(1.1f, 0.0f, 0.0f, 0.42f);
        s.w_scale = 1.08f;
    } else if (e == "sad" || e == "sad_eyes") {
        both(0.85f, 0.38f, -18.0f, 0.0f);
        s.dy = 4;
    } else if (e == "crying") {
        both(0.8f, 0.45f, -22.0f, 0.0f);
        s.dy = 5;
    } else if (e == "angry" || e == "angry_eyes") {
        both(0.9f, 0.4f, 24.0f, 0.0f);
    } else if (e == "disgusted") {
        both(0.85f, 0.3f, 14.0f, 0.22f);
    } else if (e == "surprised" || e == "shocked") {
        both(1.25f, 0.0f, 0.0f, 0.0f);
        s.w_scale = 1.12f;
        s.dy = -2;
    } else if (e == "thinking") {
        both(0.8f, 0.15f, 0.0f, 0.0f);
        s.dx = 12;
        s.dy = -6;
    } else if (e == "confused") {
        s.eye[0].h_scale = 1.1f;
        s.eye[1].h_scale = 0.65f;
        s.eye[1].top = 0.2f;
        s.eye[1].angle = -12.0f;
    } else if (e == "silly") {
        s.eye[0].h_scale = 1.2f;
        s.eye[1].h_scale = 0.6f;
        s.eye[1].bottom = 0.4f;
    } else if (e == "winking") {
        s.eye[1].h_scale = 0.12f;
        s.eye[0].bottom = 0.35f;
    } else if (e == "suspicious" || e == "suspicious_eyes") {
        s.eye[0].top = 0.55f;  // one eye squints harder
        s.eye[0].angle = 6.0f;
        s.eye[1].top = 0.32f;
        s.eye[1].angle = 4.0f;
        s.dx = -8;
    } else if (e == "sleepy") {
        both(0.9f, 0.58f, -6.0f, 0.0f);
        s.dy = 4;
    } else if (e == "cool" || e == "confident") {
        both(0.9f, 0.32f, 8.0f, 0.0f);
    } else if (e == "embarrassed") {
        both(0.85f, 0.0f, 0.0f, 0.32f);
        s.dx = -10;
        s.dy = 5;
    } else if (e == "nervous" || e == "eyes_shake") {
        both(0.85f, 0.1f, -8.0f, 0.0f);
        s.w_scale = 0.9f;
    } else if (e == "look_up_left" || e == "nhin tren trai") {
        s.dx = -kLookRangeX;
        s.dy = -kLookRangeY;
    } else if (e == "look_up_right" || e == "nhin tren phai") {
        s.dx = kLookRangeX;
        s.dy = -kLookRangeY;
    } else if (e == "look_down_left" || e == "nhin duoi trai") {
        s.dx = -kLookRangeX;
        s.dy = kLookRangeY;
    } else if (e == "look_down_right" || e == "nhin duoi phai") {
        s.dx = kLookRangeX;
        s.dy = kLookRangeY;
    } else if (e == "look_left") {
        s.dx = -kLookRangeX;
    } else if (e == "look_right") {
        s.dx = kLookRangeX;
    } else {
        *wander = true;  // unknown emotion: calm neutral eyes
    }
    return s;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void ChituEyesDisplay::Render() {
    if (layer_ == nullptr) {
        return;
    }
    const int area_h = height_ - kStatusBarHeight;
    for (int i = 0; i < 2; i++) {
        const EyeParams& p = current_.eye[i];
        int w = std::max(4, (int)lroundf(kEyeW * current_.w_scale));
        int h = std::max(2, (int)lroundf(kEyeH * p.h_scale * blink_));
        int cx = kEyeCenterX[i] + (int)lroundf(current_.dx + fx_dx_ + mode_dx_);
        int cy = area_h / 2 + (int)lroundf(current_.dy + fx_dy_ + mode_dy_);

        lv_obj_set_size(eye_[i], w, h);
        lv_obj_set_pos(eye_[i], cx - w / 2, cy - h / 2);
        lv_obj_set_style_radius(eye_[i], std::min(kEyeRadius, h / 2), 0);

        // Upper lid: an "off" rectangle whose bottom edge sits at top*h, tilted around that edge
        if (p.top > 0.01f && h > 4) {
            int lid_w = w + 24;
            int lid_h = h + 4;
            int edge = (int)lroundf(p.top * h);
            lv_obj_set_size(top_lid_[i], lid_w, lid_h);
            lv_obj_set_pos(top_lid_[i], -12, edge - lid_h);
            lv_obj_set_style_transform_pivot_x(top_lid_[i], lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(top_lid_[i], lid_h, 0);
            float angle = (i == 0) ? p.angle : -p.angle;  // mirror for the right eye
            lv_obj_set_style_transform_rotation(top_lid_[i], (int32_t)lroundf(angle * 10.0f), 0);
            lv_obj_remove_flag(top_lid_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(top_lid_[i], LV_OBJ_FLAG_HIDDEN);
        }

        // Lower lid: a wide "off" ellipse rising from below -> happy eye
        if (p.bottom > 0.01f && h > 4) {
            int lid_w = (w * 3) / 2;
            int lid_h = h;
            lv_obj_set_size(bottom_lid_[i], lid_w, lid_h);
            lv_obj_set_pos(bottom_lid_[i], (w - lid_w) / 2, h - (int)lroundf(p.bottom * h));
            lv_obj_remove_flag(bottom_lid_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bottom_lid_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ChituEyesDisplay::ShowEyes(bool show) {
    if (layer_ == nullptr) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(layer_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(layer_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------
void ChituEyesDisplay::TransitionTo(const Shape& target, uint32_t duration_ms) {
    lv_anim_delete(this, TransitionAnimCb);
    from_ = current_;
    to_ = target;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, TransitionAnimCb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, duration_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void ChituEyesDisplay::TransitionAnimCb(void* var, int32_t value) {
    auto* self = static_cast<ChituEyesDisplay*>(var);
    float t = value / 1000.0f;
    Shape& c = self->current_;
    const Shape& f = self->from_;
    const Shape& g = self->to_;
    c.w_scale = Lerp(f.w_scale, g.w_scale, t);
    c.dx = Lerp(f.dx, g.dx, t);
    c.dy = Lerp(f.dy, g.dy, t);
    for (int i = 0; i < 2; i++) {
        c.eye[i].h_scale = Lerp(f.eye[i].h_scale, g.eye[i].h_scale, t);
        c.eye[i].top = Lerp(f.eye[i].top, g.eye[i].top, t);
        c.eye[i].angle = Lerp(f.eye[i].angle, g.eye[i].angle, t);
        c.eye[i].bottom = Lerp(f.eye[i].bottom, g.eye[i].bottom, t);
    }
    self->Render();
}

void ChituEyesDisplay::BlinkAnimCb(void* var, int32_t value) {
    // value 0..2000: close during the first half, open during the second
    auto* self = static_cast<ChituEyesDisplay*>(var);
    float t = value <= 1000 ? value / 1000.0f : (2000 - value) / 1000.0f;
    self->blink_ = 1.0f - 0.92f * t;
    self->Render();
}

void ChituEyesDisplay::BlinkDoneCb(lv_anim_t* a) {
    auto* self = static_cast<ChituEyesDisplay*>(a->var);
    self->blink_ = 1.0f;
    self->blinking_ = false;
    self->Render();
}

void ChituEyesDisplay::IdleTimerCb(lv_timer_t* t) {
    auto* self = static_cast<ChituEyesDisplay*>(lv_timer_get_user_data(t));
    if (self->layer_ == nullptr || lv_obj_has_flag(self->layer_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    uint32_t now = lv_tick_get();

    if (self->sleeping_) {
        // slow "z  zZ  zZz" while asleep, no blinking or glancing
        static const char* kZ[] = {"z", "z Z", "z Z z", ""};
        if (now >= self->next_glance_ms_) {
            self->zzz_step_ = (self->zzz_step_ + 1) % 4;
            lv_label_set_text(self->zzz_, kZ[self->zzz_step_]);
            self->next_glance_ms_ = now + 700;
        }
        return;
    }

    if (!self->blinking_ && now >= self->next_blink_ms_) {
        self->blinking_ = true;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, self);
        lv_anim_set_exec_cb(&a, BlinkAnimCb);
        lv_anim_set_values(&a, 0, 2000);
        lv_anim_set_duration(&a, 160);
        lv_anim_set_completed_cb(&a, BlinkDoneCb);
        lv_anim_start(&a);
        // Occasionally a quick double blink
        self->next_blink_ms_ = now + ((esp_random() % 5 == 0) ? 260 : RandRange(2500, 6000));
    }

    if (self->idle_wander_ && !self->look_override_ && now >= self->next_glance_ms_) {
        static const float kGlances[][2] = {
            {0, 0}, {0, 0}, {-0.6f, 0}, {0.6f, 0}, {-0.4f, -0.6f}, {0.4f, -0.6f}, {0, 0.5f},
        };
        const auto& g = kGlances[esp_random() % (sizeof(kGlances) / sizeof(kGlances[0]))];
        Shape target = self->base_;
        target.dx += g[0] * kLookRangeX;
        target.dy += g[1] * kLookRangeY;
        self->TransitionTo(target, 220);
        self->next_glance_ms_ = now + RandRange(1500, 5000);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool ChituEyesDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (layer_ == nullptr) {
        return false;
    }
    std::string e = emotion ? emotion : "";
    if (e == "pet_sleep") {
        SetSleeping(true);
        return true;
    }
    SetSleeping(false);
    bool wander = false;
    base_ = ShapeFor(e == "excited" ? "happy" : emotion, &wander);
    idle_wander_ = wander;

    // Livelier expressions: a little motion on top of the eye shape
    if (e == "laughing" || e == "funny" || e == "excited") {
        StartFx(Fx::kBounce, 1600);
    } else if (e == "angry") {
        StartFx(Fx::kShake, 700);
    } else if (e == "shocked" || e == "surprised") {
        StartFx(Fx::kShake, 300);
    } else if (e == "confused" || e == "dizzy") {
        StartFx(Fx::kRoll, 1400);
    }
    if (e == "loving" || e == "kissy") {
        ShowHearts(2200);
    } else {
        HideHearts();
    }

    Shape target = base_;
    if (look_override_) {
        target.dx += look_x_ * kLookRangeX;
        target.dy += look_y_ * kLookRangeY;
    }
    TransitionTo(target, 180);
    ShowEyes(true);
    return true;
}

void ChituEyesDisplay::SetChatMessage(const char* role, const char* content) {
    OledDisplay::SetChatMessage(role, content);
    DisplayLockGuard lock(this);
    bool is_system_text =
        role != nullptr && strcmp(role, "system") == 0 && content != nullptr && content[0] != '\0';
    // Only Wi-Fi setup and the activation code are shown as text; everything else
    // (subtitles, status, errors) stays as expressions.
    auto state = Application::GetInstance().GetDeviceState();
    bool setup = state == kDeviceStateWifiConfiguring || state == kDeviceStateActivating;
    ShowEyes(!(is_system_text && setup));
}

void ChituEyesDisplay::SetStatus(const char* status) {
    OledDisplay::SetStatus(status);  // kept underneath for setup screens
    if (status == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (layer_ == nullptr) {
        return;
    }
    using namespace Lang::Strings;
    Mode mode = Mode::kIdle;
    if (strcmp(status, LISTENING) == 0) {
        mode = Mode::kListening;
    } else if (strcmp(status, SPEAKING) == 0) {
        mode = Mode::kSpeaking;
    } else if (strcmp(status, CONNECTING) == 0 || strcmp(status, LOADING_PROTOCOL) == 0 ||
               strcmp(status, CHECKING_NEW_VERSION) == 0 || strcmp(status, REGISTERING_NETWORK) == 0) {
        mode = Mode::kThinking;
    }
    if (mode == mode_) {
        return;
    }
    mode_ = mode;
    lv_anim_delete(this, ModeAnimCb);
    mode_dx_ = 0;
    mode_dy_ = 0;
    if (mode != Mode::kIdle) {
        SetSleeping(false);
        // the clock makes way; a QR code someone asked for stays up
        if (clock_layer_ != nullptr && !lv_obj_has_flag(clock_layer_, LV_OBJ_FLAG_HIDDEN)) {
            HideOverlays();
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, this);
        lv_anim_set_exec_cb(&a, ModeAnimCb);
        lv_anim_set_values(&a, 0, 100000);
        lv_anim_set_duration(&a, 100000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    if (mode == Mode::kListening) {
        // all ears: big round eyes looking up at you
        bool wander = false;
        base_ = ShapeFor("surprised", &wander);
        base_.dy -= 3;
        idle_wander_ = false;
        TransitionTo(base_, 200);
    } else if (mode == Mode::kThinking) {
        bool wander = false;
        base_ = ShapeFor("thinking", &wander);
        idle_wander_ = false;
        TransitionTo(base_, 250);
    } else if (mode == Mode::kIdle && !sleeping_) {
        bool wander = true;
        base_ = ShapeFor("neutral", &wander);  // calm again after a conversation
        idle_wander_ = wander;
        TransitionTo(base_, 300);
    }
}

// Small continuous motion that shows what Chitu is doing
void ChituEyesDisplay::ModeAnimCb(void* var, int32_t value) {
    auto* self = static_cast<ChituEyesDisplay*>(var);
    float t = value / 1000.0f;
    constexpr float kTwoPi = 6.2831853f;
    switch (self->mode_) {
        case Mode::kListening:  // slow "breathing" lean, like paying attention
            self->mode_dx_ = 0;
            self->mode_dy_ = 1.5f * sinf(t * kTwoPi / 1.4f);
            break;
        case Mode::kSpeaking:  // little nods while talking
            self->mode_dx_ = 0;
            self->mode_dy_ = -2.0f * fabsf(sinf(t * kTwoPi / 0.55f));
            break;
        case Mode::kThinking:  // eyes circle slowly, up and to the side
            self->mode_dx_ = 6.0f * cosf(t * kTwoPi / 1.6f) + 4.0f;
            self->mode_dy_ = 3.0f * sinf(t * kTwoPi / 1.6f) - 4.0f;
            break;
        default:
            self->mode_dx_ = 0;
            self->mode_dy_ = 0;
    }
    self->Render();
}

void ChituEyesDisplay::LookAt(float x, float y) {
    DisplayLockGuard lock(this);
    if (layer_ == nullptr) {
        return;
    }
    look_x_ = std::clamp(x, -1.0f, 1.0f);
    look_y_ = std::clamp(y, -1.0f, 1.0f);
    look_override_ = (look_x_ != 0.0f || look_y_ != 0.0f);
    Shape target = base_;
    target.dx += look_x_ * kLookRangeX;
    target.dy += look_y_ * kLookRangeY;
    TransitionTo(target, 200);
    next_glance_ms_ = lv_tick_get() + 2000;
}

// ---------------------------------------------------------------------------
// Expressions: bounce / shake / roll, hearts, sleeping
// ---------------------------------------------------------------------------
void ChituEyesDisplay::StartFx(Fx fx, uint32_t duration_ms) {
    lv_anim_delete(this, FxAnimCb);
    fx_ = fx;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, FxAnimCb);
    lv_anim_set_values(&a, 0, (int32_t)duration_ms);
    lv_anim_set_duration(&a, duration_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_completed_cb(&a, FxDoneCb);
    lv_anim_start(&a);
}

void ChituEyesDisplay::FxAnimCb(void* var, int32_t value) {
    auto* self = static_cast<ChituEyesDisplay*>(var);
    constexpr float kTwoPi = 6.2831853f;
    float t = value / 1000.0f;  // seconds
    switch (self->fx_) {
        case Fx::kBounce:  // happy hops
            self->fx_dx_ = 0;
            self->fx_dy_ = -5.0f * fabsf(sinf(t * kTwoPi / 0.36f));
            break;
        case Fx::kShake:  // quick side-to-side
            self->fx_dx_ = 3.0f * sinf(t * kTwoPi / 0.09f);
            self->fx_dy_ = 0;
            break;
        case Fx::kRoll:  // eyes go round in circles
            self->fx_dx_ = 9.0f * cosf(t * kTwoPi / 0.7f) - 9.0f;
            self->fx_dy_ = 5.0f * sinf(t * kTwoPi / 0.7f);
            break;
        default:
            self->fx_dx_ = 0;
            self->fx_dy_ = 0;
    }
    self->Render();
}

void ChituEyesDisplay::FxDoneCb(lv_anim_t* a) {
    auto* self = static_cast<ChituEyesDisplay*>(a->var);
    self->fx_ = Fx::kNone;
    self->fx_dx_ = 0;
    self->fx_dy_ = 0;
    self->Render();
}

void ChituEyesDisplay::HeartPulseCb(void* var, int32_t value) {
    auto* self = static_cast<ChituEyesDisplay*>(var);
    for (auto* h : self->heart_) {
        lv_obj_set_style_transform_scale(h, value, 0);
    }
}

void ChituEyesDisplay::ShowHearts(uint32_t duration_ms) {
    for (int i = 0; i < 2; i++) {
        lv_obj_add_flag(eye_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(heart_[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_anim_delete(this, HeartPulseCb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, HeartPulseCb);
    lv_anim_set_values(&a, 520, 680);  // 2.0x .. 2.65x, beating
    lv_anim_set_duration(&a, 260);
    lv_anim_set_reverse_duration(&a, 260);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    if (heart_timer_ != nullptr) {
        lv_timer_delete(heart_timer_);
    }
    heart_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<ChituEyesDisplay*>(lv_timer_get_user_data(t));
            self->heart_timer_ = nullptr;
            lv_timer_delete(t);
            self->HideHearts();
        },
        duration_ms, this);
}

void ChituEyesDisplay::HideHearts() {
    if (heart_[0] == nullptr || lv_obj_has_flag(heart_[0], LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_anim_delete(this, HeartPulseCb);
    if (heart_timer_ != nullptr) {
        lv_timer_delete(heart_timer_);
        heart_timer_ = nullptr;
    }
    for (int i = 0; i < 2; i++) {
        lv_obj_add_flag(heart_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(eye_[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void ChituEyesDisplay::SetSleeping(bool sleeping) {
    if (sleeping == sleeping_ || zzz_ == nullptr) {
        return;
    }
    sleeping_ = sleeping;
    if (sleeping) {
        HideHearts();
        bool wander = false;
        base_ = ShapeFor("sleepy", &wander);
        Shape closed = base_;
        for (auto& e : closed.eye) {
            e.h_scale = 0.12f;  // eyes shut: two thin lines
            e.top = 0.0f;
        }
        closed.dy = 6;
        idle_wander_ = false;
        TransitionTo(closed, 900);
        zzz_step_ = 0;
        lv_label_set_text(zzz_, "z");
        lv_obj_remove_flag(zzz_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(zzz_, LV_OBJ_FLAG_HIDDEN);
        next_glance_ms_ = lv_tick_get() + 1500;
    }
}

// ---------------------------------------------------------------------------
// Messages from the phone
// ---------------------------------------------------------------------------
namespace {
// Rough check: is the text only emoji (and spaces)? Then it is shown big.
int EmojiOnlyCount(const std::string& s) {
    int count = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        i += len;
        if (cp == ' ') continue;
        if (cp < 0x2190) return 0;
        count++;
    }
    return count;
}
}  // namespace

void ChituEyesDisplay::MsgScrollCb(void* var, int32_t value) {
    lv_obj_set_y(static_cast<lv_obj_t*>(var), value);
}

void ChituEyesDisplay::ShowMessage(const std::string& text, int seconds) {
    DisplayLockGuard lock(this);
    if (msg_layer_ == nullptr) {
        return;
    }
    const int area_h = height_ - kStatusBarHeight;
    HideOverlays();
    lv_anim_delete(msg_label_, MsgScrollCb);
    lv_label_set_text(msg_label_, text.c_str());

    int emoji = EmojiOnlyCount(text);
    if (emoji > 0 && emoji <= 4) {
        // just emoji: show them big
        lv_obj_set_width(msg_label_, LV_SIZE_CONTENT);
        lv_obj_update_layout(msg_label_);
        int w = lv_obj_get_width(msg_label_);
        int h = lv_obj_get_height(msg_label_);
        int scale = emoji <= 2 ? 640 : 448;  // 2.5x or 1.75x
        lv_obj_set_style_transform_pivot_x(msg_label_, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(msg_label_, h / 2, 0);
        lv_obj_set_style_transform_scale(msg_label_, scale, 0);
        lv_obj_set_pos(msg_label_, (width_ - w) / 2, (area_h - h) / 2);
    } else {
        lv_obj_set_style_transform_scale(msg_label_, 256, 0);
        lv_obj_set_width(msg_label_, width_ - 4);
        lv_obj_set_pos(msg_label_, 2, 0);
        lv_obj_update_layout(msg_label_);
        int h = lv_obj_get_height(msg_label_);
        if (h <= area_h) {
            lv_obj_set_y(msg_label_, (area_h - h) / 2);
        } else {
            // long text: scroll slowly up and back
            int travel = h - area_h;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, msg_label_);
            lv_anim_set_exec_cb(&a, MsgScrollCb);
            lv_anim_set_values(&a, 0, -travel);
            lv_anim_set_duration(&a, 700 + travel * 60);
            lv_anim_set_delay(&a, 1200);
            lv_anim_set_reverse_delay(&a, 1500);
            lv_anim_set_reverse_duration(&a, 400);
            lv_anim_set_repeat_delay(&a, 1200);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
            seconds = std::max(seconds, 4 + h / 8);
        }
    }
    lv_obj_remove_flag(msg_layer_, LV_OBJ_FLAG_HIDDEN);

    if (msg_timer_ != nullptr) {
        lv_timer_delete(msg_timer_);
    }
    msg_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<ChituEyesDisplay*>(lv_timer_get_user_data(t));
            self->msg_timer_ = nullptr;
            lv_timer_delete(t);
            self->HideMessage();
        },
        seconds * 1000, this);
}

void ChituEyesDisplay::HideMessage() {
    lv_anim_delete(msg_label_, MsgScrollCb);
    lv_obj_add_flag(msg_layer_, LV_OBJ_FLAG_HIDDEN);
    // a happy blink after reading the message
    SetSleeping(false);
    StartFx(Fx::kBounce, 700);
}

// ---------------------------------------------------------------------------
// Clock and QR code
// ---------------------------------------------------------------------------
void ChituEyesDisplay::HideOverlays() {
    if (overlay_timer_ != nullptr) {
        lv_timer_delete(overlay_timer_);
        overlay_timer_ = nullptr;
    }
    if (clock_layer_ != nullptr) lv_obj_add_flag(clock_layer_, LV_OBJ_FLAG_HIDDEN);
    if (qr_layer_ != nullptr) lv_obj_add_flag(qr_layer_, LV_OBJ_FLAG_HIDDEN);
}

void ChituEyesDisplay::ShowOverlayFor(lv_obj_t* layer, int seconds) {
    HideOverlays();
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_HIDDEN);
    overlay_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<ChituEyesDisplay*>(lv_timer_get_user_data(t));
            self->overlay_timer_ = nullptr;
            lv_timer_delete(t);
            self->HideOverlays();
            self->next_blink_ms_ = lv_tick_get();  // blink when the eyes come back
        },
        seconds * 1000, this);
}

bool ChituEyesDisplay::ShowClock(int seconds, bool hour24) {
    time_t now = time(nullptr);
    if (now < 1700000000) {
        return false;  // clock not set yet
    }
    struct tm t;
    localtime_r(&now, &t);
    int h = t.tm_hour;
    if (!hour24) {
        h = h % 12;
        if (h == 0) h = 12;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d:%02d", h, t.tm_min);
    DisplayLockGuard lock(this);
    if (clock_layer_ == nullptr || (msg_layer_ && !lv_obj_has_flag(msg_layer_, LV_OBJ_FLAG_HIDDEN)) ||
        (qr_layer_ && !lv_obj_has_flag(qr_layer_, LV_OBJ_FLAG_HIDDEN))) {
        return false;  // a message or QR code is on screen
    }
    lv_label_set_text(clock_label_, buf);
    lv_obj_center(clock_label_);
    ShowOverlayFor(clock_layer_, seconds);
    return true;
}

void ChituEyesDisplay::ShowQrCode(const std::string& text, int seconds) {
    DisplayLockGuard lock(this);
    if (qr_ == nullptr) {
        return;
    }
    lv_qrcode_update(qr_, text.data(), text.size());
    lv_obj_center(qr_);
    ShowOverlayFor(qr_layer_, seconds);
}

