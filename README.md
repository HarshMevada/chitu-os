# Chitu OS

Firmware for **Chitu**, a little AI desk robot (Seeed XIAO ESP32-S3 Sense, SH1106 OLED,
MAX98357 speaker, TB6612 motor driver).

**Install from the browser:** https://harshmevada.github.io/chitu-os/ (Chrome or Edge on a computer)

## Current release: Chitu v1.0.0 – Stable Baseline
Only the essentials, so every later feature can be added and tested one at a time:

| Feature | Where |
|---|---|
| Xiaozhi AI voice chat (xiaozhi.me), unchanged Xiaozhi core | `main/` |
| Wake word **"Chitu"** (trained microWakeWord model), the only wake word | `main/assets/wakeword/chitu.*` |
| Xiaozhi's own OLED screen: status bar, original emotions, chat text | `main/display/oled_display.*` |
| Motors: forward / backward / left / right / stop by voice, auto-stop timer | `main/boards/chitu/motor_controller.*` |
| Wi-Fi: saved networks, auto-reconnect, watchdog, setup-mode retry | `main/boards/chitu/net_manager.*`, `chitu_board.cc` |
| Mic DC fix + gain, speaker loudness with soft limiter (hardware tuning) | `main/boards/chitu/chitu_board.cc` |
| Browser install page showing the firmware version | `site/` |

Feature history and the testing checklist: `docs/BASELINE.md`.

## Version
Change it in **one place**: `PROJECT_VER` and `CHITU_RELEASE_NAME` in `CMakeLists.txt`.
It appears on the OLED at start-up (`chitu/1.0.0`), on the install page, and on xiaozhi.me.

## Build locally
ESP-IDF 6.1, then:
```
python scripts/build.py chitu --wake-word micro:chitu
```

## Credits
Chitu OS is based on [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (MIT License,
Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.), imported at commit
`1b7644a6fba2b025ed36b4ac6309f122cc3cea14` with the other boards removed. See `UPSTREAM.md`.
