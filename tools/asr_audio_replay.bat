@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0\.."

if "%~1"=="" goto :usage
if /I "%~1"=="--help" goto :usage
if /I "%~1"=="-h" goto :usage
if /I "%~1"=="/?" goto :usage

call build.bat
if errorlevel 1 exit /b %ERRORLEVEL%

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="
if exist "%VSWHERE%" (
    for /f "tokens=*" %%i in ('"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS_PATH=%%i"
)
if not defined VS_PATH set "VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community"

set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE_EXE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%VCVARS%" (
    echo ERROR: Visual Studio C++ x64 tools were not found: %VCVARS%
    exit /b 1
)
if not exist "%CMAKE_EXE%" (
    echo ERROR: Visual Studio bundled CMake was not found: %CMAKE_EXE%
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%

"%CMAKE_EXE%" --build --preset x64-release --target asr_audio_replay
if errorlevel 1 exit /b %ERRORLEVEL%

set "REPLAY_EXE=%CD%\build\artifacts\tools\asr_audio_replay.exe"
set "RUNTIME_DIR=%CD%\build\run\x64-release"
if not exist "%REPLAY_EXE%" (
    echo ERROR: Replay executable was not produced: %REPLAY_EXE%
    exit /b 1
)

set "PATH=%RUNTIME_DIR%;%PATH%"
"%REPLAY_EXE%" --runtime-dir "%RUNTIME_DIR%" %*
set "REPLAY_EXIT=%ERRORLEVEL%"
exit /b %REPLAY_EXIT%

:usage
echo Usage: tools\asr_audio_replay.bat --wav ^<diagnostic.wav^> [options]
echo.
echo The default backend is local. Cloud audio is uploaded only when an explicit
echo cloud backend is selected. Run with --list-backends to list IDs.
exit /b 0
