#pragma once

#include "common.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>
#include <mutex>
#include <vector>

struct VideoMode {
    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 fpsNum = 0;
    UINT32 fpsDen = 1;
    LONG stride = 0;
    DWORD nativeTypeIndex = 0;

    bool IsYuy2() const { return subtype == MFVideoFormat_YUY2; }
    std::wstring Label() const;
};

struct VideoDevice {
    std::wstring name;
    std::wstring symbolicLink;
    std::vector<VideoMode> modes;
};

struct CapturedFrame {
    std::vector<uint8_t> bytes;
    UINT32 width = 0;
    UINT32 height = 0;
    int64_t sourceTime100ns = 0;
    int64_t arrivalQpc = 0;
    int64_t estimatedCaptureQpc = 0;
    uint64_t serial = 0;
};

struct CaptureStats {
    double fps = 0.0;
    uint64_t received = 0;
    uint64_t dropped = 0;
    int queueDepth = 0;
};

class VideoCapture final : public IMFSourceReaderCallback {
public:
    VideoCapture();
    ~VideoCapture();

    VideoCapture(const VideoCapture&) = delete;
    VideoCapture& operator=(const VideoCapture&) = delete;

    static std::vector<VideoDevice> Enumerate();
    HRESULT Start(const VideoDevice& device, const VideoMode& mode);
    void Stop();
    bool TakeLatest(CapturedFrame& frame);
    HANDLE FrameEvent() const { return frameEvent_; }
    CaptureStats Stats() const;
    std::wstring LastError() const;

    STDMETHODIMP QueryInterface(REFIID iid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP OnReadSample(HRESULT status, DWORD streamIndex, DWORD streamFlags,
                              LONGLONG timestamp, IMFSample* sample) override;
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    HRESULT CopySample(IMFSample* sample, LONGLONG timestamp);
    void SetError(HRESULT hr, const wchar_t* context);

    std::atomic<ULONG> refs_{1};
    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFMediaSource> source_;
    HANDLE frameEvent_ = nullptr;
    std::atomic<bool> running_{false};
    VideoMode mode_{};

    mutable std::mutex frameMutex_;
    CapturedFrame pending_;
    std::vector<uint8_t> spareBytes_;
    bool hasPending_ = false;

    mutable std::mutex errorMutex_;
    std::wstring lastError_;

    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<double> fps_{0.0};
    int64_t fpsWindowQpc_ = 0;
    uint32_t fpsWindowFrames_ = 0;
    bool timelineSet_ = false;
    int64_t timelineQpc_ = 0;
    int64_t timelineSource100ns_ = 0;
};
