# Upstream: xiaozhi-esp32

- Source: https://github.com/78/xiaozhi-esp32 (MIT License, see `LICENSE`)
- Imported at: `1b7644a6fba2b025ed36b4ac6309f122cc3cea14`

## Changes made on import
- Removed all boards except `main/boards/common`; added `main/boards/chitu`
- `main/Kconfig.projbuild`: board choice reduced to `BOARD_TYPE_CHITU`; options of removed boards deleted
- `main/CMakeLists.txt`: board selection reduced to Chitu; board-specific blocks removed;
  `esp_https_server` added for the phone console
- Project renamed `chitu` (version 1.0.0); Wi-Fi setup hotspot prefix `Chitu`
- Removed upstream CI, docker, release scripts, script tests, other-chip sdkconfig defaults
- `scripts/build.py`: only the OLED display choice is looked up (LCD choice removed)
- `main/ota.cc`: for Chitu the server time is stored as real UTC (the server's time zone
  offset is not added); Chitu applies the time zone of its configured city itself

## Pulling a fix from upstream later
```
git remote add upstream https://github.com/78/xiaozhi-esp32.git
git fetch upstream
git diff 1b7644a <new-commit> -- main/application.cc main/audio main/protocols > fix.patch
git apply --3way fix.patch   # skip parts touching removed boards
```
