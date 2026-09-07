cmake_minimum_required(VERSION 3.16)
project(apollo_watch_host C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SIM_DIR "/Users/thiernodiallo/Coding/tools/lvgl-mcp/simulator")
set(FW_DIR "/Users/thiernodiallo/Coding/Hardware/apollo-firmware")

set(LV_BUILD_CONF_PATH "${SIM_DIR}/lv_conf.h" CACHE PATH "Path to lv_conf.h" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)

add_subdirectory("${SIM_DIR}/lib/lvgl" "${CMAKE_BINARY_DIR}/lvgl_build")

add_executable(watch_ui_test
    test_host_main.cc
    "${FW_DIR}/main/display/watch_ui.cc"
    "${FW_DIR}/main/display/watch_icons.cc"
    "${SIM_DIR}/hal/display_driver.c"
    "${SIM_DIR}/export/screenshot.c"
)

target_include_directories(watch_ui_test PRIVATE
    "${SIM_DIR}"
    "${SIM_DIR}/hal"
    "${SIM_DIR}/export"
    "${SIM_DIR}/lib/stb"
    "${SIM_DIR}/lib/lvgl/src"
    "${FW_DIR}/main/display"
)

target_link_libraries(watch_ui_test PRIVATE lvgl m)

set_target_properties(watch_ui_test PROPERTIES
    CXX_STANDARD 17
    C_STANDARD 99
)
