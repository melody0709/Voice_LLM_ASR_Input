cmake_minimum_required(VERSION 3.20)

project(VoxType LANGUAGES CXX RC)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(VOXTYPE_RESOURCE_HEADER "${CMAKE_SOURCE_DIR}/src/app/resource.h")
file(READ "${VOXTYPE_RESOURCE_HEADER}" _voxtype_resource_header)
foreach(_part IN ITEMS MAJOR MINOR PATCH)
    string(REGEX MATCH
        "#define[ \\t]+APP_VERSION_${_part}[ \\t]+([0-9]+)"
        _voxtype_version_match
        "${_voxtype_resource_header}")
    if(NOT DEFINED CMAKE_MATCH_1 OR "${CMAKE_MATCH_1}" STREQUAL "")
        message(FATAL_ERROR "Could not read APP_VERSION_${_part} from ${VOXTYPE_RESOURCE_HEADER}")
    endif()
    set(VOXTYPE_VERSION_${_part} "${CMAKE_MATCH_1}")
endforeach()
set(VOXTYPE_PRODUCT_VERSION
    "${VOXTYPE_VERSION_MAJOR}.${VOXTYPE_VERSION_MINOR}.${VOXTYPE_VERSION_PATCH}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VOXTYPE_RESOURCE_HEADER}")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/voxtype-version.txt"
    CONTENT "${VOXTYPE_PRODUCT_VERSION}\n")

set(SHERPA_DIR "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx")
set(KNF_DIR "${CMAKE_SOURCE_DIR}/third_party/kaldi_native_fbank")
set(OPUS_DIR "${CMAKE_SOURCE_DIR}/third_party/opus")

add_executable(VoxType WIN32
    src/app/main.cpp
    src/app/resources.rc
    src/audio/engine.cpp
    src/audio/batch_vad_trimmer.cpp
    src/audio/streaming_vad_trimmer.cpp
    src/audio/vad_trim_core.cpp
    src/audio/wasapi_capture.cpp
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
    src/asr/qwen_audio_http.cpp
    src/asr/qwen_audio_streaming.cpp
    src/asr/qwen_audio_streaming_session.cpp
    src/asr/qwen_streaming_session.cpp
    src/asr/winhttp_websocket_transport.cpp
    src/asr/qwen_free_streaming_session.cpp
    src/asr/qwen_free_proto_utdid.cpp
    src/asr/qwen_free_proto_asr.cpp
    src/asr/qwen_free_proto_llm.cpp
    src/asr/qwen_free_proto_unet.cpp
    src/asr/volcengine_net_diag.cpp
    src/asr/volcengine_streaming_session.cpp
    src/core/startup_registration.cpp
    src/ui/hud.cpp
    src/ui/hotkey.cpp
    src/ui/settings.cpp
)

target_compile_definitions(VoxType PRIVATE UNICODE _UNICODE)
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
target_link_libraries(VoxType PRIVATE
    sherpa-onnx-cxx-api
    onnxruntime
    kaldi-native-fbank-core
    user32
    gdi32
    shell32
    ole32
    oleaut32
    oleacc
    uiautomationcore
    comctl32
    d2d1
    dwrite
    shlwapi
    winmm
    winhttp
    ws2_32
    crypt32
    bcrypt
    mmdevapi
    imm32
    delayimp
    advapi32
    opus
)

if(MSVC)
    target_compile_options(VoxType PRIVATE /O2 /EHsc /utf-8)
    target_link_options(VoxType PRIVATE
        /MANIFEST:NO
        /DELAYLOAD:sherpa-onnx-cxx-api.dll
        /DELAYLOAD:onnxruntime.dll
        /DELAYLOAD:kaldi-native-fbank-core.dll
    )
endif()

include("${CMAKE_SOURCE_DIR}/cmake/VoxTypeRuntime.cmake")
voxtype_install_runtime(VoxType)

# Offline protocol regression target.  It deliberately links only the pure
# JSON/HMAC pieces, writes outside the runtime payload, and is built only when
# explicitly requested by build.bat --test. The generic HMAC implementation
# is not linked into the production VoxType executable.
add_executable(qwen_free_protocol_test
    tests/qwen_free_protocol_test.cpp
    src/asr/qwen_free_proto_sign.cpp
)
target_include_directories(qwen_free_protocol_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/asr"
)
if(MSVC)
    target_compile_options(qwen_free_protocol_test PRIVATE /O2 /EHsc /utf-8)
endif()
set_target_properties(qwen_free_protocol_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../../artifacts/tests"
)
add_test(NAME qwen_free_protocol_test COMMAND qwen_free_protocol_test)

add_executable(qwen_audio_json_test
    tests/qwen_audio_json_test.cpp
    src/asr/qwen_asr.cpp
    src/asr/qwen_audio_http.cpp
    src/asr/qwen_audio_streaming.cpp
    src/asr/winhttp_websocket_transport.cpp
    src/asr/cloud_asr_common.cpp
    src/asr/cloud_http_common.cpp
)
target_compile_definitions(qwen_audio_json_test PRIVATE VOXTYPE_QWEN_AUDIO_PROTOCOL_TEST=1)
target_include_directories(qwen_audio_json_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/asr"
    "${CMAKE_SOURCE_DIR}/src/core"
)
if(MSVC)
    target_compile_options(qwen_audio_json_test PRIVATE /O2 /EHsc /utf-8)
endif()
target_link_libraries(qwen_audio_json_test PRIVATE winhttp crypt32 bcrypt ole32 ws2_32)
set_target_properties(qwen_audio_json_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../../artifacts/tests"
)
add_test(NAME qwen_audio_json_test COMMAND qwen_audio_json_test)
