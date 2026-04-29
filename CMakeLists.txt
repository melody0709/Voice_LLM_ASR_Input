cmake_minimum_required(VERSION 3.15)
project(VoiceLLMASRInput)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(VoiceLLMASRInput WIN32
    main.cpp
    resources.rc
)

add_compile_definitions(UNICODE _UNICODE)

set_target_properties(VoiceLLMASRInput PROPERTIES
    WIN32_EXECUTABLE YES
)

target_link_libraries(VoiceLLMASRInput
    user32
    gdi32
    shell32
    ole32
    comctl32
    shlwapi
    winmm
    ws2_32
)
