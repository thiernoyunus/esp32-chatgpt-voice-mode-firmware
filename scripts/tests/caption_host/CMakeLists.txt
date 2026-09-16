cmake_minimum_required(VERSION 3.16)
project(voicemode_caption_host C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

get_filename_component(FW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(SIM_DIR "$ENV{VOICEMODE_LVGL_SIM_DIR}" CACHE PATH "LVGL simulator checkout")
if(NOT SIM_DIR OR NOT EXISTS "${SIM_DIR}/lv_conf.h")
    message(FATAL_ERROR "Set VOICEMODE_LVGL_SIM_DIR to an LVGL simulator checkout. Got: '${SIM_DIR}'")
endif()

set(LV_BUILD_CONF_PATH "${SIM_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SIM_DIR}/lib/lvgl" "${CMAKE_BINARY_DIR}/lvgl_build")

add_executable(caption_host
    caption_host_main.cc
    "${FW_DIR}/main/display/bloub/bloub_face.c"
    "${FW_DIR}/main/display/bloub/bloub_shapes.c"
    "${SIM_DIR}/hal/display_driver.c"
    "${SIM_DIR}/export/screenshot.c"
)
target_include_directories(caption_host PRIVATE
    "${SIM_DIR}" "${SIM_DIR}/hal" "${SIM_DIR}/export" "${SIM_DIR}/lib/stb"
    "${SIM_DIR}/lib/lvgl/src" "${FW_DIR}/main/display"
)
target_link_libraries(caption_host PRIVATE lvgl m)
