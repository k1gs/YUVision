#pragma once

#include "common.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

struct AudioEndpoint {
    std::wstring id;
    std::wstring name;
};

struct AudioStats {
    bool running = false;
    uint32_t bufferedFrames = 0;
    uint64_t droppedFrames = 0;
    uint64_t underflowFrames = 0;
    std::wstring error;
};

class AudioLoop {
public:
    ~AudioLoop();

    static std::vector<AudioEndpoint> EnumerateCapture();
    static std::vector<AudioEndpoint> EnumerateRender();
    HRESULT Start(const AudioEndpoint& input, const AudioEndpoint& output);
    void Stop();
    AudioStats Stats() const;

private:
    static std::vector<AudioEndpoint> Enumerate(EDataFlow flow);
    void ThreadMain();
    void DrainCapture();
    void FillRender();
    void SetError(HRESULT hr, const wchar_t* context);

    ComPtr<IAudioClient> captureClient_;
    ComPtr<IAudioCaptureClient> captureService_;
    ComPtr<IAudioClient> renderClient_;
    ComPtr<IAudioRenderClient> renderService_;
    HANDLE captureEvent_ = nullptr;
    HANDLE renderEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    static constexpr uint32_t kChannels = 2;
    static constexpr uint32_t kRingFrames = 4096;
    static constexpr uint32_t kMaxBufferedFrames = 1920; // 40 ms at 48 kHz.
    std::vector<float> ring_{static_cast<size_t>(kRingFrames) * kChannels};
    uint32_t readFrame_ = 0;
    uint32_t writeFrame_ = 0;
    std::atomic<uint32_t> bufferedFrames_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<uint64_t> underflowFrames_{0};
    UINT32 renderBufferFrames_ = 0;

    mutable std::mutex errorMutex_;
    std::wstring error_;
};
