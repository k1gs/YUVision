#pragma once

#include <windows.h>
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

inline std::wstring DiagnosticLogPath() {
    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(directory)), directory);
    if (length == 0 || length >= std::size(directory)) return L"331Viewer-YUY2Fix.log";
    return std::wstring(directory, length) + L"331Viewer-YUY2Fix.log";
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
    DeleteFileW(DiagnosticLogPath().c_str());
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
