@echo off
setlocal

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo Visual Studio Build Tools not found: %VCVARS%
    exit /b 1
)

call "%VCVARS%"
cd /d "%~dp0\.."
if not exist build mkdir build
if not exist build\tools mkdir build\tools

set "OPUS_INCLUDE=%CD%\third_party\opus\include"
set "OPUS_LIB=%CD%\third_party\opus\lib"

cl /nologo /O2 /EHsc /MT /std:c++17 /utf-8 /DUNICODE /D_UNICODE /Isrc\asr /Isrc\core /I"%OPUS_INCLUDE%" /Fobuild\tools\ /Fe:build\tools\doubao_ime_probe.exe tools\doubao_ime_probe.cpp src\asr\doubao_ime_asr.cpp winhttp.lib bcrypt.lib crypt32.lib ole32.lib "%OPUS_LIB%\opus.lib"
if errorlevel 1 exit /b 1

build\tools\doubao_ime_probe.exe %*
exit /b %ERRORLEVEL%
