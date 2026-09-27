#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
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
