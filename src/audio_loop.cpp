#include "audio_loop.hpp"

#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>

namespace {

WAVEFORMATEXTENSIBLE Float48kStereo() {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = 2;
    format.Format.nSamplesPerSec = 48000;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = 2 * sizeof(float);
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

ComPtr<IMMDevice> GetDevice(const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    CheckHr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&enumerator)), "Create MMDeviceEnumerator");
    ComPtr<IMMDevice> device;
    CheckHr(enumerator->GetDevice(id.c_str(), &device), "Get audio endpoint");
    return device;
}

} // namespace

AudioLoop::~AudioLoop() { Stop(); }

std::vector<AudioEndpoint> AudioLoop::EnumerateCapture() { return Enumerate(eCapture); }
std::vector<AudioEndpoint> AudioLoop::EnumerateRender() { return Enumerate(eRender); }

std::vector<AudioEndpoint> AudioLoop::Enumerate(EDataFlow flow) {
    std::vector<AudioEndpoint> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    CheckHr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&enumerator)), "Create MMDeviceEnumerator");
    ComPtr<IMMDeviceCollection> devices;
    CheckHr(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices),
            "EnumAudioEndpoints");
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device))) continue;
        wchar_t* rawId = nullptr;
        if (FAILED(device->GetId(&rawId))) continue;
        AudioEndpoint endpoint;
        endpoint.id = rawId;
        CoTaskMemFree(rawId);
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR) {
                endpoint.name = value.pwszVal;
            }
            PropVariantClear(&value);
        }
        if (endpoint.name.empty()) endpoint.name = endpoint.id;
        result.push_back(std::move(endpoint));
    }
    return result;
}

HRESULT AudioLoop::Start(const AudioEndpoint& input, const AudioEndpoint& output) {
    Stop();
    try {
        auto inputDevice = GetDevice(input.id);
        auto outputDevice = GetDevice(output.id);
        CheckHr(inputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(captureClient_.GetAddressOf())),
                "Activate capture IAudioClient");
        CheckHr(outputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(renderClient_.GetAddressOf())),
                "Activate render IAudioClient");

        auto format = Float48kStereo();
        WAVEFORMATEX* closest = nullptr;
        HRESULT hr = captureClient_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,
                                                        &format.Format, &closest);
        if (closest) CoTaskMemFree(closest);
        CheckHr(hr == S_OK ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT,
                "Capture 48 kHz stereo float support");
        closest = nullptr;
        hr = renderClient_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,
                                               &format.Format, &closest);
        if (closest) CoTaskMemFree(closest);
        CheckHr(hr == S_OK ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT,
                "Render 48 kHz stereo float support");

        captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        renderEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!captureEvent_ || !renderEvent_ || !stopEvent_) {
            CheckHr(HRESULT_FROM_WIN32(GetLastError()), "Create audio events");
        }
        constexpr REFERENCE_TIME bufferDuration = 200000; // 20 ms, bounded below by engine.
        const DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                  AUDCLNT_STREAMFLAGS_NOPERSIST;
        CheckHr(captureClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                           bufferDuration, 0, &format.Format, nullptr),
                "Initialize audio capture");
        CheckHr(renderClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                          bufferDuration, 0, &format.Format, nullptr),
                "Initialize audio render");
        CheckHr(captureClient_->SetEventHandle(captureEvent_), "Set capture event");
        CheckHr(renderClient_->SetEventHandle(renderEvent_), "Set render event");
        CheckHr(captureClient_->GetService(IID_PPV_ARGS(&captureService_)),
                "Get IAudioCaptureClient");
        CheckHr(renderClient_->GetService(IID_PPV_ARGS(&renderService_)),
                "Get IAudioRenderClient");
        CheckHr(renderClient_->GetBufferSize(&renderBufferFrames_), "Get render buffer size");

        readFrame_ = writeFrame_ = 0;
        bufferedFrames_ = 0;
        droppedFrames_ = 0;
        underflowFrames_ = 0;
        {
            std::scoped_lock lock(errorMutex_);
            error_.clear();
        }
        running_ = true;
        thread_ = std::thread(&AudioLoop::ThreadMain, this);
        DiagnosticLog(L"Audio thread created: " + input.name + L" -> " + output.name);
        return S_OK;
    } catch (const std::exception& exception) {
        std::wstring detail(exception.what(), exception.what() + strlen(exception.what()));
        {
            std::scoped_lock lock(errorMutex_);
            error_ = detail;
        }
        DiagnosticLog(L"Audio setup: " + detail);
        Stop();
        return E_FAIL;
    }
}

void AudioLoop::Stop() {
    running_ = false;
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (captureClient_) captureClient_->Stop();
    if (renderClient_) renderClient_->Stop();
    captureService_.Reset();
    renderService_.Reset();
    captureClient_.Reset();
    renderClient_.Reset();
    if (captureEvent_) CloseHandle(captureEvent_);
    if (renderEvent_) CloseHandle(renderEvent_);
    if (stopEvent_) CloseHandle(stopEvent_);
    captureEvent_ = renderEvent_ = stopEvent_ = nullptr;
}

AudioStats AudioLoop::Stats() const {
    AudioStats stats;
    stats.running = running_.load();
    stats.bufferedFrames = bufferedFrames_.load();
    stats.droppedFrames = droppedFrames_.load();
    stats.underflowFrames = underflowFrames_.load();
    {
        std::scoped_lock lock(errorMutex_);
        stats.error = error_;
    }
    return stats;
}

void AudioLoop::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    HRESULT hr = captureClient_->Start();
    if (SUCCEEDED(hr)) hr = renderClient_->Start();
    if (FAILED(hr)) {
        SetError(hr, L"Start audio stream");
        running_ = false;
    } else {
        DiagnosticLog(L"WASAPI capture and render streams started");
    }
    HANDLE events[]{stopEvent_, captureEvent_, renderEvent_};
    while (running_) {
        const DWORD wait = WaitForMultipleObjects(3, events, FALSE, 1000);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 1) DrainCapture();
        if (wait == WAIT_OBJECT_0 + 2) FillRender();
        if (wait == WAIT_TIMEOUT) continue;
        if (wait == WAIT_FAILED) {
            SetError(HRESULT_FROM_WIN32(GetLastError()), L"Audio event wait");
            break;
        }
    }
    if (captureClient_) captureClient_->Stop();
    if (renderClient_) renderClient_->Stop();
    running_ = false;
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

void AudioLoop::DrainCapture() {
    UINT32 packetFrames = 0;
    while (SUCCEEDED(captureService_->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
        BYTE* raw = nullptr;
        DWORD flags = 0;
        HRESULT hr = captureService_->GetBuffer(&raw, &packetFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            SetError(hr, L"Audio capture GetBuffer");
            return;
        }
        const float* samples = reinterpret_cast<const float*>(raw);
        uint32_t buffered = bufferedFrames_.load();
        for (UINT32 frame = 0; frame < packetFrames; ++frame) {
            if (buffered >= kMaxBufferedFrames) {
                readFrame_ = (readFrame_ + 1) % kRingFrames;
                --buffered;
                droppedFrames_.fetch_add(1);
            }
            for (uint32_t channel = 0; channel < kChannels; ++channel) {
                ring_[static_cast<size_t>(writeFrame_) * kChannels + channel] =
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f
                                                        : samples[static_cast<size_t>(frame) * kChannels + channel];
            }
            writeFrame_ = (writeFrame_ + 1) % kRingFrames;
            ++buffered;
        }
        bufferedFrames_ = buffered;
        captureService_->ReleaseBuffer(packetFrames);
    }
}

void AudioLoop::FillRender() {
    UINT32 padding = 0;
    HRESULT hr = renderClient_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        SetError(hr, L"Audio render GetCurrentPadding");
        return;
    }
    const UINT32 requested = renderBufferFrames_ - padding;
    if (requested == 0) return;
    BYTE* raw = nullptr;
    hr = renderService_->GetBuffer(requested, &raw);
    if (FAILED(hr)) {
        SetError(hr, L"Audio render GetBuffer");
        return;
    }
    float* destination = reinterpret_cast<float*>(raw);
    uint32_t buffered = bufferedFrames_.load();
    for (UINT32 frame = 0; frame < requested; ++frame) {
        if (buffered > 0) {
            for (uint32_t channel = 0; channel < kChannels; ++channel) {
                destination[static_cast<size_t>(frame) * kChannels + channel] =
                    ring_[static_cast<size_t>(readFrame_) * kChannels + channel];
            }
            readFrame_ = (readFrame_ + 1) % kRingFrames;
            --buffered;
        } else {
            destination[static_cast<size_t>(frame) * kChannels] = 0.0f;
            destination[static_cast<size_t>(frame) * kChannels + 1] = 0.0f;
            underflowFrames_.fetch_add(1);
        }
    }
    bufferedFrames_ = buffered;
    renderService_->ReleaseBuffer(requested, 0);
}

void AudioLoop::SetError(HRESULT hr, const wchar_t* context) {
    std::wstring message = context;
    message += L": ";
    message += HrText(hr);
    bool changed = false;
    {
        std::scoped_lock lock(errorMutex_);
        changed = error_ != message;
        error_ = message;
    }
    if (changed) DiagnosticLog(L"Audio error: " + message);
}
