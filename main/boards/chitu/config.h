#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// =====================================================================
//  Chitu robot on Seeed XIAO ESP32-S3 Sense
//
//  XIAO pin -> GPIO map (for reference)
//    D0=GPIO1  D1=GPIO2  D2=GPIO3  D3=GPIO4  D4=GPIO5  D5=GPIO6
//    D6=GPIO43 D7=GPIO44 D8=GPIO7  D9=GPIO8  D10=GPIO9
//    D11=GPIO42 / D12=GPIO41 -> on-board PDM microphone (Sense board)
//
//  Wiring used by this firmware
//    D0  GPIO1   -> MAX98357 BCLK
//    D1  GPIO2   -> MAX98357 LRC
//    D5  GPIO6   -> MAX98357 DIN          (moved from D11: GPIO42 is the mic clock)
//    D3  GPIO4   -> TB6612 AIN1
//    D6  GPIO43  -> TB6612 AIN2
//    D7  GPIO44  -> TB6612 BIN1
//    D2  GPIO3   -> TB6612 BIN2
//    D9  GPIO8   -> TB6612 PWMA + PWMB    (both PWM inputs joined, one shared speed)
//    D4  GPIO5   -> SH1106 OLED SDA
//    D10 GPIO9   -> SH1106 OLED SCL
//    D8  GPIO7   -> NOT USED (damaged)
//    TB6612 STBY -> 3.3V
// =====================================================================

#include <driver/gpio.h>

// ---------------- Audio ----------------
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Speaker loudness. Speech is lifted towards SPEAKER_TARGET_RMS (up to SPEAKER_BOOST
// times) and a soft limiter keeps the peaks smooth. Raise SPEAKER_TARGET_RMS for louder,
// lower it (e.g. 7000) if it sounds harsh. For even more volume, wire the MAX98357
// GAIN pin to GND through a 100k resistor (+15 dB instead of the default +9 dB).
#define SPEAKER_BOOST            6.0f
#define SPEAKER_TARGET_RMS       10000.0f
// Microphone gain. The Sense board's PDM mic is very quiet; samples are multiplied
// by this (whole number). Raise to 12-16 if it doesn't wake from across the room,
// lower to 4-6 if it hears itself or keeps triggering.
#define MIC_GAIN                 8
// Volume on first boot (0-100). Later changes ("set volume to 80") are remembered.
#define DEFAULT_OUTPUT_VOLUME    100

// MAX98357 I2S amplifier
#define AUDIO_I2S_SPK_GPIO_BCLK  GPIO_NUM_1   // D0
#define AUDIO_I2S_SPK_GPIO_LRCK  GPIO_NUM_2   // D1
#define AUDIO_I2S_SPK_GPIO_DOUT  GPIO_NUM_6   // D5

// On-board PDM microphone of the XIAO ESP32-S3 Sense (fixed wiring)
#define AUDIO_PDM_MIC_GPIO_CLK   GPIO_NUM_42
#define AUDIO_PDM_MIC_GPIO_DATA  GPIO_NUM_41

// ---------------- Buttons / LED ----------------
#define BOOT_BUTTON_GPIO         GPIO_NUM_0
#define BUILTIN_LED_GPIO         GPIO_NUM_21  // orange user LED, active low

// ---------------- OLED (SH1106 128x64, I2C) ----------------
#define DISPLAY_SDA_PIN          GPIO_NUM_5   // D4
#define DISPLAY_SCL_PIN          GPIO_NUM_9   // D10
#define DISPLAY_I2C_ADDR         0x3C
#define DISPLAY_WIDTH            128
#define DISPLAY_HEIGHT           64
// Flip these if the picture is upside down / mirrored on your mounting
#define DISPLAY_MIRROR_X         false
#define DISPLAY_MIRROR_Y         false

// ---------------- Wi-Fi ----------------
// If the saved Wi-Fi can't be reached at power-up, xiaozhi falls back to the setup
// hotspot and would stay there forever. With a saved network, restart and try again
// after this many seconds in setup mode (unless a phone is connected to the hotspot).
#define WIFI_RETRY_SECONDS       180

// ---------------- Motors (TB6612FNG) ----------------
#define MOTOR_A_IN1_GPIO         GPIO_NUM_4   // D3  AIN1
#define MOTOR_A_IN2_GPIO         GPIO_NUM_43  // D6  AIN2
#define MOTOR_B_IN1_GPIO         GPIO_NUM_44  // D7  BIN1
#define MOTOR_B_IN2_GPIO         GPIO_NUM_3   // D2  BIN2
#define MOTOR_PWM_GPIO           GPIO_NUM_8   // D9  PWMA + PWMB

// Which TB6612 channel is the left wheel (0 = motor A, 1 = motor B)
#define MOTOR_LEFT_IS_A          1
// Set to 1 if a wheel spins the wrong way (easier than swapping wires)
#define MOTOR_LEFT_INVERT        0
#define MOTOR_RIGHT_INVERT       0

#define MOTOR_PWM_FREQ_HZ        20000  // above hearing range, no whine
#define MOTOR_MIN_DUTY_PERCENT   35     // below this small gear motors just hum
#define MOTOR_MAX_DURATION_MS    10000  // safety cap per command

// ---------------- Obstacle sensor (TOF10120, optional) ----------------
// Shares the OLED I2C bus: SDA -> D4, SCL -> D10, VDD -> 3V3, GND -> GND.
// Without it the robot still works; roaming is simply refused.
#define TOF_I2C_ADDR             0x52
#define TOF_I2C_SPEED_HZ         100000
#define OBSTACLE_STOP_MM         200    // forward moves stop closer than this
#define ROAM_AVOID_MM            320    // roaming turns away closer than this
#define ROAM_SPEED               60     // percent
#define ROAM_TURN_MS_MIN         350
#define ROAM_TURN_MS_MAX         900
#define ROAM_MAX_MINUTES         30

// ---------------- Phone web console ----------------
#define WEB_CONSOLE_PORT         80     // control page
#define WEB_CONSOLE_PORT_SECURE 443   // https page (needed for the phone microphone)

// ---------------- Camera (XIAO ESP32-S3 Sense, OV3660 / OV2640) ----------------
#define CAMERA_PIN_PWDN          GPIO_NUM_NC
#define CAMERA_PIN_RESET         GPIO_NUM_NC
#define CAMERA_PIN_XCLK          GPIO_NUM_10
#define CAMERA_PIN_SIOD          GPIO_NUM_40
#define CAMERA_PIN_SIOC          GPIO_NUM_39
#define CAMERA_PIN_D7            GPIO_NUM_48
#define CAMERA_PIN_D6            GPIO_NUM_11
#define CAMERA_PIN_D5            GPIO_NUM_12
#define CAMERA_PIN_D4            GPIO_NUM_14
#define CAMERA_PIN_D3            GPIO_NUM_16
#define CAMERA_PIN_D2            GPIO_NUM_18
#define CAMERA_PIN_D1            GPIO_NUM_17
#define CAMERA_PIN_D0            GPIO_NUM_15
#define CAMERA_PIN_VSYNC         GPIO_NUM_38
#define CAMERA_PIN_HREF          GPIO_NUM_47
#define CAMERA_PIN_PCLK          GPIO_NUM_13
#define CAMERA_XCLK_FREQ_HZ      20000000

#endif // _BOARD_CONFIG_H_

// Default: rotated 180 deg. Fine-tune with the Flip / Mirror buttons in the phone console
// (that choice is saved on the robot and survives restarts).
#define CAMERA_HMIRROR 1
#define CAMERA_VFLIP   1

// ---------------- Personality (what Chitu does on its own) ----------------
// Switch on/off in the phone console (Chitu tab). Times are minutes without attention.
#define PET_BORED_AFTER_MINUTES  3           // calm/curious -> bored/playful
#define PET_SLEEPY_AFTER_MINUTES 10          // -> sleepy (yawns, droopy eyes)
#define PET_SLEEP_AFTER_MINUTES  15          // -> asleep (eyes closed, "z z")
#define PET_QUIET_FROM_HOUR      22          // no sounds or chatter from 22:00 ...
#define PET_QUIET_TO_HOUR        8           // ... until 08:00 (local time)
#define PET_CHAT_MIN_MINUTES     6           // "chatty": speaks up on its own every 6-15 min
#define PET_CHAT_MAX_MINUTES     15          // (only while bored/playful, never at night)
#define PET_MOVES                1           // 1 = small moves on the spot, 0 = face only
#define PET_LOUD_SOUND_REACT     1           // 1 = startles at sudden loud sounds
#define PET_MUSIC_DANCE          1           // 1 = dances when it hears rhythmic music

// ---------------- Voice profile (applies to everything Chitu says or plays) ----------------
// The voice itself (male/female, language) is chosen on xiaozhi.me (Agent -> Voice).
// On top of that the robot can change how it sounds:
//   0 = natural   (as sent by the server)
//   1 = cute      (higher pitch, +4 semitones)
//   2 = tiny      (very high, +7 semitones)
//   3 = deep      (lower pitch, -4 semitones)
//   4 = robot     (metallic ring effect)
// Can also be changed later in the phone console (Chitu tab); that choice is saved.
#define VOICE_PROFILE            0

// ---------------- Screen ----------------
// Expressions only. While idle a big clock appears now and then.
#define CLOCK_EVERY_SECONDS      90          // show the clock every 90 s while calm (0 = never)
#define CLOCK_SHOW_SECONDS       4           // ... for 4 seconds
#define CLOCK_24H                0           // 0 = 12-hour (7:05), 1 = 24-hour (19:05)
#define QR_SHOW_SECONDS          45          // phone console QR code stays this long
#define INFO_SHOW_SECONDS        8           // time / date / weather page after a question

// ---------------- Time, place and weather ----------------
// The city is found automatically from the internet address (approximate); set the
// right city in the phone console (Setup tab) to override it. Times come from the
// internet time servers (NTP), weather from open-meteo.com (no account needed).
#define DEFAULT_TIMEZONE         "IST-5:30"  // used until a location is known (POSIX TZ)
#define WEATHER_REFRESH_MINUTES  20
#define WEATHER_MAX_AGE_MINUTES  90          // older weather is reported as unavailable
#define TEMP_FAHRENHEIT          0           // 1 = show and say temperatures in F

// ---------------- Camera ----------------
// The camera is OFF unless needed: a photo question powers it up for a moment,
// the phone console's live view keeps it on while you watch.
#define CAMERA_IDLE_OFF_SECONDS  15          // switch off this long after the last use
#define CAMERA_STREAM_FRAMESIZE  FRAMESIZE_QVGA   // live view 320x240 (smooth on Wi-Fi)
#define CAMERA_PHOTO_FRAMESIZE   FRAMESIZE_VGA    // AI photos 640x480

// ---------------- Wi-Fi stability ----------------
// 0 = no power saving (most stable, ~+40 mA), 1 = light power saving
#define WIFI_POWER_SAVE          0
// Go back to the preferred (first) saved network when away from it for this long
#define WIFI_PREFERRED_RETRY_MINUTES 15
