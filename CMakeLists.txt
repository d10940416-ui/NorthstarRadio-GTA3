cmake_minimum_required(VERSION 3.20)
project(NorthstarRadio CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Source files
# ============================================================
add_library(NorthstarRadio SHARED
    src/Main.cpp
    src/Switch.cpp
    src/AmbientCar.cpp
    src/RadioVehicles.cpp
)

# ============================================================
# safetyhook — clonируется CI автоматически
# ============================================================
add_subdirectory(deps/safetyhook)
target_link_libraries(NorthstarRadio PRIVATE safetyhook)

# ============================================================
# plugin-sdk GTA III — clonируется CI автоматически
# ============================================================
target_include_directories(NorthstarRadio PRIVATE
    deps/plugin-sdk/plugin_III
    deps/plugin-sdk/plugin_III/rw
    deps/plugin-sdk/plugin_s
    deps/plugin-sdk/shared
    deps/plugin-sdk/shared/audio
    deps/plugin-sdk/injector
)

# Добавляем путь к скомпилированной plugin.lib из plugin-sdk
# CI строит её на шаге "Build plugin-sdk lib"
link_directories(${CMAKE_SOURCE_DIR}/deps/plugin-sdk/output/gta3/Release)
target_link_libraries(NorthstarRadio PRIVATE plugin)

# ============================================================
# BASS audio library
# Файлы нужно скачать вручную с https://www.un4seen.com/
# и положить в папку libs/bass/ вашего репозитория:
#   libs/bass/bass.h
#   libs/bass/bass.lib      (папка c/ внутри архива BASS)
#   libs/bass/bass_fx.h
#   libs/bass/bass_fx.lib   (папка c/ внутри архива bass_fx)
# ============================================================
target_include_directories(NorthstarRadio PRIVATE libs/bass)
target_link_libraries(NorthstarRadio PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/bass/bass.lib
    ${CMAKE_SOURCE_DIR}/libs/bass/bass_fx.lib
)

# ============================================================
# Системные библиотеки Windows
# ============================================================
target_link_libraries(NorthstarRadio PRIVATE
    Xinput.lib
    kernel32.lib
    user32.lib
    winmm.lib
)

# ============================================================
# Переименовать DLL → .asi
# ============================================================
set_target_properties(NorthstarRadio PROPERTIES
    OUTPUT_NAME "NorthstarRadio"
    SUFFIX      ".asi"
    PREFIX      ""
)

# ============================================================
# Флаги компилятора MSVC
# ============================================================
if(MSVC)
    target_compile_options(NorthstarRadio PRIVATE
        /W3
        /MP       # параллельная сборка
        /EHsc
    )
    target_compile_definitions(NorthstarRadio PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WINVER=0x0501
        _WIN32_WINNT=0x0501
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    # Статичная CRT — убирает зависимость от VCRUNTIME DLL
    set_property(TARGET NorthstarRadio
        PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
