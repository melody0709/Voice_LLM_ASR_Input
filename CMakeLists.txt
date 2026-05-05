cmake_minimum_required(VERSION 3.15)
project(VoiceLLMASRInput)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SHERPA_DIR "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx")
set(KNF_DIR "${CMAKE_SOURCE_DIR}/third_party/kaldi_native_fbank")

add_executable(VoiceLLMASRInput WIN32
    src/main.cpp
    src/engine.cpp
    src/hud.cpp
    src/hotkey.cpp
    src/settings.cpp
    src/resources.rc
)

add_compile_definitions(UNICODE _UNICODE)

set_target_properties(VoiceLLMASRInput PROPERTIES
    WIN32_EXECUTABLE YES
    LINK_FLAGS "/DELAYLOAD:sherpa-onnx-cxx-api.dll /DELAYLOAD:onnxruntime.dll /DELAYLOAD:kaldi-native-fbank-core.dll"
)

target_include_directories(VoiceLLMASRInput PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${SHERPA_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/include"
    "${KNF_DIR}/include"
)

target_link_directories(VoiceLLMASRInput PRIVATE
    "${SHERPA_DIR}/lib"
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime"
    "${KNF_DIR}/lib"
)

target_link_libraries(VoiceLLMASRInput
    sherpa-onnx-cxx-api
    onnxruntime
    kaldi-native-fbank-core
    user32
    gdi32
    shell32
    ole32
    comctl32
    d2d1
    dwrite
    shlwapi
    winmm
    winhttp
    crypt32
    delayimp
)
