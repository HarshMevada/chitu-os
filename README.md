# Chitu OS

Firmware for **Chitu**, a little AI desk robot (Seeed XIAO ESP32-S3 Sense, SH1106 OLED,
MAX98357 speaker, TB6612 motor driver, TOF10120 distance sensor).

**Install from the browser:** https://harshmevada.github.io/chitu-os/ (Chrome or Edge on a computer)

## Features
- AI voice chat (xiaozhi.me), wake word chosen on the install page
- Animated drawn eyes, 20+ expressions with motion (bounce, shake, hearts, sleep)
- Vision: "what do you see?" sends a camera photo to the AI
- Driving by voice, dancing, auto-roam with obstacle stop
- Phone console `https://<robot-ip>/`: live camera, drive pad, hold-to-talk with the phone
  mic, Chitu's voice on the phone, room listening, text + emoji messages on the screen
- Pet mode: looks around, pulls faces, chirps, wiggles, naps, speaks up now and then
- Wi-Fi auto-retry, hardware test sketches (`tests/`)

## Layout
| Path | What |
|---|---|
| `main/boards/chitu/` | Everything Chitu-specific: board, eyes, motors, sensor, camera, console, pet mode. Settings in `config.h` |
| `main/` | Voice-assistant core (from xiaozhi-esp32): app state machine, audio, protocols, MCP tools, display |
| `site/` | Install page (GitHub Pages) |
| `tests/` | Arduino sketches that test one part at a time |
| `.github/workflows/build.yml` | Cloud build: one firmware per wake word, then publishes the install page |

## Build locally
ESP-IDF 6.1, then:
```
python main/boards/chitu/tools/make_cert.py main/boards/chitu/chitu_cert.h
python scripts/build.py chitu --wake-word wn9_hiesp
```

## Credits
Chitu OS is based on [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (MIT License,
Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.), imported at commit
`1b7644a6fba2b025ed36b4ac6309f122cc3cea14` with the other boards removed. See `UPSTREAM.md`.
Emoji glyphs: Noto Emoji (SIL Open Font License 1.1).
