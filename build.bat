@echo off
setlocal

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo Visual Studio Build Tools not found: %VCVARS%
    exit /b 1
)

call "%VCVARS%"
cd /d "%~dp0"
if not exist build mkdir build

set "SHERPA_INCLUDE=%~dp0third_party\sherpa-onnx\include"
set "SHERPA_LIB=%~dp0third_party\sherpa-onnx\lib"
set "ONNX_INCLUDE=%~dp0third_party\onnxruntime\include"
set "ONNX_LIB=%~dp0third_party\onnxruntime"
set "KNF_INCLUDE=%~dp0third_party\kaldi_native_fbank\include"
set "KNF_LIB=%~dp0third_party\kaldi_native_fbank\lib"
set "OPUS_INCLUDE=%~dp0third_party\opus\include"
set "OPUS_LIB=%~dp0third_party\opus\lib"

rc /nologo /fo build\app.res src\app\resources.rc
if errorlevel 1 exit /b 1

cl /nologo /O2 /EHsc /MT /std:c++17 /utf-8 /DUNICODE /D_UNICODE /Isrc\app /Isrc\asr /Isrc\audio /Isrc\ui /Isrc\core /I"%SHERPA_INCLUDE%" /I"%ONNX_INCLUDE%" /I"%KNF_INCLUDE%" /I"%OPUS_INCLUDE%" /Fobuild\ /Fe:build\VoxType.exe src\app\main.cpp src\audio\engine.cpp src\audio\batch_vad_trimmer.cpp src\audio\streaming_vad_trimmer.cpp src\audio\vad_trim_core.cpp src\asr\asr_session.cpp src\asr\asr_result.cpp src\asr\asr_dispatcher.cpp src\asr\asr_runtime_log.cpp src\asr\cloud_asr_common.cpp src\asr\cloud_http_common.cpp src\asr\doubao_ime_asr.cpp src\asr\doubao_ime_streaming_session.cpp src\asr\mimo_asr.cpp src\asr\qwen_asr.cpp src\asr\qwen_streaming_session.cpp src\asr\volcengine_streaming_session.cpp src\asr\volcengine_net_diag.cpp src\ui\hud.cpp src\ui\hotkey.cpp src\ui\settings.cpp src\audio\wasapi_capture.cpp build\app.res user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib d2d1.lib dwrite.lib shlwapi.lib winmm.lib winhttp.lib ws2_32.lib crypt32.lib bcrypt.lib mmdevapi.lib imm32.lib delayimp.lib "%OPUS_LIB%\opus.lib" "%SHERPA_LIB%\sherpa-onnx-cxx-api.lib" "%ONNX_LIB%\onnxruntime.lib" "%KNF_LIB%\kaldi-native-fbank-core.lib" /link /DELAYLOAD:sherpa-onnx-cxx-api.dll /DELAYLOAD:onnxruntime.dll /DELAYLOAD:kaldi-native-fbank-core.dll
if errorlevel 1 exit /b 1

copy /y "%~dp0dll\sherpa-onnx-cxx-api.dll" build\ >nul
copy /y "%~dp0dll\sherpa-onnx-c-api.dll" build\ >nul
copy /y "%~dp0dll\onnxruntime.dll" build\ >nul
copy /y "%~dp0dll\kaldi-native-fbank-core.dll" build\ >nul

echo Build Success: build\VoxType.exe
