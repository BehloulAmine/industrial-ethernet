cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(industrial_ethernet)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_shared
  src/core
  src/ipc
  src/net
  src/protocols/dpws
  src/protocols/eip
  src/protocols/modbus
  src/ui
  src/web
)

target_sources(app PRIVATE
  src/core/main.c
  src/core/ident.c
  src/ipc/app_ipc.c
  src/net/net_cfg.c
  src/shell/util_cmds.c
  src/protocols/dpws/app_dpws.c
  src/protocols/modbus/app_modbus_tcp.c
  src/protocols/modbus/app_modbus_scanner.c
  src/ui/app_lcd.c
  src/web/app_web.c
)

add_subdirectory(src/protocols/eip)
