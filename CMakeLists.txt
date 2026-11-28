cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME SmartEdge_F407)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# LVGL v9.6.0 第三方图形库（仅编译纯 C 核心源码，配置由 Middlewares/Third_Party/lvgl/lv_conf.h 裁剪）
set(LVGL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/lvgl")
file(GLOB_RECURSE LVGL_C_SOURCES CONFIGURE_DEPENDS "${LVGL_ROOT}/src/*.c")

add_library(lvgl_lib OBJECT ${LVGL_C_SOURCES})
target_include_directories(lvgl_lib PUBLIC
    "${LVGL_ROOT}"
    "${LVGL_ROOT}/include"
    "${LVGL_ROOT}/include/lvgl"
    "${CMAKE_CURRENT_SOURCE_DIR}/App/Inc"
)
target_compile_definitions(lvgl_lib PUBLIC
    LV_CONF_INCLUDE_SIMPLE
    LV_KCONFIG_IGNORE
    LV_LVGL_H_INCLUDE_SIMPLE
)
# 对第三方 LVGL 库启用 -Os 体积与栈帧优化，大幅降低 Debug (-O0) 模式下 HmiTask 栈溢出风险并提升刷屏帧率
target_compile_options(lvgl_lib PRIVATE -Os)
target_link_libraries(lvgl_lib PUBLIC stm32cubemx)

# 自研源码清单（按 M1-M5 阶段逐步追加，并对自研代码严格启用 -Wall -Wextra）
set(USER_SOURCES
    BSP/Src/bsp_sys.c
    BSP/Src/bsp_uart_cli.c
    BSP/Src/bsp_flash.c
    BSP/Src/bsp_st7789.c
    BSP/Src/bsp_xpt2046.c
    BSP/Src/bsp_rs485.c
    BSP/Src/bsp_can.c
    App/Src/app_cfg.c
    App/Src/app_data.c
    App/Src/app_modbus_rtu.c
    App/Src/app_can_proto.c
    App/Src/app_monitor.c
    App/Src/app_cli.c
    App/Src/app_hmi.c
    App/Src/app_sensor.c
    App/Src/app_fansim.c
    App/Src/app_control.c
    App/Src/ui_main.c
    App/Src/ui_page_overview.c
    App/Src/ui_page_trend.c
    App/Src/ui_page_diag.c
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${USER_SOURCES}
)

# 仅对自研 C 文件开启更严格的 -Wextra 检查，确保零警告标准
set_source_files_properties(${USER_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra"
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    BSP/Inc
    App/Inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    lvgl_lib

    # Add user defined libraries
)
