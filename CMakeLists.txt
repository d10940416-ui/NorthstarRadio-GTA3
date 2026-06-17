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
# safetyhook вЂ” clonРёСЂСѓРµС‚СЃСЏ CI Р°РІС‚РѕРјР°С‚РёС‡РµСЃРєРё
# ============================================================
add_subdirectory(deps/safetyhook)
target_link_libraries(NorthstarRadio PRIVATE safetyhook)

# ============================================================
# plugin-sdk GTA III вЂ” clonРёСЂСѓРµС‚СЃСЏ CI Р°РІС‚РѕРјР°С‚РёС‡РµСЃРєРё
# ============================================================
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

# plugin-sdk РІ СЃРѕРІСЂРµРјРµРЅРЅС‹С… РІРµСЂСЃРёСЏС… РІ РѕСЃРЅРѕРІРЅРѕРј header-only.
# Р•СЃР»Рё РїРѕС‚СЂРµР±СѓРµС‚СЃСЏ plugin.lib, РІРµСЂРЅРёС‚Рµ Р»РёРЅРєРѕРІРєСѓ РѕР±СЂР°С‚РЅРѕ.

# ============================================================
# BASS audio library
# Р¤Р°Р№Р»С‹ РЅСѓР¶РЅРѕ СЃРєР°С‡Р°С‚СЊ РІСЂСѓС‡РЅСѓСЋ СЃ https://www.un4seen.com/
# Рё РїРѕР»РѕР¶РёС‚СЊ РІ РїР°РїРєСѓ libs/bass/ РІР°С€РµРіРѕ СЂРµРїРѕР·РёС‚РѕСЂРёСЏ:
#   libs/bass/bass.h
#   libs/bass/bass.lib      (РїР°РїРєР° c/ РІРЅСѓС‚СЂРё Р°СЂС…РёРІР° BASS)
#   libs/bass/bass_fx.h
#   libs/bass/bass_fx.lib   (РїР°РїРєР° c/ РІРЅСѓС‚СЂРё Р°СЂС…РёРІР° bass_fx)
# ============================================================
target_include_directories(NorthstarRadio PRIVATE libs/bass)
target_link_libraries(NorthstarRadio PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/bass/bass.lib
    ${CMAKE_SOURCE_DIR}/libs/bass/bass_fx.lib
)

# ============================================================
# РЎРёСЃС‚РµРјРЅС‹Рµ Р±РёР±Р»РёРѕС‚РµРєРё Windows
# ============================================================
target_link_libraries(NorthstarRadio PRIVATE
    Xinput.lib
    kernel32.lib
    user32.lib
    winmm.lib
)

# ============================================================
# РџРµСЂРµРёРјРµРЅРѕРІР°С‚СЊ DLL в†’ .asi
# ============================================================
set_target_properties(NorthstarRadio PROPERTIES
    OUTPUT_NAME "NorthstarRadio"
    SUFFIX      ".asi"
    PREFIX      ""
)

# ============================================================
# Р¤Р»Р°РіРё РєРѕРјРїРёР»СЏС‚РѕСЂР° MSVC
# ============================================================
if(MSVC)
    target_compile_options(NorthstarRadio PRIVATE
        /W3
        /MP       # РїР°СЂР°Р»Р»РµР»СЊРЅР°СЏ СЃР±РѕСЂРєР°
        /EHsc
    )
    target_compile_definitions(NorthstarRadio PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WINVER=0x0501
        _WIN32_WINNT=0x0501
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    # РЎС‚Р°С‚РёС‡РЅР°СЏ CRT вЂ” СѓР±РёСЂР°РµС‚ Р·Р°РІРёСЃРёРјРѕСЃС‚СЊ РѕС‚ VCRUNTIME DLL
    set_property(TARGET NorthstarRadio
        PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
