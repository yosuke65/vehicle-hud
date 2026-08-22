cmake_minimum_required(VERSION 3.22.1)
project("vehiclehud")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(vehiclehud SHARED
        native_bridge.cpp
        render_thread.cpp
        telemetry_source.cpp
        vulkan_native_bridge.cpp
        vulkan_renderer.cpp)

find_library(log-lib log)
find_library(egl-lib EGL)
find_library(glesv2-lib GLESv2)
find_library(android-lib android)
find_library(vulkan-lib vulkan)

target_link_libraries(vehiclehud
        ${log-lib}
        ${egl-lib}
        ${glesv2-lib}
        ${android-lib}
        ${vulkan-lib})
