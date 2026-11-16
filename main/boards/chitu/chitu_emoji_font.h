#pragma once
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif
// ~200 common emoji (Noto Emoji, SIL Open Font License 1.1), 1 bpp.
extern const lv_font_t chitu_emoji_font;      // 20 px, next to message text
extern const lv_font_t chitu_emoji_big_font;  // 40 px, emoji-only messages and hearts
// Message text (Inter SemiBold 19 px, ASCII) and the big clock digits
extern const lv_font_t chitu_text_font;
extern const lv_font_t chitu_clock_font;
#ifdef __cplusplus
}
#endif
