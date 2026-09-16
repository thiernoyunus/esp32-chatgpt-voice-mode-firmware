cmake_minimum_required(VERSION 3.16)
project(voicemode_watch_host C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The firmware root is three levels up from scripts/tests/watch_ui_host.
get_filename_component(FW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# The LVGL simulator is an external checkout, so it has to be pointed at.
set(SIM_DIR "$ENV{VOICEMODE_LVGL_SIM_DIR}" CACHE PATH "LVGL simulator checkout")
if(NOT SIM_DIR OR NOT EXISTS "${SIM_DIR}/lv_conf.h")
    message(FATAL_ERROR
        "Set VOICEMODE_LVGL_SIM_DIR (or -DSIM_DIR=) to an LVGL simulator checkout "
        "containing lv_conf.h. Got: '${SIM_DIR}'")
endif()

set(LV_BUILD_CONF_PATH "${SIM_DIR}/lv_conf.h" CACHE PATH "Path to lv_conf.h" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)

add_subdirectory("${SIM_DIR}/lib/lvgl" "${CMAKE_BINARY_DIR}/lvgl_build")

add_executable(watch_ui_test
    test_host_main.cc
    "${FW_DIR}/main/display/watch_ui.cc"
    "${FW_DIR}/main/display/watch_icons.cc"
    "${FW_DIR}/main/display/bloub/bloub_face.c"
    "${FW_DIR}/main/display/bloub/bloub_shapes.c"
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
