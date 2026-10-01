#pragma once

#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

inline void CheckHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buffer[256]{};
        sprintf_s(buffer, "%s failed (0x%08X)", what, static_cast<unsigned>(hr));
        throw std::runtime_error(buffer);
    }
}

inline std::wstring HrText(HRESULT hr) {
    wchar_t* raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0,
                   reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring result = raw ? raw : L"Unknown error";
    if (raw) LocalFree(raw);
    return result;
}

inline int64_t QpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

inline double QpcSeconds(int64_t ticks) {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<double>(ticks) / frequency;
}

inline std::wstring DiagnosticLogDirectory() {
    PWSTR localAppData = nullptr;
    std::wstring directory;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                       &localAppData)) && localAppData) {
        directory = std::wstring(localAppData) + L"\\YUVision";
        CoTaskMemFree(localAppData);
        CreateDirectoryW(directory.c_str(), nullptr);
        directory += L"\\Logs";
        CreateDirectoryW(directory.c_str(), nullptr);
        return directory;
    }
    if (localAppData) CoTaskMemFree(localAppData);

    wchar_t temporary[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    if (length == 0 || length >= std::size(temporary)) return L".";
    return std::wstring(temporary, length);
}

inline std::wstring DiagnosticLogPath() {
    return DiagnosticLogDirectory() + L"\\YUVision.log";
}

inline std::wstring PreviousDiagnosticLogPath() {
    return DiagnosticLogDirectory() + L"\\YUVision.previous.log";
}

inline std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

inline void ResetDiagnosticLog() {
    const std::wstring current = DiagnosticLogPath();
    const std::wstring previous = PreviousDiagnosticLogPath();
    DeleteFileW(previous.c_str());
    MoveFileExW(current.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
}

inline void DiagnosticLog(const std::wstring& message) {
    static std::mutex mutex;
    std::scoped_lock lock(mutex);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t prefix[32]{};
    swprintf_s(prefix, L"%02u:%02u:%02u.%03u  ", time.wHour, time.wMinute, time.wSecond,
               time.wMilliseconds);
    std::ofstream stream(DiagnosticLogPath(), std::ios::binary | std::ios::app);
    const std::string line = Utf8(std::wstring(prefix) + message + L"\r\n");
    stream.write(line.data(), static_cast<std::streamsize>(line.size()));
}
