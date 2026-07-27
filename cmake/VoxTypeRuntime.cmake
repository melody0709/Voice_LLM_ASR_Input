# Canonical Runtime install rules. The build tree is never a supported runtime
# directory; build.bat installs this component into build/run/x64-release.

function(voxtype_install_runtime target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "voxtype_install_runtime: target '${target}' does not exist")
    endif()

    set(_src "${CMAKE_SOURCE_DIR}")
    set(_runtime_files
        "${_src}/dll/sherpa-onnx-cxx-api.dll"
        "${_src}/dll/sherpa-onnx-c-api.dll"
        "${_src}/dll/onnxruntime.dll"
        "${_src}/dll/kaldi-native-fbank-core.dll"
        "${_src}/models/silero_vad.int8.onnx"
        "${_src}/models/fireredvad_stream_vad_with_cache.onnx"
        "${_src}/dll/aria2c.exe"
        "${_src}/download_models.ps1"
        "${_src}/README.md"
        "${_src}/doc/volcengine_asr_guide_zh.md"
    )
    foreach(_required IN LISTS _runtime_files)
        if(NOT EXISTS "${_required}")
            message(FATAL_ERROR "Missing required VoxType runtime input: ${_required}")
        endif()
    endforeach()

    set(CMAKE_INSTALL_MESSAGE NEVER)

    # All mutable data belongs in LocalAppData (or in a Portable payload). The
    # development runtime may therefore be refreshed exactly on every install.
    install(CODE [=[
        set(_runtime_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
        file(REMOVE_RECURSE
            "${_runtime_root}/models"
            "${_runtime_root}/docs")
        file(REMOVE
            "${_runtime_root}/VoxType.exe"
            "${_runtime_root}/sherpa-onnx-cxx-api.dll"
            "${_runtime_root}/sherpa-onnx-c-api.dll"
            "${_runtime_root}/onnxruntime.dll"
            "${_runtime_root}/kaldi-native-fbank-core.dll"
            "${_runtime_root}/aria2c.exe"
            "${_runtime_root}/download_models.ps1"
            "${_runtime_root}/README.md"
            "${_runtime_root}/runtime-manifest.json")
    ]=] COMPONENT Runtime)

    install(TARGETS ${target}
        RUNTIME DESTINATION "."
        COMPONENT Runtime)
    install(FILES
        "${_src}/dll/sherpa-onnx-cxx-api.dll"
        "${_src}/dll/sherpa-onnx-c-api.dll"
        "${_src}/dll/onnxruntime.dll"
        "${_src}/dll/kaldi-native-fbank-core.dll"
        DESTINATION "."
        COMPONENT Runtime)
    install(PROGRAMS "${_src}/dll/aria2c.exe"
        DESTINATION "."
        COMPONENT Runtime)
    install(FILES "${_src}/download_models.ps1" "${_src}/README.md"
        DESTINATION "."
        COMPONENT Runtime)
    install(FILES
        "${_src}/models/silero_vad.int8.onnx"
        "${_src}/models/fireredvad_stream_vad_with_cache.onnx"
        DESTINATION "models"
        COMPONENT Runtime)
    install(FILES "${_src}/doc/volcengine_asr_guide_zh.md"
        DESTINATION "docs"
        COMPONENT Runtime)

    set(_manifest_script "${CMAKE_CURRENT_BINARY_DIR}/WriteRuntimeManifest.cmake")
    configure_file(
        "${_src}/cmake/WriteRuntimeManifest.cmake.in"
        "${_manifest_script}"
        @ONLY)
    install(SCRIPT "${_manifest_script}" COMPONENT Runtime)
endfunction()

