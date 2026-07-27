#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

// Windows Run-key values are command lines and are limited to 260 characters.
constexpr size_t kStartupRegistrationCommandLineMaxChars = 260;

struct StartupRegistrationState {
    bool registered = false;
    bool pointsToCurrentExecutable = false;
    std::wstring commandLine;
    DWORD error = ERROR_SUCCESS;

    bool Succeeded() const { return error == ERROR_SUCCESS; }
};

// The registry is the source of truth. This intentionally is not duplicated
// in config.json, where it could drift from Windows' actual Run registration.
StartupRegistrationState QueryVoxTypeStartupRegistration();

// Adds, updates, or removes the current user's VoxType Run value. Updating is
// required when a Portable folder has been moved since it was first registered.
DWORD SetVoxTypeStartupRegistration(bool enable);

std::wstring BuildStartupRegistrationCommandLine(const std::wstring& executablePath);
bool IsStartupRegistrationCommandLineValid(const std::wstring& commandLine);

