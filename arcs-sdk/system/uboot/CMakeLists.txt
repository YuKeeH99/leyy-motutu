cmake_minimum_required(VERSION 3.19)
set(CMAKE_SYSTEM_NAME Generic)
project(boot NONE)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/boot_standalone.cmake)
