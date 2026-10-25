cmake_minimum_required(VERSION 3.16)
project(MintDesk VERSION 0.8.1 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "MintDesk builds for Windows only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Select a Windows x64 toolchain.")
endif()

add_executable(MintDesk WIN32
    src/main.cpp
    src/state.cpp
    src/shell_items.cpp
    src/snap.cpp
    src/desktop_menu.cpp
    src/dragdrop.cpp
    src/alpha_surface.cpp
    src/desktop_items.cpp
    src/hidden_items.cpp
    src/startup_integration.cpp
    src/glass_backdrop.cpp
    src/file_helper.cpp
    src/app.rc
)
target_compile_features(MintDesk PRIVATE cxx_std_17)
target_compile_definitions(MintDesk PRIVATE
    UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00
)
target_include_directories(MintDesk PRIVATE assets)
target_link_libraries(MintDesk PRIVATE
    comctl32 shell32 ole32 uuid shlwapi advapi32 dwmapi gdi32 user32 comdlg32 msimg32
)

if(MSVC)
    set_property(TARGET MintDesk PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(MintDesk PRIVATE /W4 /permissive- /utf-8 $<$<CONFIG:Release>:/O1>)
    target_link_options(MintDesk PRIVATE /MANIFEST:NO /DYNAMICBASE /NXCOMPAT)
else()
    target_compile_options(MintDesk PRIVATE -Wall -Wextra -Wpedantic -fno-ident -ffunction-sections -fdata-sections $<$<CONFIG:Release>:-Os>)
    target_link_options(MintDesk PRIVATE -municode -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--dynamicbase -Wl,--nxcompat $<$<CONFIG:Release>:-s>)
endif()

set_target_properties(MintDesk PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")
install(TARGETS MintDesk RUNTIME DESTINATION .)
