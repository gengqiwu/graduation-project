# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components/Middlewares)
add_compile_options(-fdiagnostics-color=always)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(04_duihua1)

# 绕过 GCC 14.2 在 esp_lcd_panel_rgb.c 中的 ICE (internal compiler error) 问题
# 对该文件单独关闭 -Og 优化，改用 -O0
set_source_files_properties(
    $ENV{IDF_PATH}/components/esp_lcd/rgb/esp_lcd_panel_rgb.c
    PROPERTIES COMPILE_FLAGS "-O0"
)
