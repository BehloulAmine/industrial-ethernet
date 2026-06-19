cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(industrial_ethernet)

target_include_directories(app PRIVATE
  src/net
  src/protocols/modbus
)

target_sources(app PRIVATE
  src/core/main.c
  src/net/net_cfg.c
  src/shell/util_cmds.c
  src/protocols/modbus/app_modbus_tcp.c
)