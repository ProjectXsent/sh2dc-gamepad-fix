cmake_minimum_required(VERSION 3.20)
project(sh2dc_gamepad_fix CXX)

# Configure with:  cmake -S . -B build -A Win32 -DSDL3_DIR=<path-to-32bit-SDL3>/cmake
#   cmake --build build --config Release
# Output in build/Release: dinput8.dll, SDL3.dll and sh2input.ini.
# Copy all three next to sh2pc.exe (add gamecontrollerdb.txt too if you use it).

# Link the MSVC runtime statically so the DLL also loads on Windows 7
# without a Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(SDL3 REQUIRED CONFIG)

add_library(dinput8 SHARED dllmain.cpp)
set_target_properties(dinput8 PROPERTIES PREFIX "" CXX_STANDARD 17)
target_compile_definitions(dinput8 PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)

# Prefer the shared SDL3 so SDL3.dll can be copied next to our DLL.
if(TARGET SDL3::SDL3-shared)
    set(SH2_SDL_TARGET SDL3::SDL3-shared)
else()
    set(SH2_SDL_TARGET SDL3::SDL3)
endif()
target_link_libraries(dinput8 PRIVATE ${SH2_SDL_TARGET} dxguid)

# Put sh2input.ini next to the built DLL.
add_custom_command(TARGET dinput8 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/sh2input.ini"
            "$<TARGET_FILE_DIR:dinput8>/sh2input.ini"
    VERBATIM)

# Put SDL3.dll next to the built DLL.
if(TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET dinput8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3-shared>"
                "$<TARGET_FILE_DIR:dinput8>"
        VERBATIM)
else()
    message(WARNING "SDL3::SDL3-shared not found: SDL3.dll will not be copied automatically.")
endif()