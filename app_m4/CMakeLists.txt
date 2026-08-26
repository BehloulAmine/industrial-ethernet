cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(industrial_ethernet_motor)

target_include_directories(app PRIVATE src)

target_sources(app PRIVATE
  src/main.c
  src/motor_buttons.c
  src/motor_control.c
  src/motor_potentiometer.c
)
