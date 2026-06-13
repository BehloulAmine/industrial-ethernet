cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(h747_demo)

target_sources(app PRIVATE
  src/main.c
  src/net_cfg.c
  src/util_cmds.c
  src/app_modbus_tcp.c
)
