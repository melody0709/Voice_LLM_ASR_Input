cmake_minimum_required(VERSION 3.15)
project(VoxType)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SHERPA_DIR "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx")
set(KNF_DIR "${CMAKE_SOURCE_DIR}/third_party/kaldi_native_fbank")
set(OPUS_DIR "${CMAKE_SOURCE_DIR}/third_party/opus")

add_executable(VoxType WIN32
    src/app/main.cpp
    src/audio/engine.cpp
    src/audio/batch_vad_trimmer.cpp
    src/audio/streaming_vad_trimmer.cpp
    src/audio/vad_trim_core.cpp
    src/asr/asr_session.cpp
    src/asr/asr_result.cpp
    src/asr/asr_dispatcher.cpp
    src/asr/asr_runtime_log.cpp
    src/asr/cloud_asr_common.cpp
    src/asr/cloud_http_common.cpp
    src/asr/doubao_ime_asr.cpp
    src/asr/doubao_ime_streaming_session.cpp
    src/asr/mimo_asr.cpp
    src/asr/qwen_asr.cpp
    src/asr/qwen_streaming_session.cpp
    src/asr/volcengine_net_diag.cpp
    src/asr/volcengine_streaming_session.cpp
    src/ui/hud.cpp
    src/ui/hotkey.cpp
    src/ui/settings.cpp
    src/audio/wasapi_capture.cpp
    src/app/resources.rc
)

add_compile_definitions(UNICODE _UNICODE)

set_target_properties(VoxType PROPERTIES
    WIN32_EXECUTABLE YES
    LINK_FLAGS "/DELAYLOAD:sherpa-onnx-cxx-api.dll /DELAYLOAD:onnxruntime.dll /DELAYLOAD:kaldi-native-fbank-core.dll"
)

target_include_directories(VoxType PRIVATE
    "${CMAKE_SOURCE_DIR}/src/app"
    "${CMAKE_SOURCE_DIR}/src/asr"
    "${CMAKE_SOURCE_DIR}/src/audio"
    "${CMAKE_SOURCE_DIR}/src/ui"
    "${CMAKE_SOURCE_DIR}/src/core"
    "${SHERPA_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/include"
    "${KNF_DIR}/include"
    "${OPUS_DIR}/include"
)

target_link_directories(VoxType PRIVATE
    "${SHERPA_DIR}/lib"
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime"
    "${KNF_DIR}/lib"
    "${OPUS_DIR}/lib"
)

target_link_libraries(VoxType
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
    ws2_32
    crypt32
    bcrypt
    opus
    mmdevapi
    imm32
    delayimp
)
