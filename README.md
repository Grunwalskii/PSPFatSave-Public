# PSPFatSave

A savestate plugin for the PSP-1000 ("Phat"), running under [ARK-4](https://github.com/PSP-Archive/ARK-4) CFW 6.61. It lets you freeze and restore a running game's full state (kernel + game RAM + VRAM) to the Memory Stick.

## Requisites

PSP-1000 ONLY (use one of the other PSPStates plugins instead on other models)
Tested only on ARK-4 6.61
No other CFW will ever be supported

## Instructions

Don't use Save or Load during a game's loading screens or videos. From the game's normal menu should be fine.

## Features

**Savestates**
- Multiple save slots per game
- Compress saves to save Memory Stick space (per-game toggle)
- Default slot setting (global)
- MB/s display during save can be used as a Memory Stick write-speed benchmark

**Live FPS Overlay** (optional, in-game only)
- FPS counter with three window modes: 1s / 0.5s / 0.2s
- 1% Low FPS (stutter metric — worst 1% of frames)
- Frametime histogram chart (visual frame-time distribution)
- CPU/GPU bottleneck highlighting (text + chart bar coloring)

**Performance Monitoring**
- CPU Usage % (idle-clocks based, same method as PSP-HUD)
- GPU Usage % (GE busy-duty-cycle estimate)
- Bottleneck indicator: highlights whichever of CPU/GPU is the current bottleneck

**Battery Status Overlay** (optional, in-game only)
- OFF / Percent / Percent+Time / ALL tiers
  - **Percent:** Battery % only
  - **Percent+Time:** % + estimated remaining discharge/charge time
  - **ALL:** Extra telemetry (voltage, current, temperature, capacity, cycle count)
- Charge state indicator (↑ charging / ↓ discharging arrow)

**Per-Game Settings**
- Intro Video Skip (OFF / learn mode / timed playback)
- Auto-Open on Boot (resume last save on game start)
- Frame Limit (25-60 FPS)

**System Settings** (global, all games)
- Overclock (PSP-1000 only; raw PLL registers, 0 = stock 333MHz)
- Debug Messages (UART logging + on-screen markers, debug build only)

## Why this exists

[PSPStates v2](https://github.com/PSP-Archive/PspStates-Experiment) (the classic savestate plugin, by Dark-Alex) only supports PSP Slim/Brite and later models, which have 64MB of RAM — double the Phat's 32MB. V2 saves state in one shot by copying the live lower 32MB into a spare, otherwise-unused upper 32MB bank while the firmware is suspended.

The Phat has no spare bank: just 8MB kernel + 24MB game RAM, all of it live. This plugin instead frees room by staging through the game's own RAM/VRAM (written out to the Memory Stick first):

- **Save**: freeze game threads -> compress and write game RAM + VRAM to the Memory Stick (freeing that RAM) -> suspend the firmware -> copy the live 8MB kernel into the now-free game RAM as staging -> resume -> write that kernel snapshot to the Memory Stick.
- **Load**: freeze -> read the saved kernel snapshot into game RAM as staging -> suspend -> overwrite the live kernel from staging -> resume -> restore game RAM + VRAM from the Memory Stick.

## Status

**Beta.** Please read before using:

- Can corrupt the Memory Stick (has not happened in testing so far).
- Can reset PSP system settings (happened once; since fixed).
- No guarantee that a save succeeds — there is a real chance of a crash and loss of any unsaved progress.
- No guarantee that a load succeeds — there is a real chance of a crash.

**When to save/load:**
- **Safe:** Gameplay only. Avoid saving during videos, transitions, or loading screens.
- **Risky:** Loading from game start, intro, or main menu can cause crashes or audio issues (missing soundtracks after load).

Save and load within the same game session works reliably, repeated any number of times. Cross-session load (restoring a save made in an earlier boot) is still occasionally unreliable — see open issues.

If a game doesn't work for you, please open an issue with at least: the plugin version, which game, what was happening in the game at the time, and the debug log if logging was enabled.

## Building

Requires:
- [PSPSDK](https://github.com/pspdev/pspsdk) toolchain on your `PATH` (`psp-gcc`, `psp-config`, etc.)
- [ARK-4](https://github.com/PSP-Archive/ARK-4) SDK files are included in this repo (`ARK-4/`); provides the kernel-mode `SystemCtrlForKernel` headers/lib.

```powershell
# standard build (debug mode: UART + MS logging, on-screen markers):
make

# release build (no debug logging/UART, smaller binary):
make DEBUG_BUILD=0
```

Produces `pspfatsave.prx`. Copy it to `ms0:/seplugins/` and enable it in your plugins config. (GAME only - "game, pspfatsave.prx, on")

## Credits

**Architecture & Design**
- Architecturally inspired by [PSPStates v2](https://github.com/PSP-Archive/PspStates-Experiment) (Dark-Alex), reversed by plum, for the freeze/suspend/resume approach — no code shared, only the design.

**Libraries & Assets**
- Bundles [FastLZ](http://fastlz.org/) (MIT licensed, see file headers in `src/plugin/fastlz_*.c`) for save compression.
- Font from [PSP-HUD](https://github.com/ErikPshat/PSP-HUD) by Erik Pshat, used in the on-screen overlay.

**Build & Dependencies**
- Built against [PSPSDK](https://github.com/pspdev/pspsdk) and [ARK-4](https://github.com/PSP-Archive/ARK-4).
- Includes minimal [ARK-4](https://github.com/PSP-Archive/ARK-4) SDK files (headers + `libpspsystemctrl_kernel.a`) for kernel-mode system control.

**References & Documentation**
- [ARK-4](https://github.com/PSP-Archive/ARK-4) — CFW SDK, kernel syscall hooks, system control documentation.
- [PSPSDK](https://github.com/pspdev/pspsdk) — PSP hardware APIs and headers.
- [uOFW (Unofficial OFW)](https://github.com/uofw/uofw) — Firmware reverse engineering, syscall stubs, GE/battery/power documentation.
- [PSP-HUD](https://github.com/ErikPshat/PSP-HUD) — Reference for CPU usage measurement via idle-clocks.
- PSP hardware datasheets and firmware internals documentation.

## License

MIT - see [LICENSE](LICENSE). FastLZ source files carry their own MIT header.

## Support

If you enjoy this project and want to support its development, you can buy me a coffee:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/W1L623I6WG)

## Disclaimer

This plugin is provided free of charge and as-is. The author accepts no
responsibility for any damage to your device or loss of data — including game save
data — that may result from using it. This is worth calling out explicitly here, given
what the plugin does.
Back up your NAND and anything important on the Memory Stick before use.

**AI-Generated Code:** This codebase was 100% written by AI (Claude). While care has been taken to validate against hardware references and test on real PSP-1000 hardware, the risk profile is higher than hand-written code. Use at your own risk.