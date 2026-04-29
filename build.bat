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

rc /nologo /fo build\app.res resources.rc
if errorlevel 1 exit /b 1

cl /nologo /O2 /EHsc /MT /std:c++17 /DUNICODE /D_UNICODE /Fobuild\ /Fe:build\VoiceLLMASRInput.exe main.cpp build\app.res user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib shlwapi.lib winmm.lib ws2_32.lib
if errorlevel 1 exit /b 1

echo Build Success: build\VoiceLLMASRInput.exe
