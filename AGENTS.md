# Notes for coding agents

- Chitu OS targets one board: `main/boards/chitu` (ESP32-S3, IDF 6.1). Chitu code lives there.
- Builds use `-fno-exceptions` and `-Werror`: return errors (see `ErrorJson` in motor_controller.cc), never `throw`.
- Stable Baseline rule: add ONE feature at a time, only after the owner confirms the previous one on the robot. Bump `PROJECT_VER` in `CMakeLists.txt` with each feature.
- Core (`main/` outside boards) comes from xiaozhi-esp32; keep changes there small and list them in `UPSTREAM.md`.
- CI: `.github/workflows/build.yml` builds the firmware (wake word `micro:chitu`) on every push; `main` is published to GitHub Pages.
