#include "audio/audio_loop.hpp"
#include "capture/video_capture.hpp"
#include "render/renderer.hpp"
#include "resources/resource.h"

#include <mfapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <sstream>

namespace {

constexpr UINT kDeviceBase = 1000;
constexpr UINT kModeBase = 2000;
constexpr UINT kChromaBase = 3000;
constexpr UINT kRangeLimited = 3010;
constexpr UINT kRangeFull = 3011;
constexpr UINT kToggleVsync = 3020;
constexpr UINT kToggleFullscreen = 3021;
constexpr UINT kToggleOverlay = 3022;
constexpr UINT kOffsetMinus = 3030;
constexpr UINT kOffsetPlus = 3031;
constexpr UINT kThresholdMinus = 3040;
constexpr UINT kThresholdPlus = 3041;
constexpr UINT kToggleSplit = 3050;
constexpr UINT kToggleDownscaleAa = 3051;
constexpr UINT kAudioInputBase = 4000;
constexpr UINT kAudioOutputBase = 5000;
constexpr UINT kAudioVolumeDown = 6000;
constexpr UINT kAudioVolumeUp = 6001;
constexpr UINT kAudioMute = 6002;
constexpr UINT kOpenLogFolder = 6100;

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::wstring DefaultEndpointId(EDataFlow flow) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) return {};
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) return {};
    wchar_t* raw = nullptr;
    if (FAILED(device->GetId(&raw))) return {};
    std::wstring id = raw;
    CoTaskMemFree(raw);
    return id;
}

class App {
public:
    ~App();
    int Run(HINSTANCE instance, int show);
    LRESULT WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

private:
    void EnumerateDevices();
    HMENU BuildMenu(bool popupRoot) const;
    void RebuildMenu();
    void StartBestVideo();
    void StartVideo(size_t modeIndex);
    void StartAudio();
    void AdjustAudioVolume(float delta);
    void ToggleAudioMute();
    void HandleCommand(UINT command);
    void HandleKey(UINT key, bool shift);
    void ToggleFullscreen();
    std::wstring OverlayText(const CapturedFrame* frame) const;
    static size_t FindBestMode(const VideoDevice& device);

    HWND window_ = nullptr;
    HMENU contextMenu_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Renderer renderer_;
    VideoCapture capture_;
    AudioLoop audio_;
    std::vector<VideoDevice> videoDevices_;
    std::vector<AudioEndpoint> audioInputs_;
    std::vector<AudioEndpoint> audioOutputs_;
    size_t videoDeviceIndex_ = 0;
    size_t videoModeIndex_ = 0;
    size_t audioInputIndex_ = 0;
    size_t audioOutputIndex_ = 0;
    bool rendererReady_ = false;
    bool fullscreen_ = false;
    int wheelDeltaRemainder_ = 0;
    WINDOWPLACEMENT windowPlacement_{sizeof(WINDOWPLACEMENT)};
    DWORD windowStyle_ = 0;
    std::wstring currentFormat_ = L"No signal";
    std::wstring captureState_ = L"Starting";
};

App* gApp = nullptr;

App::~App() {
    if (contextMenu_) DestroyMenu(contextMenu_);
}

LRESULT CALLBACK StaticWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return app ? app->WindowProc(window, message, wparam, lparam)
               : DefWindowProcW(window, message, wparam, lparam);
}

size_t App::FindBestMode(const VideoDevice& device) {
    size_t fallback = device.modes.size();
    for (size_t i = 0; i < device.modes.size(); ++i) {
        const auto& mode = device.modes[i];
        if (!mode.IsYuy2()) continue;
        if (fallback == device.modes.size()) fallback = i;
        const double fps = mode.fpsDen ? static_cast<double>(mode.fpsNum) / mode.fpsDen : 0.0;
        if (mode.width == 1920 && mode.height == 1080 && fps > 59.0 && fps < 61.0) return i;
    }
    return fallback;
}

void App::EnumerateDevices() {
    videoDevices_ = VideoCapture::Enumerate();
    audioInputs_ = AudioLoop::EnumerateCapture();
    audioOutputs_ = AudioLoop::EnumerateRender();
    DiagnosticLog(L"Video devices: " + std::to_wstring(videoDevices_.size()) +
                  L", audio inputs: " + std::to_wstring(audioInputs_.size()) +
                  L", audio outputs: " + std::to_wstring(audioOutputs_.size()));
    for (const auto& device : videoDevices_) {
        DiagnosticLog(L"Video device: " + device.name + L" (" +
                      std::to_wstring(device.modes.size()) + L" native modes)");
        for (const auto& mode : device.modes) DiagnosticLog(L"  " + mode.Label());
    }

    const std::wstring defaultOutput = DefaultEndpointId(eRender);
    for (size_t i = 0; i < audioOutputs_.size(); ++i) {
        if (audioOutputs_[i].id == defaultOutput) {
            audioOutputIndex_ = i;
            break;
        }
    }

    for (size_t i = 0; i < videoDevices_.size(); ++i) {
        const auto name = Lower(videoDevices_[i].name);
        if (name.find(L"ezcap") != std::wstring::npos ||
            name.find(L"cam link") != std::wstring::npos) {
            videoDeviceIndex_ = i;
            break;
        }
    }
    for (size_t i = 0; i < audioInputs_.size(); ++i) {
        const auto name = Lower(audioInputs_[i].name);
        if (name.find(L"ezcap") != std::wstring::npos ||
            name.find(L"cam link") != std::wstring::npos ||
            name.find(L"digital audio") != std::wstring::npos) {
            audioInputIndex_ = i;
            break;
        }
    }
}

HMENU App::BuildMenu(bool popupRoot) const {
    HMENU menu = popupRoot ? CreatePopupMenu() : CreateMenu();
    HMENU video = CreatePopupMenu();
    HMENU devices = CreatePopupMenu();
    for (size_t i = 0; i < videoDevices_.size(); ++i) {
        AppendMenuW(devices, MF_STRING | (i == videoDeviceIndex_ ? MF_CHECKED : 0),
                    kDeviceBase + static_cast<UINT>(i), videoDevices_[i].name.c_str());
    }
    if (videoDevices_.empty()) AppendMenuW(devices, MF_STRING | MF_GRAYED, 0, L"No devices");
    AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(devices), L"Device");

    HMENU modes = CreatePopupMenu();
    if (videoDeviceIndex_ < videoDevices_.size()) {
        const auto& list = videoDevices_[videoDeviceIndex_].modes;
        for (size_t i = 0; i < list.size(); ++i) {
            const UINT flags = MF_STRING | (i == videoModeIndex_ ? MF_CHECKED : 0) |
                               (!list[i].IsYuy2() ? MF_GRAYED : 0);
            AppendMenuW(modes, flags, kModeBase + static_cast<UINT>(i), list[i].Label().c_str());
        }
    }
    AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(modes), L"Native mode");
    AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(video, MF_STRING | (renderer_.Vsync() ? MF_CHECKED : 0), kToggleVsync,
                L"VSync\tV");
    AppendMenuW(video, MF_STRING | (fullscreen_ ? MF_CHECKED : 0), kToggleFullscreen,
                L"Borderless fullscreen\tF11");
    AppendMenuW(video, MF_STRING | (renderer_.OverlayEnabled() ? MF_CHECKED : 0), kToggleOverlay,
                L"Debug overlay\tO");
    AppendMenuW(video, MF_STRING | (renderer_.DownscaleAa() ? MF_CHECKED : 0),
                kToggleDownscaleAa, L"Sharp bicubic window scaling\tA");

    HMENU chroma = CreatePopupMenu();
    for (int i = 0; i < 6; ++i) {
        const auto mode = static_cast<ChromaMode>(i);
        AppendMenuW(chroma, MF_STRING | (renderer_.GetChromaMode() == mode ? MF_CHECKED : 0),
                    kChromaBase + i, ChromaModeName(mode));
    }
    AppendMenuW(chroma, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(chroma, MF_STRING, kOffsetMinus, L"Offset -0.05 px\tLeft");
    AppendMenuW(chroma, MF_STRING, kOffsetPlus, L"Offset +0.05 px\tRight");
    AppendMenuW(chroma, MF_SEPARATOR, 0, nullptr);
    std::wostringstream thresholdLabel;
    thresholdLabel.setf(std::ios::fixed);
    thresholdLabel.precision(2);
    thresholdLabel << L"Edge threshold: " << renderer_.EdgeThreshold();
    AppendMenuW(chroma, MF_STRING | MF_GRAYED, 0, thresholdLabel.str().c_str());
    AppendMenuW(chroma, MF_STRING, kThresholdMinus, L"Threshold -0.01\tDown");
    AppendMenuW(chroma, MF_STRING, kThresholdPlus, L"Threshold +0.01\tUp");
    AppendMenuW(chroma, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(chroma, MF_STRING | (renderer_.SplitScreen() ? MF_CHECKED : 0), kToggleSplit,
                L"Split: Bilinear | selected\tS");

    HMENU range = CreatePopupMenu();
    AppendMenuW(range, MF_STRING | (renderer_.LimitedRange() ? MF_CHECKED : 0),
                kRangeLimited, L"Limited (Rec.709)");
    AppendMenuW(range, MF_STRING | (!renderer_.LimitedRange() ? MF_CHECKED : 0),
                kRangeFull, L"Full (Rec.709)");

    HMENU audio = CreatePopupMenu();
    HMENU inputs = CreatePopupMenu();
    for (size_t i = 0; i < audioInputs_.size(); ++i) {
        AppendMenuW(inputs, MF_STRING | (i == audioInputIndex_ ? MF_CHECKED : 0),
                    kAudioInputBase + static_cast<UINT>(i), audioInputs_[i].name.c_str());
    }
    HMENU outputs = CreatePopupMenu();
    for (size_t i = 0; i < audioOutputs_.size(); ++i) {
        AppendMenuW(outputs, MF_STRING | (i == audioOutputIndex_ ? MF_CHECKED : 0),
                    kAudioOutputBase + static_cast<UINT>(i), audioOutputs_[i].name.c_str());
    }
    AppendMenuW(audio, MF_POPUP, reinterpret_cast<UINT_PTR>(inputs), L"HDMI input");
    AppendMenuW(audio, MF_POPUP, reinterpret_cast<UINT_PTR>(outputs), L"Output");
    AppendMenuW(audio, MF_SEPARATOR, 0, nullptr);
    const int volumePercent = static_cast<int>(audio_.Volume() * 100.0f + 0.5f);
    const std::wstring volumeLabel = L"Volume: " + std::to_wstring(volumePercent) + L"%";
    AppendMenuW(audio, MF_STRING | MF_GRAYED, 0, volumeLabel.c_str());
    AppendMenuW(audio, MF_STRING, kAudioVolumeUp, L"Volume up\t+");
    AppendMenuW(audio, MF_STRING, kAudioVolumeDown, L"Volume down\t-");
    AppendMenuW(audio, MF_STRING | (audio_.Muted() ? MF_CHECKED : 0), kAudioMute,
                L"Mute\tM");

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(video), L"Video");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(chroma), L"Chroma");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(range), L"Range");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(audio), L"Audio");
    AppendMenuW(menu, MF_STRING, kOpenLogFolder, L"Open logs");
    return menu;
}

void App::RebuildMenu() {
    HMENU old = GetMenu(window_);
    if (fullscreen_) {
        if (old) SetMenu(window_, nullptr);
    } else {
        SetMenu(window_, BuildMenu(false));
    }
    if (old) DestroyMenu(old);

    HMENU oldContext = contextMenu_;
    contextMenu_ = BuildMenu(true);
    if (oldContext) DestroyMenu(oldContext);
    DrawMenuBar(window_);
}

void App::StartBestVideo() {
    if (videoDeviceIndex_ >= videoDevices_.size()) {
        capture_.Stop();
        renderer_.ClearSource();
        captureState_ = L"No Media Foundation video capture devices found";
        DiagnosticLog(captureState_);
        RebuildMenu();
        return;
    }
    const size_t mode = FindBestMode(videoDevices_[videoDeviceIndex_]);
    if (mode == videoDevices_[videoDeviceIndex_].modes.size()) {
        capture_.Stop();
        renderer_.ClearSource();
        currentFormat_ = L"No native YUY2 mode on selected device";
        captureState_ = currentFormat_;
        DiagnosticLog(captureState_);
        RebuildMenu();
        return;
    }
    StartVideo(mode);
}

void App::StartVideo(size_t modeIndex) {
    if (videoDeviceIndex_ >= videoDevices_.size() ||
        modeIndex >= videoDevices_[videoDeviceIndex_].modes.size()) return;
    const auto& mode = videoDevices_[videoDeviceIndex_].modes[modeIndex];
    if (!mode.IsYuy2()) return;
    renderer_.ClearSource();
    captureState_ = L"Opening " + videoDevices_[videoDeviceIndex_].name;
    DiagnosticLog(captureState_ + L" / " + mode.Label());
    const HRESULT hr = capture_.Start(videoDevices_[videoDeviceIndex_], mode);
    if (FAILED(hr)) {
        currentFormat_ = L"Capture start failed: " + HrText(hr);
        captureState_ = capture_.LastError().empty() ? currentFormat_ : capture_.LastError();
        DiagnosticLog(captureState_);
        MessageBoxW(window_, currentFormat_.c_str(), L"YUVision", MB_OK | MB_ICONERROR);
        return;
    }
    videoModeIndex_ = modeIndex;
    currentFormat_ = mode.Label();
    captureState_ = L"Capture started; waiting for first frame";
    RebuildMenu();
}

void App::StartAudio() {
    if (audioInputIndex_ >= audioInputs_.size() || audioOutputIndex_ >= audioOutputs_.size()) return;
    const HRESULT hr = audio_.Start(audioInputs_[audioInputIndex_], audioOutputs_[audioOutputIndex_]);
    if (FAILED(hr)) {
        std::wstring message = L"Could not start 48 kHz stereo WASAPI audio. Select another "
                               L"input or output endpoint.";
        const std::wstring detail = audio_.Stats().error;
        if (!detail.empty()) message += L"\n\n" + detail;
        MessageBoxW(window_, message.c_str(), L"YUVision audio", MB_OK | MB_ICONWARNING);
    }
}

void App::AdjustAudioVolume(float delta) {
    audio_.SetVolume(audio_.Volume() + delta);
    if (audio_.Muted() && delta > 0.0f) audio_.SetMuted(false);
    renderer_.ShowVolumeOverlay(audio_.Volume(), audio_.Muted());
    RebuildMenu();
}

void App::ToggleAudioMute() {
    audio_.SetMuted(!audio_.Muted());
    renderer_.ShowVolumeOverlay(audio_.Volume(), audio_.Muted());
    RebuildMenu();
}

void App::HandleCommand(UINT command) {
    if (command >= kDeviceBase && command < kDeviceBase + videoDevices_.size()) {
        videoDeviceIndex_ = command - kDeviceBase;
        StartBestVideo();
    } else if (command >= kModeBase && videoDeviceIndex_ < videoDevices_.size() &&
               command < kModeBase + videoDevices_[videoDeviceIndex_].modes.size()) {
        StartVideo(command - kModeBase);
    } else if (command >= kChromaBase && command < kChromaBase + 6) {
        renderer_.SetChromaMode(static_cast<ChromaMode>(command - kChromaBase));
        RebuildMenu();
    } else if (command == kRangeLimited || command == kRangeFull) {
        renderer_.SetLimitedRange(command == kRangeLimited);
        RebuildMenu();
    } else if (command == kToggleVsync) {
        renderer_.SetVsync(!renderer_.Vsync());
        RebuildMenu();
    } else if (command == kToggleFullscreen) {
        ToggleFullscreen();
    } else if (command == kToggleOverlay) {
        renderer_.SetOverlay(!renderer_.OverlayEnabled());
        RebuildMenu();
    } else if (command == kToggleDownscaleAa) {
        renderer_.SetDownscaleAa(!renderer_.DownscaleAa());
        RebuildMenu();
    } else if (command == kOffsetMinus || command == kOffsetPlus) {
        renderer_.SetChromaOffset(renderer_.ChromaOffset() +
                                  (command == kOffsetPlus ? 0.05f : -0.05f));
    } else if (command == kThresholdMinus || command == kThresholdPlus) {
        renderer_.SetEdgeThreshold(renderer_.EdgeThreshold() +
                                   (command == kThresholdPlus ? 0.01f : -0.01f));
        RebuildMenu();
    } else if (command == kToggleSplit) {
        renderer_.SetSplitScreen(!renderer_.SplitScreen());
        RebuildMenu();
    } else if (command >= kAudioInputBase && command < kAudioInputBase + audioInputs_.size()) {
        audioInputIndex_ = command - kAudioInputBase;
        StartAudio();
        RebuildMenu();
    } else if (command >= kAudioOutputBase && command < kAudioOutputBase + audioOutputs_.size()) {
        audioOutputIndex_ = command - kAudioOutputBase;
        StartAudio();
        RebuildMenu();
    } else if (command == kAudioVolumeDown) {
        AdjustAudioVolume(-0.05f);
    } else if (command == kAudioVolumeUp) {
        AdjustAudioVolume(0.05f);
    } else if (command == kAudioMute) {
        ToggleAudioMute();
    } else if (command == kOpenLogFolder) {
        const auto directory = DiagnosticLogDirectory();
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(window_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            MessageBoxW(window_, L"Could not open the YUVision log folder.", L"YUVision logs",
                        MB_OK | MB_ICONWARNING);
        }
    }
}

void App::HandleKey(UINT key, bool shift) {
    switch (key) {
    case VK_F11: ToggleFullscreen(); break;
    case VK_RETURN:
        if (GetKeyState(VK_MENU) & 0x8000) ToggleFullscreen();
        break;
    case 'V': HandleCommand(kToggleVsync); break;
    case 'O': HandleCommand(kToggleOverlay); break;
    case 'A': HandleCommand(kToggleDownscaleAa); break;
    case 'R': renderer_.SetLimitedRange(!renderer_.LimitedRange()); RebuildMenu(); break;
    case '1': renderer_.SetChromaMode(ChromaMode::Nearest); RebuildMenu(); break;
    case '2': renderer_.SetChromaMode(ChromaMode::Bilinear); RebuildMenu(); break;
    case '3': renderer_.SetChromaMode(ChromaMode::Bicubic); RebuildMenu(); break;
    case '4': renderer_.SetChromaMode(ChromaMode::LumaGuided); RebuildMenu(); break;
    case '5': renderer_.SetChromaMode(ChromaMode::Conservative); RebuildMenu(); break;
    case '6': renderer_.SetChromaMode(ChromaMode::AdaptiveBlend); RebuildMenu(); break;
    case 'S': HandleCommand(kToggleSplit); break;
    case 'M':
    case VK_VOLUME_MUTE: ToggleAudioMute(); break;
    case VK_OEM_PLUS:
    case VK_ADD:
    case VK_VOLUME_UP: AdjustAudioVolume(0.05f); break;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
    case VK_VOLUME_DOWN: AdjustAudioVolume(-0.05f); break;
    case VK_DOWN:
        renderer_.SetEdgeThreshold(renderer_.EdgeThreshold() - (shift ? 0.025f : 0.01f));
        RebuildMenu();
        break;
    case VK_UP:
        renderer_.SetEdgeThreshold(renderer_.EdgeThreshold() + (shift ? 0.025f : 0.01f));
        RebuildMenu();
        break;
    case VK_LEFT:
        renderer_.SetChromaOffset(renderer_.ChromaOffset() - (shift ? 0.25f : 0.05f));
        break;
    case VK_RIGHT:
        renderer_.SetChromaOffset(renderer_.ChromaOffset() + (shift ? 0.25f : 0.05f));
        break;
    }
}

void App::ToggleFullscreen() {
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        windowStyle_ = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
        GetWindowPlacement(window_, &windowPlacement_);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        HMENU oldMenu = GetMenu(window_);
        SetMenu(window_, nullptr);
        if (oldMenu) DestroyMenu(oldMenu);
        SetWindowLongPtrW(window_, GWL_STYLE, windowStyle_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(window_, GWL_STYLE, windowStyle_);
        SetWindowPlacement(window_, &windowPlacement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER);
        RebuildMenu();
    }
}

std::wstring App::OverlayText(const CapturedFrame* frame) const {
    const auto captureStats = capture_.Stats();
    const auto audioStats = audio_.Stats();
    std::wostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    if (!frame) out << L"NO VIDEO FRAME\n" << captureState_ << L"\n";
    const std::wstring captureError = capture_.LastError();
    if (!captureError.empty()) out << L"Error: " << captureError << L"\n";
    out << L"Capture " << captureStats.fps << L" fps   Render " << renderer_.RenderFps()
        << L" fps   Dropped " << captureStats.dropped << L"   Queue " << captureStats.queueDepth
        << L"   Received " << captureStats.received;
    if (frame) {
        const double arrivalAgeMs = QpcSeconds(QpcNow() - frame->arrivalQpc) * 1000.0;
        const double timestampAgeMs = QpcSeconds(QpcNow() - frame->estimatedCaptureQpc) * 1000.0;
        out << L"\nFrame arrival age " << std::max(0.0, arrivalAgeMs)
            << L" ms   Timestamp age " << std::max(0.0, timestampAgeMs) << L" ms";
    }
    out << L"\n" << currentFormat_ << L"   Rec.709 "
        << (renderer_.LimitedRange() ? L"Limited" : L"Full")
        << L"   " << ChromaModeName(renderer_.GetChromaMode())
        << L"   offset " << renderer_.ChromaOffset() << L" px"
        << L"   edge threshold " << renderer_.EdgeThreshold()
        << (renderer_.SplitScreen() ? L"   SPLIT" : L"")
        << L"\nVSync " << (renderer_.Vsync() ? L"On" : L"Off")
        << L"   Sharp scale " << (renderer_.DownscaleAa() ? L"On" : L"Off")
        << L"   Tearing " << (renderer_.TearingSupported() ? L"available" : L"unavailable")
        << L"   Audio " << (audioStats.running ? L"48 kHz" : L"off")
        << L"   volume " << static_cast<int>(audio_.Volume() * 100.0f + 0.5f) << L"%"
        << (audio_.Muted() ? L" muted" : L"")
        << L"   audio queue " << audioStats.bufferedFrames << L" frames";
    if (!audioStats.error.empty()) out << L"\nAudio error: " << audioStats.error;
    out << L"\nLog: " << DiagnosticLogPath();
    return out.str();
}

LRESULT App::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        HandleCommand(LOWORD(wparam));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        HandleKey(static_cast<UINT>(wparam), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 0;
    case WM_CONTEXTMENU:
        if (contextMenu_) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (point.x == -1 && point.y == -1) GetCursorPos(&point);
            TrackPopupMenu(contextMenu_, TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        wheelDeltaRemainder_ += GET_WHEEL_DELTA_WPARAM(wparam);
        const int steps = wheelDeltaRemainder_ / WHEEL_DELTA;
        wheelDeltaRemainder_ %= WHEEL_DELTA;
        if (steps != 0) AdjustAudioVolume(static_cast<float>(steps) * 0.05f);
        return 0;
    }
    case WM_APPCOMMAND:
        switch (GET_APPCOMMAND_LPARAM(lparam)) {
        case APPCOMMAND_VOLUME_MUTE: ToggleAudioMute(); return TRUE;
        case APPCOMMAND_VOLUME_UP: AdjustAudioVolume(0.05f); return TRUE;
        case APPCOMMAND_VOLUME_DOWN: AdjustAudioVolume(-0.05f); return TRUE;
        default: break;
        }
        break;
    case WM_SIZE:
        if (rendererReady_ && wparam != SIZE_MINIMIZED) {
            try {
                renderer_.Resize(LOWORD(lparam), HIWORD(lparam));
            } catch (const std::exception& error) {
                const std::wstring detail(error.what(), error.what() + strlen(error.what()));
                DiagnosticLog(L"Resize error: " + detail);
                MessageBoxW(window_, detail.c_str(), L"YUVision resize error",
                            MB_OK | MB_ICONERROR);
            }
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int App::Run(HINSTANCE instance, int show) {
    instance_ = instance;
    ResetDiagnosticLog();
    DiagnosticLog(L"YUVision starting");
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = StaticWindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"YUVisionWindow";
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    RegisterClassExW(&windowClass);

    RECT rect{0, 0, 1280, 720};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, TRUE);
    window_ = CreateWindowExW(0, windowClass.lpszClassName, L"YUVision",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                              instance_, this);
    if (!window_) return 1;
    renderer_.Initialize(window_);
    rendererReady_ = true;
    EnumerateDevices();
    RebuildMenu();
    ShowWindow(window_, show);
    UpdateWindow(window_);
    StartBestVideo();
    if (!audioInputs_.empty() && !audioOutputs_.empty()) StartAudio();

    bool framePending = false;
    bool quit = false;
    CapturedFrame frame;
    int64_t lastStatusRenderQpc = 0;
    while (!quit) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (quit) break;

        HANDLE handles[]{capture_.FrameEvent(), renderer_.FrameLatencyEvent()};
        const DWORD handleCount = framePending ? 2 : 1;
        const DWORD wait = MsgWaitForMultipleObjectsEx(handleCount, handles,
                                                       framePending ? 16 : 250, QS_ALLINPUT,
                                                       MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) framePending = true;
        const int64_t now = QpcNow();
        const bool statusDue = QpcSeconds(now - lastStatusRenderQpc) >= 0.25 &&
                               (renderer_.SourceWidth() == 0 || !capture_.LastError().empty());
        const bool swapchainReady = (handleCount == 2 && wait == WAIT_OBJECT_0 + 1) ||
            ((framePending || statusDue) &&
             WaitForSingleObject(renderer_.FrameLatencyEvent(), 0) == WAIT_OBJECT_0);
        if (framePending && swapchainReady) {
            if (capture_.TakeLatest(frame)) {
                framePending = false;
                captureState_ = L"Streaming";
                try {
                    if (renderer_.Upload(frame)) {
                        const HRESULT presentHr = renderer_.Render(OverlayText(&frame));
                        if (frame.serial == 1 && SUCCEEDED(presentHr)) {
                            DiagnosticLog(L"First frame uploaded and presented successfully");
                        }
                    }
                } catch (const std::exception& error) {
                    std::wstring text(error.what(), error.what() + strlen(error.what()));
                    MessageBoxW(window_, text.c_str(), L"Render error", MB_OK | MB_ICONERROR);
                    quit = true;
                }
            }
        } else if (statusDue && swapchainReady) {
            try {
                renderer_.Render(OverlayText(nullptr));
                lastStatusRenderQpc = now;
            } catch (const std::exception& error) {
                DiagnosticLog(L"Status render failed: " +
                              std::wstring(error.what(), error.what() + strlen(error.what())));
                quit = true;
            }
        }
    }
    audio_.Stop();
    capture_.Stop();
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        CheckHr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx");
        CheckHr(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
        App app;
        gApp = &app;
        const int result = app.Run(instance, show);
        MFShutdown();
        CoUninitialize();
        return result;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "YUVision fatal error", MB_OK | MB_ICONERROR);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
}
