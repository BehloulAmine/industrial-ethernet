# Repository Guidelines

## Project Structure & Module Organization

This repository is a Zephyr firmware project for the STM32H747I-DISCO Cortex-M7.

- `app/` is the Zephyr application: `CMakeLists.txt`, `prj.conf`, and board overlay.
- `app/src/core/` contains startup and the heartbeat loop.
- `app/src/net/` owns DHCP/static IPv4 configuration and persistent network settings.
- `app/src/protocols/modbus/` contains the Modbus TCP server, Unit-ID 2 scanner window, and register map in `modbus_map.h`.
- `app/src/web/` contains the socket-based HTTP server, REST endpoints, embedded web assets, and logo.
- `app/src/shell/` contains UART shell commands.
- `patches/` contains environment-specific Zephyr patches; `tools/` contains host-side helpers.

Keep protocol-specific code in its module. Update `app/CMakeLists.txt` when adding a C source file, and update `modbus_map.h` whenever the Modbus register contract changes.

## Build, Flash, and Development Commands

Run commands from the repository root after activating the Zephyr environment.

```bash
west build -p always -b stm32h747i_disco/stm32h747xx/m7 app
cmake --build build
west flash --runner openocd
```

The first command performs a reliable build. Use `cmake --build build` for quick rebuilds when board, Kconfig, and devicetree files are unchanged. The flash command programs the board through ST-Link/OpenOCD. See `README.md` for setup and UART access.

## Coding Style & Naming Conventions

Use Zephyr C conventions: tabs for indentation, braces on the same line as control statements, and `snake_case` for functions and variables. Prefix application-facing APIs with `app_` (for example, `app_web_start`). Use uppercase `APP_MB_*` names for Modbus constants and keep comments short and technical. Prefer Zephyr APIs over custom platform wrappers.

No formatter or linter is configured. Preserve surrounding formatting and compile before submitting.

## Testing Guidelines

No automated test suite is configured. For each functional change, build the M7 image, flash it, and record manual validation: UART boot logs, `ping`, web UI/REST behavior, and Modbus reads/writes with `modpoll`. Keep behavior observable through logs or an existing interface.

## Commit & Pull Request Guidelines

Recent commits use phase-scoped imperative subjects, such as `Phase 4: Add REST API endpoints` and `Phase 3: Implement Modbus TCP scanner`. Follow `Phase N: concise action`; keep each commit focused.

Pull requests should describe the affected protocol or UI behavior, list build/manual tests, mention register-map or configuration compatibility changes, and include a browser screenshot for web UI changes.

## Configuration & Security

Do not commit local build output, credentials, or private network configuration. Treat mutable REST routes and Modbus register writes as control interfaces; preserve validation and bounds checks when extending them.
