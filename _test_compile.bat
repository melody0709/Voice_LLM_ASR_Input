@echo off
set "VCToolsRoot=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
set "SDKRoot=C:\Program Files (x86)\Windows Kits\10"
for /f %%i in ('dir /b /ad "%SDKRoot%\Include\" 2^>nul') do set "SDKVer=%%i"

set "INCLUDE=%VCToolsRoot%\include;%SDKRoot%\Include\%SDKVer%\ucrt;%SDKRoot%\Include\%SDKVer%\um;%SDKRoot%\Include\%SDKVer%\shared"
set "LIB=%VCToolsRoot%\lib\x64;%SDKRoot%\Lib\%SDKVer%\ucrt\x64;%SDKRoot%\Lib\%SDKVer%\um\x64"
set "PATH=%VCToolsRoot%\bin\Hostx64\x64;%PATH%"

cd /d "%~dp0"
if not exist build mkdir build

set "SHERPA_INCLUDE=%~dp0third_party\sherpa-onnx\include"
set "SHERPA_LIB=%~dp0third_party\sherpa-onnx\lib"
set "ONNX_INCLUDE=%~dp0third_party\onnxruntime\include"
set "ONNX_LIB=%~dp0third_party\onnxruntime"
set "KNF_INCLUDE=%~dp0third_party\kaldi_native_fbank\include"
set "KNF_LIB=%~dp0third_party\kaldi_native_fbank\lib"

cl /nologo /O2 /EHsc /MT /std:c++17 /utf-8 /DUNICODE /D_UNICODE /Isrc /I"%SHERPA_INCLUDE%" /I"%ONNX_INCLUDE%" /I"%KNF_INCLUDE%" /Fobuild\ /Fe:build\VoxType.exe src\main.cpp src\engine.cpp src\hud.cpp src\hotkey.cpp src\settings.cpp src\wasapi_capture.cpp build\app.res user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib d2d1.lib dwrite.lib shlwapi.lib winmm.lib winhttp.lib crypt32.lib mmdevapi.lib imm32.lib delayimp.lib "%SHERPA_LIB%\sherpa-onnx-cxx-api.lib" "%ONNX_LIB%\onnxruntime.lib" "%KNF_LIB%\kaldi-native-fbank-core.lib" /link /DELAYLOAD:sherpa-onnx-cxx-api.dll /DELAYLOAD:onnxruntime.dll /DELAYLOAD:kaldi-native-fbank-core.dll
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILD SUCCESS
