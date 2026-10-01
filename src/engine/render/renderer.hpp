#pragma once

#include "engine/capture/video_capture.hpp"
#include "engine/common/common.hpp"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_6.h>

#include <string>

enum class ChromaMode : int {
    Nearest = 0,
    Bilinear = 1,
    Bicubic = 2,
    LumaGuided = 3,
    Conservative = 4,
    AdaptiveBlend = 5,
};

class Renderer {
public:
    ~Renderer();
    void Initialize(HWND window);
    void Resize(UINT width, UINT height);
    void ClearSource();
    bool Upload(const CapturedFrame& frame);
    HRESULT Render(const std::wstring& overlay);

    void SetVsync(bool value) { vsync_ = value; }
    bool Vsync() const { return vsync_; }
    bool TearingSupported() const { return tearingSupported_; }
    void SetOverlay(bool value) { overlayEnabled_ = value; }
    bool OverlayEnabled() const { return overlayEnabled_; }
    void SetLimitedRange(bool value) { limitedRange_ = value; }
    bool LimitedRange() const { return limitedRange_; }
    void SetChromaMode(ChromaMode value) { chromaMode_ = value; }
    ChromaMode GetChromaMode() const { return chromaMode_; }
    void SetChromaOffset(float value);
    float ChromaOffset() const { return chromaOffset_; }
    void SetEdgeThreshold(float value);
    float EdgeThreshold() const { return edgeThreshold_; }
    void SetSplitScreen(bool value) { splitScreen_ = value; }
    bool SplitScreen() const { return splitScreen_; }
    void SetDownscaleAa(bool value) { downscaleAa_ = value; }
    bool DownscaleAa() const { return downscaleAa_; }
    void ShowVolumeOverlay(float volume, bool muted);
    HANDLE FrameLatencyEvent() const { return frameLatencyEvent_; }
    uint64_t PresentedFrames() const { return presentedFrames_; }
    double RenderFps() const { return renderFps_; }
    UINT SourceWidth() const { return sourceWidth_; }
    UINT SourceHeight() const { return sourceHeight_; }

private:
    void CreateSwapchainResources();
    void ReleaseSwapchainResources();
    void CreatePipeline();
    void DrawOverlay(const std::wstring& text);
    void DrawVolumeOverlay();

    HWND window_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain2> swapchain_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11Texture2D> sourceTexture_;
    ComPtr<ID3D11ShaderResourceView> sourceView_;
    ComPtr<ID3D11Texture2D> convertedTexture_;
    ComPtr<ID3D11RenderTargetView> convertedTarget_;
    ComPtr<ID3D11ShaderResourceView> convertedView_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> conversionPixelShader_;
    ComPtr<ID3D11PixelShader> scalingPixelShader_;
    ComPtr<ID3D11Buffer> constants_;

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<ID2D1Bitmap1> d2dTarget_;
    ComPtr<ID2D1SolidColorBrush> overlayBrush_;
    ComPtr<ID2D1SolidColorBrush> shadowBrush_;
    ComPtr<ID2D1SolidColorBrush> volumeTrackBrush_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IDWriteTextFormat> volumeTextFormat_;

    HANDLE frameLatencyEvent_ = nullptr;
    UINT outputWidth_ = 1;
    UINT outputHeight_ = 1;
    UINT sourceWidth_ = 0;
    UINT sourceHeight_ = 0;
    bool vsync_ = true;
    bool tearingSupported_ = false;
    bool overlayEnabled_ = true;
    bool limitedRange_ = true;
    ChromaMode chromaMode_ = ChromaMode::LumaGuided;
    float chromaOffset_ = 0.0f;
    float edgeThreshold_ = 0.04f;
    bool splitScreen_ = false;
    bool downscaleAa_ = true;
    float volumeOverlayLevel_ = 1.0f;
    bool volumeOverlayMuted_ = false;
    double volumeOverlayUntil_ = 0.0;
    uint64_t presentedFrames_ = 0;
    double renderFps_ = 0.0;
    uint32_t fpsWindowFrames_ = 0;
    int64_t fpsWindowQpc_ = 0;
};

const wchar_t* ChromaModeName(ChromaMode mode);
