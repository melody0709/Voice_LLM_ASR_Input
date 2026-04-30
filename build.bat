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

set "SHERPA_INCLUDE=%~dp0runtime\Lib\site-packages\sherpa_onnx\include"
set "SHERPA_LIB=%~dp0runtime\Lib\site-packages\sherpa_onnx\lib"
set "ONNX_INCLUDE=%~dp0third_party\onnxruntime\include"
set "ONNX_LIB=%~dp0third_party\onnxruntime"
set "KNF_INCLUDE=%~dp0third_party\kaldi_native_fbank\include"
set "KNF_LIB=%~dp0third_party\kaldi_native_fbank\lib"

rc /nologo /fo build\app.res resources.rc
if errorlevel 1 exit /b 1

cl /nologo /O2 /EHsc /MT /std:c++17 /utf-8 /DUNICODE /D_UNICODE /I"%SHERPA_INCLUDE%" /I"%ONNX_INCLUDE%" /I"%KNF_INCLUDE%" /Fobuild\ /Fe:build\VoiceLLMASRInput.exe main.cpp build\app.res user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib d2d1.lib dwrite.lib shlwapi.lib winmm.lib "%SHERPA_LIB%\sherpa-onnx-cxx-api.lib" "%ONNX_LIB%\onnxruntime.lib" "%KNF_LIB%\kaldi-native-fbank-core.lib"
if errorlevel 1 exit /b 1

copy /y "%SHERPA_LIB%\sherpa-onnx-cxx-api.dll" build\ >nul
copy /y "%SHERPA_LIB%\sherpa-onnx-c-api.dll" build\ >nul
copy /y "%SHERPA_LIB%\onnxruntime.dll" build\ >nul
copy /y "%KNF_LIB%\kaldi-native-fbank-core.dll" build\ >nul

echo Build Success: build\VoiceLLMASRInput.exe
