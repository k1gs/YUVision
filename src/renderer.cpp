#include "renderer.hpp"

#include <d3dcompiler.h>
#include <algorithm>
#include <array>

namespace {

constexpr char kShader[] = R"hlsl(
Texture2D<float4> packedYuy2 : register(t0);

cbuffer Settings : register(b0) {
    float2 sourceSize;
    float2 outputSize;
    float chromaOffset;
    int chromaMode;
    int limitedRange;
    float padding;
};

struct VsOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };

VsOut VSMain(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

int PairCount() { return max(1, (int)sourceSize.x / 2); }

float4 Pair(int pairX, int y) {
    pairX = clamp(pairX, 0, PairCount() - 1);
    y = clamp(y, 0, (int)sourceSize.y - 1);
    return packedYuy2.Load(int3(pairX, y, 0));
}

float LumaAt(int x, int y) {
    x = clamp(x, 0, (int)sourceSize.x - 1);
    float4 p = Pair(x / 2, y);
    return (x & 1) ? p.b : p.r;
}

float2 ChromaAt(int pairX, int y) {
    float4 p = Pair(pairX, y);
    return p.ga;
}

float PairLuma(int pairX, int y) {
    float4 p = Pair(pairX, y);
    return 0.5 * (p.r + p.b);
}

float CubicWeight(float x) {
    x = abs(x);
    if (x < 1.0) return 1.5*x*x*x - 2.5*x*x + 1.0;
    if (x < 2.0) return -0.5*x*x*x + 2.5*x*x - 4.0*x + 2.0;
    return 0.0;
}

float2 ReconstructChroma(float sourceX, int y, float targetY) {
    // 4:2:2 nominal co-sited phase: pair k's chroma is at luma x=2k.
    // chromaOffset is expressed in full-resolution luma pixels.
    float c = (sourceX + chromaOffset) * 0.5;
    if (chromaMode == 0) return ChromaAt((int)floor(c + 0.5), y);

    int base = (int)floor(c);
    float f = frac(c);
    if (chromaMode == 1) {
        return lerp(ChromaAt(base, y), ChromaAt(base + 1, y), f);
    }
    if (chromaMode == 2) {
        float2 sum = 0;
        float weightSum = 0;
        [unroll] for (int i = -1; i <= 2; ++i) {
            float w = CubicWeight((float)i - f);
            sum += ChromaAt(base + i, y) * w;
            weightSum += w;
        }
        return sum / max(weightSum, 1e-5);
    }

    // Fixed-cost joint bilateral support. Full-resolution luma discourages chroma
    // interpolation across a strong brightness edge without introducing history.
    float2 guidedSum = 0;
    float guidedWeight = 0;
    [unroll] for (int j = -1; j <= 2; ++j) {
        int k = base + j;
        float spatial = exp2(-1.35 * abs((float)k - c));
        float lumaDifference = abs(targetY - PairLuma(k, y));
        float guide = exp2(-28.0 * lumaDifference);
        float w = spatial * (0.035 + guide);
        guidedSum += ChromaAt(k, y) * w;
        guidedWeight += w;
    }
    return guidedSum / max(guidedWeight, 1e-5);
}

float3 ToRgb(float y, float2 uv) {
    float cb, cr;
    if (limitedRange != 0) {
        y = (y - 16.0/255.0) * (255.0/219.0);
        cb = (uv.x - 128.0/255.0) * (255.0/224.0);
        cr = (uv.y - 128.0/255.0) * (255.0/224.0);
    } else {
        cb = uv.x - 0.5;
        cr = uv.y - 0.5;
    }
    return float3(y + 1.5748*cr,
                  y - 0.187324*cb - 0.468124*cr,
                  y + 1.8556*cb);
}

float4 PSMain(VsOut input) : SV_Target {
    float sourceAspect = sourceSize.x / sourceSize.y;
    float outputAspect = outputSize.x / outputSize.y;
    float2 scale = 1.0;
    if (outputAspect > sourceAspect) scale.x = sourceAspect / outputAspect;
    else scale.y = outputAspect / sourceAspect;
    float2 centered = (input.uv - 0.5) / scale + 0.5;
    if (any(centered < 0.0) || any(centered > 1.0)) return float4(0, 0, 0, 1);

    float2 pixel = centered * sourceSize - 0.5;
    int2 ip = int2(clamp(floor(pixel + 0.5), 0.0, sourceSize - 1.0));
    float y = LumaAt(ip.x, ip.y);
    float2 uv = ReconstructChroma(pixel.x, ip.y, y);
    return float4(saturate(ToRgb(y, uv)), 1.0);
}
)hlsl";

struct alignas(16) ShaderSettings {
    float sourceSize[2];
    float outputSize[2];
    float chromaOffset;
    int chromaMode;
    int limitedRange;
    float padding;
};

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "YUY2.hlsl", nullptr, nullptr,
                                  entry, target, flags, 0, &code, &errors);
    if (FAILED(hr)) {
        const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer())
                                     : "D3DCompile failed";
        throw std::runtime_error(message);
    }
    return code;
}

} // namespace

const wchar_t* ChromaModeName(ChromaMode mode) {
    switch (mode) {
    case ChromaMode::Nearest: return L"Nearest";
    case ChromaMode::Bilinear: return L"Bilinear";
    case ChromaMode::Bicubic: return L"Bicubic Catmull-Rom";
    case ChromaMode::LumaGuided: return L"Luma-guided";
    }
    return L"Unknown";
}

Renderer::~Renderer() {
    if (frameLatencyEvent_) CloseHandle(frameLatencyEvent_);
}

void Renderer::Initialize(HWND window) {
    window_ = window;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const std::array levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels.data(), static_cast<UINT>(levels.size()),
                                   D3D11_SDK_VERSION, &device_, &selected, &context_);
#if defined(_DEBUG)
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               levels.data(), static_cast<UINT>(levels.size()),
                               D3D11_SDK_VERSION, &device_, &selected, &context_);
    }
#endif
    CheckHr(hr, "D3D11CreateDevice");

    ComPtr<IDXGIDevice> dxgiDevice;
    CheckHr(device_.As(&dxgiDevice), "ID3D11Device->IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter;
    CheckHr(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIFactory2> factory;
    CheckHr(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allow, sizeof(allow)))) {
            tearingSupported_ = allow != FALSE;
        }
    }

    RECT client{};
    GetClientRect(window_, &client);
    outputWidth_ = std::max<UINT>(1, client.right - client.left);
    outputHeight_ = std::max<UINT>(1, client.bottom - client.top);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = outputWidth_;
    desc.Height = outputHeight_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearingSupported_) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> chain1;
    CheckHr(factory->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr,
                                            &chain1), "CreateSwapChainForHwnd");
    CheckHr(chain1.As(&swapchain_), "IDXGISwapChain2");
    CheckHr(swapchain_->SetMaximumFrameLatency(1), "SetMaximumFrameLatency");
    frameLatencyEvent_ = swapchain_->GetFrameLatencyWaitableObject();
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);

    CheckHr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                              IID_PPV_ARGS(&d2dFactory_)), "D2D1CreateFactory");
    CheckHr(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_), "D2D CreateDevice");
    CheckHr(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_),
            "D2D CreateDeviceContext");
    CheckHr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())),
            "DWriteCreateFactory");
    CheckHr(writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us",
                                             &textFormat_), "CreateTextFormat");
    textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    CreatePipeline();
    CreateSwapchainResources();
    fpsWindowQpc_ = QpcNow();
}

void Renderer::CreatePipeline() {
    const auto vs = Compile("VSMain", "vs_5_0");
    const auto ps = Compile("PSMain", "ps_5_0");
    CheckHr(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                        &vertexShader_), "CreateVertexShader");
    CheckHr(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr,
                                       &pixelShader_), "CreatePixelShader");

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(ShaderSettings);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    CheckHr(device_->CreateBuffer(&cb, nullptr, &constants_), "CreateBuffer(constants)");
}

void Renderer::CreateSwapchainResources() {
    ComPtr<ID3D11Texture2D> backBuffer;
    CheckHr(swapchain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "GetBuffer(backbuffer)");
    CheckHr(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_),
            "CreateRenderTargetView");

    ComPtr<IDXGISurface> surface;
    CheckHr(backBuffer.As(&surface), "Backbuffer IDXGISurface");
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    CheckHr(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &d2dTarget_),
            "CreateBitmapFromDxgiSurface");
    d2dContext_->SetTarget(d2dTarget_.Get());
    CheckHr(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.94f), &overlayBrush_),
            "Create overlay brush");
    CheckHr(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.72f), &shadowBrush_),
            "Create shadow brush");
}

void Renderer::ReleaseSwapchainResources() {
    d2dContext_->SetTarget(nullptr);
    overlayBrush_.Reset();
    shadowBrush_.Reset();
    d2dTarget_.Reset();
    renderTarget_.Reset();
    context_->Flush();
}

void Renderer::Resize(UINT width, UINT height) {
    if (!swapchain_ || width == 0 || height == 0) return;
    outputWidth_ = width;
    outputHeight_ = height;
    ReleaseSwapchainResources();
    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearingSupported_) flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    CheckHr(swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags),
            "ResizeBuffers");
    CreateSwapchainResources();
}

bool Renderer::Upload(const CapturedFrame& frame) {
    if (frame.width == 0 || frame.height == 0 || frame.bytes.empty()) return false;
    if (!sourceTexture_ || sourceWidth_ != frame.width || sourceHeight_ != frame.height) {
        sourceView_.Reset();
        sourceTexture_.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = frame.width / 2;
        desc.Height = frame.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CheckHr(device_->CreateTexture2D(&desc, nullptr, &sourceTexture_),
                "CreateTexture2D(YUY2 packed)");
        CheckHr(device_->CreateShaderResourceView(sourceTexture_.Get(), nullptr, &sourceView_),
                "CreateShaderResourceView(YUY2 packed)");
        sourceWidth_ = frame.width;
        sourceHeight_ = frame.height;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHr(context_->Map(sourceTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
            "Map(YUY2 texture)");
    const size_t sourcePitch = static_cast<size_t>(frame.width) * 2;
    for (UINT y = 0; y < frame.height; ++y) {
        memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
               frame.bytes.data() + static_cast<size_t>(y) * sourcePitch, sourcePitch);
    }
    context_->Unmap(sourceTexture_.Get(), 0);
    return true;
}

HRESULT Renderer::Render(const std::wstring& overlay) {
    if (!renderTarget_) return S_FALSE;

    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(outputWidth_),
                                  static_cast<float>(outputHeight_), 0, 1};
    context_->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* target = renderTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    const float black[4]{0, 0, 0, 1};
    context_->ClearRenderTargetView(renderTarget_.Get(), black);
    if (sourceView_) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHr(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                "Map(constants)");
        auto* settings = static_cast<ShaderSettings*>(mapped.pData);
        settings->sourceSize[0] = static_cast<float>(sourceWidth_);
        settings->sourceSize[1] = static_cast<float>(sourceHeight_);
        settings->outputSize[0] = static_cast<float>(outputWidth_);
        settings->outputSize[1] = static_cast<float>(outputHeight_);
        settings->chromaOffset = chromaOffset_;
        settings->chromaMode = static_cast<int>(chromaMode_);
        settings->limitedRange = limitedRange_ ? 1 : 0;
        settings->padding = 0;
        context_->Unmap(constants_.Get(), 0);

        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* view = sourceView_.Get();
        context_->PSSetShaderResources(0, 1, &view);
        ID3D11Buffer* cb = constants_.Get();
        context_->PSSetConstantBuffers(0, 1, &cb);
        context_->Draw(3, 0);
    }

    if (overlayEnabled_) DrawOverlay(overlay);
    const UINT flags = (!vsync_ && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT hr = swapchain_->Present(vsync_ ? 1 : 0, flags);
    if (SUCCEEDED(hr)) {
        ++presentedFrames_;
        ++fpsWindowFrames_;
        const int64_t now = QpcNow();
        const double elapsed = QpcSeconds(now - fpsWindowQpc_);
        if (elapsed >= 1.0) {
            renderFps_ = static_cast<double>(fpsWindowFrames_) / elapsed;
            fpsWindowFrames_ = 0;
            fpsWindowQpc_ = now;
        }
    }
    return hr;
}

void Renderer::DrawOverlay(const std::wstring& text) {
    d2dContext_->BeginDraw();
    const D2D1_RECT_F panel = D2D1::RectF(12, 12, 760, 212);
    d2dContext_->FillRectangle(panel, shadowBrush_.Get());
    const D2D1_RECT_F layout = D2D1::RectF(22, 18, 750, 208);
    d2dContext_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), textFormat_.Get(),
                           layout, overlayBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const HRESULT hr = d2dContext_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // Resize/recreation will rebuild it on the next window-size event.
        d2dTarget_.Reset();
    }
}

void Renderer::SetChromaOffset(float value) {
    chromaOffset_ = std::clamp(value, -1.5f, 1.5f);
}
