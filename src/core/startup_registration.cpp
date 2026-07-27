#include "startup_registration.h"

#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace {

constexpr wchar_t kRunKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"VoxType";

std::wstring CurrentExecutablePath(DWORD* error) {
    for (DWORD capacity = MAX_PATH; capacity <= 32768; capacity *= 2) {
        std::vector<wchar_t> buffer(capacity, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), capacity);
        if (length == 0) {
            *error = GetLastError();
            return {};
        }
        if (length < capacity - 1) {
            *error = ERROR_SUCCESS;
            return std::wstring(buffer.data(), length);
        }
    }

    *error = ERROR_FILENAME_EXCED_RANGE;
    return {};
}

bool EqualCommandLinesIgnoreCase(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
        left.c_str(), static_cast<int>(left.size()),
        right.c_str(), static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

DWORD WriteStartupRegistration() {
    DWORD pathError = ERROR_SUCCESS;
    const std::wstring executablePath = CurrentExecutablePath(&pathError);
    if (pathError != ERROR_SUCCESS) return pathError;

    const std::wstring commandLine = BuildStartupRegistrationCommandLine(executablePath);
    if (!IsStartupRegistrationCommandLineValid(commandLine)) {
        return ERROR_FILENAME_EXCED_RANGE;
    }

    HKEY key = nullptr;
    const LSTATUS openResult = RegCreateKeyExW(
        HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_SET_VALUE,
        nullptr, &key, nullptr);
    if (openResult != ERROR_SUCCESS) return static_cast<DWORD>(openResult);

    const DWORD bytes = static_cast<DWORD>((commandLine.size() + 1) * sizeof(wchar_t));
    const LSTATUS writeResult = RegSetValueExW(
        key, kRunValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(commandLine.c_str()), bytes);
    RegCloseKey(key);
    return static_cast<DWORD>(writeResult);
}

DWORD RemoveStartupRegistration() {
    HKEY key = nullptr;
    const LSTATUS openResult = RegOpenKeyExW(
        HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key);
    if (openResult == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (openResult != ERROR_SUCCESS) return static_cast<DWORD>(openResult);

    const LSTATUS deleteResult = RegDeleteValueW(key, kRunValueName);
    RegCloseKey(key);
    return deleteResult == ERROR_FILE_NOT_FOUND
        ? ERROR_SUCCESS
        : static_cast<DWORD>(deleteResult);
}

} // namespace

std::wstring BuildStartupRegistrationCommandLine(const std::wstring& executablePath) {
    if (executablePath.empty()) return {};
    return L"\"" + executablePath + L"\"";
}

bool IsStartupRegistrationCommandLineValid(const std::wstring& commandLine) {
    return !commandLine.empty() &&
        commandLine.size() <= kStartupRegistrationCommandLineMaxChars;
}

StartupRegistrationState QueryVoxTypeStartupRegistration() {
    HKEY key = nullptr;
    const LSTATUS openResult = RegOpenKeyExW(
        HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key);
    if (openResult == ERROR_FILE_NOT_FOUND) return {};
    if (openResult != ERROR_SUCCESS) return { false, false, {}, static_cast<DWORD>(openResult) };

    DWORD type = 0;
    DWORD bytes = 0;
    const LSTATUS sizeResult = RegQueryValueExW(
        key, kRunValueName, nullptr, &type, nullptr, &bytes);
    if (sizeResult == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return {};
    }
    if (sizeResult != ERROR_SUCCESS) {
        RegCloseKey(key);
        return { false, false, {}, static_cast<DWORD>(sizeResult) };
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        RegCloseKey(key);
        return { true, false, {}, ERROR_DATATYPE_MISMATCH };
    }

    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    const LSTATUS queryResult = RegQueryValueExW(
        key, kRunValueName, nullptr, &type,
        reinterpret_cast<BYTE*>(value.data()), &bytes);
    RegCloseKey(key);
    if (queryResult != ERROR_SUCCESS) {
        return { false, false, {}, static_cast<DWORD>(queryResult) };
    }

    const std::wstring commandLine(value.data());
    DWORD pathError = ERROR_SUCCESS;
    const std::wstring currentExecutable = CurrentExecutablePath(&pathError);
    if (pathError != ERROR_SUCCESS) {
        return { true, false, commandLine, pathError };
    }

    const std::wstring expected = BuildStartupRegistrationCommandLine(currentExecutable);
    return { true, EqualCommandLinesIgnoreCase(commandLine, expected), commandLine, ERROR_SUCCESS };
}

DWORD SetVoxTypeStartupRegistration(bool enable) {
    const StartupRegistrationState current = QueryVoxTypeStartupRegistration();
    if (!current.Succeeded()) return current.error;

    if (!enable) {
        return current.registered ? RemoveStartupRegistration() : ERROR_SUCCESS;
    }
    if (current.registered && current.pointsToCurrentExecutable) {
        return ERROR_SUCCESS;
    }
    return WriteStartupRegistration();
}

