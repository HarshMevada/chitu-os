#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// =====================================================================
//  Chitu robot on Seeed XIAO ESP32-S3 Sense  -  Stable Baseline
//
//  XIAO pin -> GPIO map (for reference)
//    D0=GPIO1  D1=GPIO2  D2=GPIO3  D3=GPIO4  D4=GPIO5  D5=GPIO6
//    D6=GPIO43 D7=GPIO44 D8=GPIO7  D9=GPIO8  D10=GPIO9
//    D11=GPIO42 / D12=GPIO41 -> on-board PDM microphone (Sense board)
//
//  Wiring used by this firmware
//    D0  GPIO1   -> MAX98357 BCLK
//    D1  GPIO2   -> MAX98357 LRC
//    D5  GPIO6   -> MAX98357 DIN
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
// times) and a soft limiter keeps the peaks smooth.
#define SPEAKER_BOOST            6.0f
#define SPEAKER_TARGET_RMS       10000.0f
// Microphone gain. The Sense board's PDM mic is very quiet; samples are multiplied
// by this. Raise to 12-16 if it doesn't wake from across the room,
// lower to 4-6 if it hears itself or keeps triggering.
#define MIC_GAIN                 8
// Volume at every start / restart (0-100)
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
// hotspot. With a saved network, restart and try again after this many seconds in
// setup mode (unless a phone is connected to the hotspot).
#define WIFI_RETRY_SECONDS       180
// Connected before but no Wi-Fi for this long: restart the Wi-Fi station (fresh scan)
#define WIFI_WATCHDOG_SECONDS    90

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

#endif // _BOARD_CONFIG_H_
