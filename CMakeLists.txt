# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# The address of the Mac to dial and the shared secret live in a gitignored
# sdkconfig.defaults.local. ESP-IDF does not look for that name on its own, so
# it is named here. Without this, `idf.py build` produces firmware with an empty
# url and token: the device shows "connecting" for a moment and gives up, with
# nothing in any log to explain why. ESP-IDF appends the per-target file to each
# entry by itself, and the last entry wins.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.7.4")
project(xiaozhi)

if(NOT CONFIG_VOICEMODE_CODEX_VOICE OR NOT CONFIG_USE_DEFAULT_MESSAGE_STYLE)
    message(FATAL_ERROR "This workspace builds only Codex Voice with the orb. Use the separate backup for Classic firmware.")
endif()
