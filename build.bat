@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem =============================================================================
rem VoxType build entry
rem   build.bat                    Incremental CMake build + exact runtime install
rem   build.bat --package          Build and create verified MSI + Portable assets
rem   build.bat --package-msi      Build and create a verified MSI
rem   build.bat --package-portable Build and create a verified Portable .7z
rem   build.bat --test             Build and run offline protocol/request tests
rem   build.bat --clean            Remove generated compile/runtime/test/log trees
rem
rem Generated layout: cmake\x64-release, run\x64-release, packages,
rem                   artifacts, logs, and README.txt only.
rem =============================================================================

set "VOXTYPE_CLEAN_ONLY=0"
set "VOXTYPE_REBUILD=0"
set "VOXTYPE_PACKAGE_MODE="
set "VOXTYPE_REQUIRE_SIGNING=0"
set "VOXTYPE_TEST_MODE=0"

for %%A in (%*) do (
    if /I "%%~A"=="--clean" set "VOXTYPE_CLEAN_ONLY=1"
    if /I "%%~A"=="--rebuild" set "VOXTYPE_REBUILD=1"
    if /I "%%~A"=="--stop-running" rem accepted; the runtime instance is stopped automatically
    if /I "%%~A"=="--package" call :set_package_mode All
    if /I "%%~A"=="--package-msi" call :set_package_mode Msi
    if /I "%%~A"=="--package-portable" call :set_package_mode Portable
    if /I "%%~A"=="--require-signing" set "VOXTYPE_REQUIRE_SIGNING=1"
    if /I "%%~A"=="--test" set "VOXTYPE_TEST_MODE=1"
    if /I "%%~A"=="--help" goto :usage
    if /I "%%~A"=="-h" goto :usage
    if /I "%%~A"=="/?" goto :usage
)

if "!VOXTYPE_PACKAGE_CONFLICT!"=="1" (
    echo ERROR: choose only one of --package, --package-msi, or --package-portable.
    exit /b 2
)
if "!VOXTYPE_REQUIRE_SIGNING!"=="1" if not defined VOXTYPE_PACKAGE_MODE (
    echo ERROR: --require-signing requires a package mode.
    exit /b 2
)

set "ROOT=%CD%"
set "BUILD_ROOT=%ROOT%\build"
set "BUILD_TREE=%BUILD_ROOT%\cmake\x64-release"
set "RUNTIME_DIR=%BUILD_ROOT%\run\x64-release"
set "RUNTIME_EXE=%RUNTIME_DIR%\VoxType.exe"
set "INSTALL_MANIFEST=%BUILD_TREE%\install_manifest_Runtime.txt"
set "VERSION_FILE=%BUILD_TREE%\voxtype-version.txt"
set "POWERSHELL_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"

if not exist "%POWERSHELL_EXE%" (
    echo ERROR: Windows PowerShell was not found: %POWERSHELL_EXE%
    exit /b 1
)

call :stop_runtime_instance
if errorlevel 1 exit /b !ERRORLEVEL!
call :validate_existing_layout
if errorlevel 1 exit /b !ERRORLEVEL!

if "!VOXTYPE_CLEAN_ONLY!"=="1" (
    call :clean_generated
    exit /b !ERRORLEVEL!
)
if "!VOXTYPE_REBUILD!"=="1" (
    call :clean_generated
    if errorlevel 1 exit /b !ERRORLEVEL!
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="
if exist "%VSWHERE%" (
    for /f "tokens=*" %%i in ('"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS_PATH=%%i"
)
if not defined VS_PATH set "VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community"

set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo ERROR: Visual Studio C++ x64 tools were not found: %VCVARS%
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b !ERRORLEVEL!

set "CMAKE_EXE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_DIR=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if not exist "%CMAKE_EXE%" (
    echo ERROR: Visual Studio bundled CMake was not found: %CMAKE_EXE%
    exit /b 1
)
if not exist "%NINJA_DIR%\ninja.exe" (
    echo ERROR: Visual Studio bundled Ninja was not found: %NINJA_DIR%\ninja.exe
    exit /b 1
)
set "PATH=%NINJA_DIR%;%PATH%"

echo Configuring CMake preset x64-release...
"%CMAKE_EXE%" --preset x64-release
if errorlevel 1 exit /b !ERRORLEVEL!

echo Building VoxType...
"%CMAKE_EXE%" --build --preset x64-release --target VoxType
if errorlevel 1 exit /b !ERRORLEVEL!

if not exist "%BUILD_TREE%\VoxType.exe" (
    echo ERROR: CMake did not produce %BUILD_TREE%\VoxType.exe
    exit /b 1
)
if not exist "%VERSION_FILE%" (
    echo ERROR: CMake did not produce %VERSION_FILE%
    exit /b 1
)
set "VOXTYPE_PRODUCT_VERSION="
for /f "usebackq delims=" %%v in ("%VERSION_FILE%") do set "VOXTYPE_PRODUCT_VERSION=%%v"
if not defined VOXTYPE_PRODUCT_VERSION (
    echo ERROR: generated product version is empty.
    exit /b 1
)

call :stop_runtime_instance
if errorlevel 1 exit /b !ERRORLEVEL!

echo Installing canonical runtime payload...
"%CMAKE_EXE%" --install "%BUILD_TREE%" --prefix "%RUNTIME_DIR%" --component Runtime
if errorlevel 1 exit /b !ERRORLEVEL!

if not exist "%RUNTIME_EXE%" (
    echo ERROR: Runtime install did not produce %RUNTIME_EXE%
    exit /b 1
)

"%POWERSHELL_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\validate_build_layout.ps1" ^
    -BuildRoot "%BUILD_ROOT%" -RuntimeDirectory "%RUNTIME_DIR%" -InstallManifest "%INSTALL_MANIFEST%"
if errorlevel 1 exit /b !ERRORLEVEL!

"%POWERSHELL_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\validate_settings_layout.ps1" ^
    -SourceRoot "%ROOT%"
if errorlevel 1 exit /b !ERRORLEVEL!

call :write_layout_readme

if "!VOXTYPE_TEST_MODE!"=="1" (
    echo Building offline protocol/request regression tests...
    "%CMAKE_EXE%" --build --preset x64-release --target qwen_free_protocol_test qwen_audio_json_test llm_refine_test audio_diagnostics_test asr_json_protocol_test
    if errorlevel 1 exit /b !ERRORLEVEL!
    set "VOXTYPE_TEST_EXE=%BUILD_ROOT%\artifacts\tests\qwen_free_protocol_test.exe"
    if not exist "!VOXTYPE_TEST_EXE!" (
        echo ERROR: Qwen protocol test executable was not produced: !VOXTYPE_TEST_EXE!
        exit /b 1
    )
    echo Running offline Qwen protocol regression tests...
    "!VOXTYPE_TEST_EXE!"
    if errorlevel 1 exit /b !ERRORLEVEL!
    set "VOXTYPE_AUDIO_JSON_TEST_EXE=%BUILD_ROOT%\artifacts\tests\qwen_audio_json_test.exe"
    if not exist "!VOXTYPE_AUDIO_JSON_TEST_EXE!" (
        echo ERROR: Qwen Audio JSON test executable was not produced: !VOXTYPE_AUDIO_JSON_TEST_EXE!
        exit /b 1
    )
    echo Running Qwen Audio JSON regression tests...
    "!VOXTYPE_AUDIO_JSON_TEST_EXE!"
    if errorlevel 1 exit /b !ERRORLEVEL!
    set "VOXTYPE_LLM_REFINE_TEST_EXE=%BUILD_ROOT%\artifacts\tests\llm_refine_test.exe"
    if not exist "!VOXTYPE_LLM_REFINE_TEST_EXE!" (
        echo ERROR: LLM refine test executable was not produced: !VOXTYPE_LLM_REFINE_TEST_EXE!
        exit /b 1
    )
    echo Running LLM refine regression tests...
    "!VOXTYPE_LLM_REFINE_TEST_EXE!"
    if errorlevel 1 exit /b !ERRORLEVEL!
    set "VOXTYPE_AUDIO_DIAGNOSTICS_TEST_EXE=%BUILD_ROOT%\artifacts\tests\audio_diagnostics_test.exe"
    if not exist "!VOXTYPE_AUDIO_DIAGNOSTICS_TEST_EXE!" (
        echo ERROR: Audio diagnostics test executable was not produced: !VOXTYPE_AUDIO_DIAGNOSTICS_TEST_EXE!
        exit /b 1
    )
    echo Running audio diagnostics regression tests...
    "!VOXTYPE_AUDIO_DIAGNOSTICS_TEST_EXE!"
    if errorlevel 1 exit /b !ERRORLEVEL!
    set "VOXTYPE_ASR_JSON_PROTOCOL_TEST_EXE=%BUILD_ROOT%\artifacts\tests\asr_json_protocol_test.exe"
    if not exist "!VOXTYPE_ASR_JSON_PROTOCOL_TEST_EXE!" (
        echo ERROR: ASR JSON protocol test executable was not produced: !VOXTYPE_ASR_JSON_PROTOCOL_TEST_EXE!
        exit /b 1
    )
    echo Running ASR JSON protocol regression tests...
    "!VOXTYPE_ASR_JSON_PROTOCOL_TEST_EXE!"
    if errorlevel 1 exit /b !ERRORLEVEL!
)

if defined VOXTYPE_PACKAGE_MODE (
    echo Packaging !VOXTYPE_PACKAGE_MODE! from the canonical runtime payload...
    set "VOXTYPE_SIGNING_ARGUMENT="
    if "!VOXTYPE_REQUIRE_SIGNING!"=="1" set "VOXTYPE_SIGNING_ARGUMENT=-RequireSigning"
    "%POWERSHELL_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\package_voxtype.ps1" ^
        -Mode "!VOXTYPE_PACKAGE_MODE!" -BuildRoot "%BUILD_ROOT%" ^
        -RuntimeDirectory "%RUNTIME_DIR%" -InstallManifest "%INSTALL_MANIFEST%" ^
        -ProductVersion "!VOXTYPE_PRODUCT_VERSION!" !VOXTYPE_SIGNING_ARGUMENT!
    if errorlevel 1 exit /b !ERRORLEVEL!
)

echo Build Success
echo Runnable: %RUNTIME_EXE%
if defined VOXTYPE_PACKAGE_MODE echo Packages: %BUILD_ROOT%\packages
exit /b 0

:set_package_mode
if defined VOXTYPE_PACKAGE_MODE if /I not "!VOXTYPE_PACKAGE_MODE!"=="%~1" (
    set "VOXTYPE_PACKAGE_CONFLICT=1"
)
set "VOXTYPE_PACKAGE_MODE=%~1"
exit /b 0

:stop_runtime_instance
if not exist "%RUNTIME_EXE%" exit /b 0
"%POWERSHELL_EXE%" -NoProfile -ExecutionPolicy Bypass -Command "$target = [IO.Path]::GetFullPath('%RUNTIME_EXE%'); Get-Process VoxType -ErrorAction SilentlyContinue | Where-Object { $_.Path -and [string]::Equals($_.Path, $target, [System.StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force"
exit /b 0

:clean_generated
for %%D in ("%BUILD_ROOT%\cmake" "%BUILD_ROOT%\run" "%BUILD_ROOT%\artifacts" "%BUILD_ROOT%\logs") do (
    if exist "%%~fD" rmdir /s /q "%%~fD"
)
if exist "%BUILD_ROOT%\README.txt" del /f /q "%BUILD_ROOT%\README.txt"
call :validate_existing_layout
if errorlevel 1 exit /b !ERRORLEVEL!
echo Clean complete. %BUILD_ROOT%\packages was preserved.
exit /b 0

:write_layout_readme
if not exist "%BUILD_ROOT%" mkdir "%BUILD_ROOT%"
(
    echo VoxType generated output
    echo.
    echo cmake\x64-release\  - disposable CMake/Ninja cache, objects, and package staging
    echo run\x64-release\    - sole supported runnable development payload
    echo packages\           - verified MSI and Portable release assets; preserved by --clean
    echo artifacts\          - generated package verification, test, and diagnostic reports
    echo logs\               - explicit build and test logs
    echo.
    echo The top-level whitelist is cmake, run, packages, artifacts, logs, and README.txt.
    echo Unexpected build-root files fail validation. Do not store source files, backups, or user data here.
) > "%BUILD_ROOT%\README.txt"
exit /b 0

:validate_existing_layout
if not exist "%BUILD_ROOT%" exit /b 0
"%POWERSHELL_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\validate_build_layout.ps1" ^
    -BuildRoot "%BUILD_ROOT%" -RuntimeDirectory "%RUNTIME_DIR%" -InstallManifest "%INSTALL_MANIFEST%" -SkipRuntime
exit /b !ERRORLEVEL!

:usage
echo Usage: build.bat [--rebuild] [--clean] [--test] [--package ^| --package-msi ^| --package-portable] [--require-signing]
exit /b 0
