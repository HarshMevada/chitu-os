# Chitu v1.0.0 – Stable Baseline

## What is in this firmware
Xiaozhi server + "Chitu" wake word + Xiaozhi's own OLED expressions + motor control +
saved Wi-Fi with automatic reconnection + install page with the version number. Nothing else.

## Rule from now on
One feature at a time: agree the feature → build only that → test on the robot →
re-check this list → bump the version → next feature only after the owner confirms.

## Baseline testing checklist
Tick each line on the real robot. Open **Logs & Console** on the install page (or any
serial monitor at 115200) to see the log lines quoted below.

| # | Test | How | Pass when |
|---|---|---|---|
| 1 | Powers on | Switch on / plug USB | OLED lights up, orange LED behaves as before |
| 2 | Starts without errors | Watch the log for 30 s | `Chitu board initialized`, `Motors ready and stopped`; no `E (` error lines, no reboot loop |
| 3 | Wi-Fi connects by itself | Power on near the router | Status bar shows the Wi-Fi icon, then standby, within ~15 s |
| 4 | Wi-Fi reconnects | Switch the router off 30 s, then on | Log `Wi-Fi lost (reason …)`; Chitu is back on Wi-Fi within ~1 min of the router returning, no restart needed |
| 5 | Xiaozhi server connects | After Wi-Fi | Screen shows standby; xiaozhi.me device page shows it online with version 1.0.0 |
| 6 | Wake word | Say "Chitu" from ~1 m, 10 times | Wakes at least 6 of 10 times; no wake-ups during 10 min of normal talk/TV |
| 7 | Mic and speaker | Ask "What is two plus two?" | Clear answer, loud enough, no crackle |
| 8 | Original expressions | Ask a happy and a sad question | OLED shows Xiaozhi's emotion icon and the chat text |
| 9 | Motors by voice | Lift the robot. Say "Chitu… move forward for one second", then backward, turn left, turn right | Each wheel turns the right way and stops by itself |
| 10 | Motors stay stopped | Power on / reset several times; also say "stop" mid-move | Wheels never twitch at start-up; "stop" stops at once |
| 11 | Install page opens | https://harshmevada.github.io/chitu-os/ in Chrome/Edge | Page loads, "Connect & install" works |
| 12 | Version shown | Page header; OLED right after restart | Page: `Chitu v1.0.0 – Stable Baseline`; OLED: `chitu/1.0.0` |
| 13 | Stable over time | Leave on 30 min, talk now and then | No freeze, no unexpected restart, replies start within 2–3 s |

If a motor turns the wrong way: set `MOTOR_LEFT_INVERT` / `MOTOR_RIGHT_INVERT` in
`main/boards/chitu/config.h` (that is a setting, not a new feature).

## Removed in this release (candidates to bring back, one at a time)
Phone web console · drawn Wall-E eyes, clock, QR · camera ("what do you see?") ·
distance sensor stop · listening auto-timeout · alarms · time / city / weather ·
pet mode / personality · dance / roam · voice effects · hardware test pages.
The previous full firmware is kept in git history (commit `9f6472e`).
