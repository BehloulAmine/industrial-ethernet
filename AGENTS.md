# Repository Guidelines

## Project Structure & Module Organization

This repository is a Zephyr 4.4 firmware project for the STM32H747I-DISCO
Cortex-M7. Phases 1 through 6 are implemented; Phase 7 (WS-Discovery) is next.

- `app/` is the Zephyr application, configuration, and board overlay.
- `app/src/core/` contains startup and the heartbeat loop.
- `app/src/net/` owns DHCP/static IPv4 configuration and persistent network settings.
- `app/src/protocols/modbus/` contains Modbus TCP, the Unit-ID 2 scanner window,
  and the 50-register contract in `modbus_map.h`.
- `app/src/protocols/eip/` contains the Zephyr port and application glue for
  OpENer; upstream sources are the `app/third_party/opener/` Git submodule.
- `app/src/web/` contains the socket-based HTTP server, REST endpoints, embedded web assets, and logo.
- `app/src/ui/` contains the optional M7 LVGL touchscreen dashboard.
- `app/src/shell/` contains UART shell commands.
- `tools/eip_probe.py` validates explicit EtherNet/IP messaging.

Keep protocol code inside its module. Update `app/CMakeLists.txt` for new sources
and `modbus_map.h` whenever the public register contract changes.

## Current Architecture and Resume Context

- Modbus TCP listens on port 502. Unit-ID 1 exposes 50 holding registers;
  Unit-ID 2 exposes 10 runtime-mapped scanner words. `REG40..REG49` configure
  the scanner mapping; its default targets are `REG10..REG19`.
- The web UI uses a REST API on port 80. Keep its `refreshInFlight` and editing
  guards so polling cannot overwrite user input or overlap requests.
- OpENer uses TCP/UDP 44818 and UDP 2222. Assembly 100 is 20-byte Output
  (O-to-T), Assembly 101 is 20-byte Input (T-to-O), and Assembly 1 is empty
  configuration. EIP, Modbus scanner, and REST share the same ten words.
- LCD/LVGL runs on M7 using the MB1166 shield, starts asleep, wakes on touch,
  and owns all LVGL objects from its dedicated thread.
- Keep `CONFIG_NET_MAX_CONN=16`. Eight contexts were insufficient once a
  Class 1 EIP connection and an idle Modbus client coexisted; TCP listeners then
  refused web connections. Do not reintroduce EIP priority/yield or socket
  ownership workarounds for that resolved issue.

## Build, Flash, and Development Commands

After cloning, initialize OpENer and the Zephyr workspace:

```bash
git submodule update --init --recursive
west init -l .
west update
west build -p always -b stm32h747i_disco/stm32h747xx/m7 app
west flash --runner openocd
```

Build the LCD image with:

```bash
west build -p always -b stm32h747i_disco/stm32h747xx/m7 app -- \
  -DSHIELD=st_b_lcd40_dsi1_mb1166 -DEXTRA_CONF_FILE=lcd.conf
```

Add `diagnostics.conf` to `EXTRA_CONF_FILE` only while using UART commands
`net conn`, `net mem`, and `net stats`. Use `cmake --build build` for quick C
rebuilds. See `README.md` for Windows/WSL setup and panel revision details.

## Coding Style & Naming Conventions

Use Zephyr C conventions: tabs for indentation, braces on the same line as control statements, and `snake_case` for functions and variables. Prefix application-facing APIs with `app_` (for example, `app_web_start`). Use uppercase `APP_MB_*` names for Modbus constants and keep comments short and technical. Prefer Zephyr APIs over custom platform wrappers.

No formatter or linter is configured. Preserve surrounding formatting and compile before submitting.

## Testing Guidelines

No automated target suite exists. Build and flash the M7 image, then validate
UART boot logs, `ping`, concurrent web and Modbus operation, scanner reads and
writes, LCD wake/sleep, and `python tools/eip_probe.py <board-ip>`. For Class 1
EIP, test assemblies 100/101 while web and an idle Modbus connection remain
usable.

## Commit & Pull Request Guidelines

Use phase-scoped imperative commits, for example `Phase 6: Fix EIP connection
capacity`. Keep commits focused and commit the OpENer gitlink, not vendored
source files.

Pull requests should describe the affected protocol or UI behavior, list build/manual tests, mention register-map or configuration compatibility changes, and include a browser screenshot for web UI changes.

## Configuration & Security

Do not commit local build output, credentials, or private network configuration. Treat mutable REST routes and Modbus register writes as control interfaces; preserve validation and bounds checks when extending them.
