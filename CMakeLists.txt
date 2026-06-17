cmake_minimum_required(VERSION 3.20)
project(NorthstarRadio CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
add_library(NorthstarRadio SHARED
    src/Main.cpp
    src/Switch.cpp
    src/AmbientCar.cpp
    src/RadioVehicles.cpp
)

# safetyhook
add_subdirectory(deps/safetyhook)
target_link_libraries(NorthstarRadio PRIVATE safetyhook)

# plugin-sdk include paths
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
)

# BASS
target_include_directories(NorthstarRadio PRIVATE libs/bass)
target_link_libraries(NorthstarRadio PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/bass/bass.lib
    ${CMAKE_SOURCE_DIR}/libs/bass/bass_fx.lib
)

# Windows system libs
target_link_libraries(NorthstarRadio PRIVATE
    Xinput.lib
    kernel32.lib
    user32.lib
    winmm.lib
)

# Output as .asi
set_target_properties(NorthstarRadio PROPERTIES
    OUTPUT_NAME "NorthstarRadio"
    SUFFIX      ".asi"
    PREFIX      ""
)

if(MSVC)
    target_compile_options(NorthstarRadio PRIVATE
        /W3
        /MP
        /EHsc
    )
    target_compile_definitions(NorthstarRadio PRIVATE
        GTAIII                    # FIX 1: tells plugin-sdk which game we target
        _CRT_SECURE_NO_WARNINGS
        WINVER=0x0501
        _WIN32_WINNT=0x0501
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    set_property(TARGET NorthstarRadio
        PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
