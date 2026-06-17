cmake_minimum_required(VERSION 3.20)
project(NorthstarRadio CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(NorthstarRadio SHARED
    src/Main.cpp
    src/Switch.cpp
    src/AmbientCar.cpp
    src/RadioVehicles.cpp
)

add_subdirectory(deps/safetyhook)
target_link_libraries(NorthstarRadio PRIVATE safetyhook)

target_include_directories(NorthstarRadio PRIVATE
    deps/plugin-sdk
    deps/plugin-sdk/plugin_III
    deps/plugin-sdk/plugin_III/game_III
    deps/plugin-sdk/plugin_III/game_III/meta
    deps/plugin-sdk/plugin_III/rw
    deps/plugin-sdk/plugin_s
    deps/plugin-sdk/shared
    deps/plugin-sdk/shared/audio
    deps/plugin-sdk/injector
    libs/bass
)

target_link_libraries(NorthstarRadio PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/bass/bass.lib
    ${CMAKE_SOURCE_DIR}/libs/bass/bass_fx.lib
    Xinput.lib
    kernel32.lib
    user32.lib
    winmm.lib
)

set_target_properties(NorthstarRadio PROPERTIES
    OUTPUT_NAME "NorthstarRadio"
    SUFFIX      ".asi"
    PREFIX      ""
)

if(MSVC)
    target_compile_options(NorthstarRadio PRIVATE /W3 /MP /EHsc)
    set_property(TARGET NorthstarRadio
        PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
