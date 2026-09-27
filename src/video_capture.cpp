#include "video_capture.hpp"

#include <mferror.h>
#include <sstream>

namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);

std::wstring SubtypeName(const GUID& guid) {
    if (guid == MFVideoFormat_YUY2) return L"YUY2";
    if (guid == MFVideoFormat_NV12) return L"NV12";
    if (guid == MFVideoFormat_RGB24) return L"RGB24";
    if (guid == MFVideoFormat_RGB32) return L"XRGB";
    if (guid == MFVideoFormat_ARGB32) return L"ARGB";
    if (guid == MFVideoFormat_MJPG) return L"MJPEG";
    wchar_t text[64]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    return text;
}

ComPtr<IMFMediaSource> ActivateVideoSource(const std::wstring& symbolicLink) {
    ComPtr<IMFAttributes> attrs;
    CheckHr(MFCreateAttributes(&attrs, 2), "MFCreateAttributes");
    CheckHr(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
            "Set source type");
    CheckHr(attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                             symbolicLink.c_str()),
            "Set symbolic link");
    ComPtr<IMFMediaSource> source;
    CheckHr(MFCreateDeviceSource(attrs.Get(), &source), "MFCreateDeviceSource");
    return source;
}

ComPtr<IMFAttributes> ReaderAttributes(IMFSourceReaderCallback* callback) {
    ComPtr<IMFAttributes> attrs;
    CheckHr(MFCreateAttributes(&attrs, 6), "MFCreateAttributes(reader)");
    CheckHr(attrs->SetUINT32(MF_LOW_LATENCY, TRUE), "MF_LOW_LATENCY");
    CheckHr(attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE),
            "MF_READWRITE_DISABLE_CONVERTERS");
    CheckHr(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE),
            "MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING");
    CheckHr(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE),
            "MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING");
    if (callback) {
        CheckHr(attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback),
                "MF_SOURCE_READER_ASYNC_CALLBACK");
    }
    return attrs;
}

} // namespace

std::wstring VideoMode::Label() const {
    std::wostringstream out;
    out << width << L"x" << height << L"  ";
    if (fpsDen != 0 && fpsNum % fpsDen == 0) out << fpsNum / fpsDen;
    else out << static_cast<double>(fpsNum) / static_cast<double>(fpsDen);
    out << L" fps  " << SubtypeName(subtype);
    return out.str();
}

VideoCapture::VideoCapture() {
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!frameEvent_) throw std::runtime_error("CreateEvent(frame) failed");
}

VideoCapture::~VideoCapture() {
    Stop();
    if (frameEvent_) CloseHandle(frameEvent_);
}

std::vector<VideoDevice> VideoCapture::Enumerate() {
    std::vector<VideoDevice> result;
    ComPtr<IMFAttributes> attrs;
    CheckHr(MFCreateAttributes(&attrs, 1), "MFCreateAttributes(devices)");
    CheckHr(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
            "Set video source type");

    IMFActivate** rawDevices = nullptr;
    UINT32 count = 0;
    CheckHr(MFEnumDeviceSources(attrs.Get(), &rawDevices, &count), "MFEnumDeviceSources");
    for (UINT32 i = 0; i < count; ++i) {
        VideoDevice device;
        wchar_t* value = nullptr;
        UINT32 chars = 0;
        if (SUCCEEDED(rawDevices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &value, &chars))) {
            device.name.assign(value, chars);
            CoTaskMemFree(value);
        }
        value = nullptr;
        chars = 0;
        if (SUCCEEDED(rawDevices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &value, &chars))) {
            device.symbolicLink.assign(value, chars);
            CoTaskMemFree(value);
        }

        try {
            auto source = ActivateVideoSource(device.symbolicLink);
            auto readerAttrs = ReaderAttributes(nullptr);
            ComPtr<IMFSourceReader> reader;
            CheckHr(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader),
                    "MFCreateSourceReaderFromMediaSource(enumerate)");
            for (DWORD typeIndex = 0;; ++typeIndex) {
                ComPtr<IMFMediaType> type;
                const HRESULT hr = reader->GetNativeMediaType(
                    kVideoStream, typeIndex, &type);
                if (hr == MF_E_NO_MORE_TYPES) break;
                if (FAILED(hr)) continue;
                VideoMode mode;
                mode.nativeTypeIndex = typeIndex;
                if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &mode.subtype))) continue;
                if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                                              &mode.width, &mode.height))) continue;
                MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &mode.fpsNum, &mode.fpsDen);
                UINT32 rawStride = 0;
                if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) {
                    mode.stride = static_cast<LONG>(rawStride);
                } else if (mode.IsYuy2()) {
                    mode.stride = static_cast<LONG>(mode.width * 2);
                }
                device.modes.push_back(mode);
            }
            source->Shutdown();
        } catch (...) {
            // Keep the device visible even if this driver refuses type enumeration.
        }
        result.push_back(std::move(device));
        rawDevices[i]->Release();
    }
    CoTaskMemFree(rawDevices);
    return result;
}

HRESULT VideoCapture::Start(const VideoDevice& device, const VideoMode& mode) {
    Stop();
    try {
        source_ = ActivateVideoSource(device.symbolicLink);
        auto attrs = ReaderAttributes(this);
        CheckHr(MFCreateSourceReaderFromMediaSource(source_.Get(), attrs.Get(), &reader_),
                "MFCreateSourceReaderFromMediaSource");
        CheckHr(reader_->SetStreamSelection(kAllStreams, FALSE),
                "SetStreamSelection(all off)");
        CheckHr(reader_->SetStreamSelection(kVideoStream, TRUE),
                "SetStreamSelection(video on)");

        ComPtr<IMFMediaType> nativeType;
        CheckHr(reader_->GetNativeMediaType(kVideoStream,
                                            mode.nativeTypeIndex, &nativeType),
                "GetNativeMediaType");
        GUID actualSubtype{};
        UINT32 actualWidth = 0, actualHeight = 0, actualNum = 0, actualDen = 0;
        CheckHr(nativeType->GetGUID(MF_MT_SUBTYPE, &actualSubtype), "Get subtype");
        CheckHr(MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE,
                                   &actualWidth, &actualHeight), "Get frame size");
        CheckHr(MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE,
                                    &actualNum, &actualDen), "Get frame rate");
        if (actualSubtype != mode.subtype || actualWidth != mode.width ||
            actualHeight != mode.height || actualNum != mode.fpsNum || actualDen != mode.fpsDen) {
            return MF_E_INVALIDMEDIATYPE;
        }
        CheckHr(reader_->SetCurrentMediaType(kVideoStream,
                                             nullptr, nativeType.Get()),
                "SetCurrentMediaType(native)");

        mode_ = mode;
        received_ = 0;
        dropped_ = 0;
        fps_ = 0.0;
        fpsWindowQpc_ = QpcNow();
        fpsWindowFrames_ = 0;
        timelineSet_ = false;
        {
            std::scoped_lock lock(frameMutex_);
            pending_ = {};
            hasPending_ = false;
        }
        {
            std::scoped_lock lock(errorMutex_);
            lastError_.clear();
        }
        running_ = true;
        const HRESULT hr = reader_->ReadSample(kVideoStream, 0,
                                               nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            running_ = false;
            SetError(hr, L"Initial ReadSample");
        }
        return hr;
    } catch (const std::exception&) {
        Stop();
        return E_FAIL;
    }
}

void VideoCapture::Stop() {
    running_ = false;
    if (reader_) reader_->Flush(kVideoStream);
    reader_.Reset();
    if (source_) source_->Shutdown();
    source_.Reset();
}

bool VideoCapture::TakeLatest(CapturedFrame& frame) {
    std::scoped_lock lock(frameMutex_);
    if (!hasPending_) return false;
    spareBytes_ = std::move(frame.bytes);
    frame = std::move(pending_);
    pending_ = {};
    hasPending_ = false;
    return true;
}

CaptureStats VideoCapture::Stats() const {
    CaptureStats stats;
    stats.fps = fps_.load();
    stats.received = received_.load();
    stats.dropped = dropped_.load();
    {
        std::scoped_lock lock(frameMutex_);
        stats.queueDepth = hasPending_ ? 1 : 0;
    }
    return stats;
}

std::wstring VideoCapture::LastError() const {
    std::scoped_lock lock(errorMutex_);
    return lastError_;
}

HRESULT VideoCapture::QueryInterface(REFIID iid, void** object) {
    if (!object) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
        *object = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG VideoCapture::AddRef() { return ++refs_; }

ULONG VideoCapture::Release() {
    // This object is stack/owner managed. MF's callback references are still tracked so that the
    // COM contract is obeyed, but lifetime is ended by the owner after Stop/Flush.
    return --refs_;
}

HRESULT VideoCapture::OnReadSample(HRESULT status, DWORD, DWORD streamFlags,
                                   LONGLONG timestamp, IMFSample* sample) {
    if (!running_) return S_OK;
    if (FAILED(status)) {
        SetError(status, L"ReadSample callback");
        running_ = false;
        return status;
    }
    if (streamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        SetError(MF_E_TRANSFORM_STREAM_CHANGE, L"Unexpected media type change");
        running_ = false;
        return S_OK;
    }
    if (sample) {
        const HRESULT hr = CopySample(sample, timestamp);
        if (FAILED(hr)) SetError(hr, L"CopySample");
    }
    if (running_ && reader_) {
        const HRESULT hr = reader_->ReadSample(kVideoStream, 0,
                                               nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            SetError(hr, L"ReadSample(reissue)");
            running_ = false;
        }
    }
    return S_OK;
}

HRESULT VideoCapture::CopySample(IMFSample* sample, LONGLONG timestamp) {
    const DWORD rowBytes = mode_.width * 2;
    CapturedFrame frame;
    frame.width = mode_.width;
    frame.height = mode_.height;
    frame.sourceTime100ns = timestamp;
    frame.arrivalQpc = QpcNow();
    frame.serial = received_.fetch_add(1) + 1;
    {
        std::scoped_lock lock(frameMutex_);
        frame.bytes = std::move(spareBytes_);
    }
    frame.bytes.resize(static_cast<size_t>(rowBytes) * mode_.height);

    if (!timelineSet_) {
        timelineSet_ = true;
        timelineQpc_ = frame.arrivalQpc;
        timelineSource100ns_ = timestamp;
    }
    const double sourceDeltaSeconds = static_cast<double>(timestamp - timelineSource100ns_) / 1.0e7;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    frame.estimatedCaptureQpc = timelineQpc_ +
        static_cast<int64_t>(sourceDeltaSeconds * static_cast<double>(frequency.QuadPart));

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) return hr;

    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        BYTE* scanline0 = nullptr;
        LONG pitch = 0;
        hr = buffer2d->Lock2D(&scanline0, &pitch);
        if (FAILED(hr)) return hr;
        for (UINT32 y = 0; y < mode_.height; ++y) {
            memcpy(frame.bytes.data() + static_cast<size_t>(y) * rowBytes,
                   scanline0 + static_cast<ptrdiff_t>(y) * pitch, rowBytes);
        }
        buffer2d->Unlock2D();
    } else {
        BYTE* data = nullptr;
        DWORD maxLength = 0, currentLength = 0;
        hr = buffer->Lock(&data, &maxLength, &currentLength);
        if (FAILED(hr)) return hr;
        const LONG stride = mode_.stride != 0 ? mode_.stride : static_cast<LONG>(rowBytes);
        const size_t required = static_cast<size_t>(std::abs(stride)) * mode_.height;
        if (currentLength < required) {
            buffer->Unlock();
            return MF_E_BUFFERTOOSMALL;
        }
        const BYTE* first = data;
        if (stride < 0) first += static_cast<size_t>(mode_.height - 1) * -stride;
        for (UINT32 y = 0; y < mode_.height; ++y) {
            memcpy(frame.bytes.data() + static_cast<size_t>(y) * rowBytes,
                   first + static_cast<ptrdiff_t>(y) * stride, rowBytes);
        }
        buffer->Unlock();
    }

    {
        std::scoped_lock lock(frameMutex_);
        if (hasPending_) {
            dropped_.fetch_add(1);
            spareBytes_ = std::move(pending_.bytes);
        }
        pending_ = std::move(frame);
        hasPending_ = true;
    }
    SetEvent(frameEvent_);

    ++fpsWindowFrames_;
    const int64_t now = QpcNow();
    const double elapsed = QpcSeconds(now - fpsWindowQpc_);
    if (elapsed >= 1.0) {
        fps_ = static_cast<double>(fpsWindowFrames_) / elapsed;
        fpsWindowFrames_ = 0;
        fpsWindowQpc_ = now;
    }
    return S_OK;
}

void VideoCapture::SetError(HRESULT hr, const wchar_t* context) {
    std::scoped_lock lock(errorMutex_);
    lastError_ = context;
    lastError_ += L": ";
    lastError_ += HrText(hr);
}
