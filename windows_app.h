#pragma once

#include <windows.h>
#include <string>
#include <vector>

class SingleInstanceGuard
{
public:
    SingleInstanceGuard() = default;
    SingleInstanceGuard(const SingleInstanceGuard &) = delete;
    SingleInstanceGuard &operator=(const SingleInstanceGuard &) = delete;

    ~SingleInstanceGuard()
    {
        if (handle_)
            CloseHandle(handle_);
    }

    bool Acquire(const wchar_t *name)
    {
        handle_ = CreateMutexW(nullptr, FALSE, name);
        if (!handle_)
            return false;

        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            alreadyRunning_ = true;
            CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
        return true;
    }

    bool AlreadyRunning() const
    {
        return alreadyRunning_;
    }

private:
    HANDLE handle_ = nullptr;
    bool alreadyRunning_ = false;
};

static inline bool GetCurrentExecutablePath(std::wstring &path)
{
    std::vector<wchar_t> buffer(512);
    for (;;)
    {
        DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return false;
        if (length < buffer.size())
        {
            path.assign(buffer.data(), length);
            return true;
        }
        if (buffer.size() >= 32768)
            return false;
        buffer.resize(buffer.size() * 2);
    }
}

static inline bool IsStartupEnabled(const wchar_t *valueName)
{
    std::wstring executablePath;
    if (!GetCurrentExecutablePath(executablePath))
        return false;
    const std::wstring expectedCommand = L"\"" + executablePath + L"\"";

    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size);
    if (result != ERROR_SUCCESS || type != REG_SZ ||
        size < sizeof(wchar_t) || size % sizeof(wchar_t) != 0)
    {
        RegCloseKey(key);
        return false;
    }

    std::vector<wchar_t> command(size / sizeof(wchar_t));
    result = RegQueryValueExW(
        key, valueName, nullptr, &type,
        reinterpret_cast<BYTE *>(command.data()), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || command.empty() || command.back() != L'\0')
        return false;

    command.pop_back();
    return CompareStringOrdinal(
               command.data(), static_cast<int>(command.size()),
               expectedCommand.data(), static_cast<int>(expectedCommand.size()), TRUE) ==
           CSTR_EQUAL;
}

static inline bool SetStartupEnabled(const wchar_t *valueName, bool enabled)
{
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS)
        return false;

    if (enabled)
    {
        std::wstring path;
        if (!GetCurrentExecutablePath(path))
        {
            RegCloseKey(key);
            return false;
        }

        std::wstring command = L"\"" + path + L"\"";
        result = RegSetValueExW(
            key, valueName, 0, REG_SZ,
            reinterpret_cast<const BYTE *>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        result = RegDeleteValueW(key, valueName);
        if (result == ERROR_FILE_NOT_FOUND)
            result = ERROR_SUCCESS;
    }

    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
