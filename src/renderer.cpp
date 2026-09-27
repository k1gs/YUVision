#include "renderer.hpp"

#include <d3dcompiler.h>
#include <algorithm>
#include <array>

namespace {

constexpr char kShader[] = R"hlsl(
Texture2D<float4> packedYuy2 : register(t0);
Texture2D<float4> convertedRgb : register(t1);

cbuffer Settings : register(b0) {
    float2 sourceSize;
    float2 outputSize;
    float chromaOffset;
    float edgeThreshold;
    int chromaMode;
    int limitedRange;
    int splitScreen;
    int downscaleAa;
    float2 padding;
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

float2 BicubicChroma(int base, int y, float f) {
    float2 sum = 0;
    float weightSum = 0;
    [unroll] for (int i = -1; i <= 2; ++i) {
        float w = CubicWeight((float)i - f);
        sum += ChromaAt(base + i, y) * w;
        weightSum += w;
    }
    float2 result = sum / max(weightSum, 1e-5);
    float2 lo = min(ChromaAt(base, y), ChromaAt(base + 1, y));
    float2 hi = max(ChromaAt(base, y), ChromaAt(base + 1, y));
    return clamp(result, lo, hi); // Avoid saturated UI ringing.
}

float EdgeConfidence(int x, int y, int base, float targetY, out float2 edgeCandidate) {
    float centerLeft = abs(targetY - LumaAt(x - 1, y));
    float centerRight = abs(LumaAt(x + 1, y) - targetY);
    float primaryGradient = max(centerLeft, centerRight);
    float competingGradient = min(centerLeft, centerRight);
    float outerGradient = max(abs(LumaAt(x - 1, y) - LumaAt(x - 2, y)),
                              abs(LumaAt(x + 2, y) - LumaAt(x + 1, y)));

    // Two candidates only: never average chroma across a larger radius at an edge.
    int leftReferenceX = (int)floor(base * 2.0 - chromaOffset + 0.5);
    int rightReferenceX = (int)floor((base + 1) * 2.0 - chromaOffset + 0.5);
    float leftMatch = abs(targetY - LumaAt(leftReferenceX, y));
    float rightMatch = abs(targetY - LumaAt(rightReferenceX, y));
    edgeCandidate = leftMatch <= rightMatch ? ChromaAt(base, y) : ChromaAt(base + 1, y);

    float strength = smoothstep(edgeThreshold, edgeThreshold * 2.5, primaryGradient);
    float complexity = max(competingGradient, outerGradient);
    float isolation = 1.0 - saturate(complexity / max(primaryGradient, 1e-5));
    float matchConfidence = smoothstep(0.0, edgeThreshold,
                                       abs(leftMatch - rightMatch));
    float candidateContrast = abs(LumaAt(leftReferenceX, y) - LumaAt(rightReferenceX, y));
    float edgeAgreement = smoothstep(edgeThreshold * 0.5, edgeThreshold * 2.0,
                                     candidateContrast);
    return saturate(strength * isolation * matchConfidence * (0.35 + 0.65 * edgeAgreement));
}

float2 ReconstructChroma(float sourceX, int y, float targetY, int mode) {
    // 4:2:2 nominal co-sited phase: pair k's chroma is at luma x=2k.
    // chromaOffset is expressed in full-resolution luma pixels.
    float c = (sourceX + chromaOffset) * 0.5;
    if (mode == 0) return ChromaAt((int)floor(c + 0.5), y);

    int base = (int)floor(c);
    float f = frac(c);
    float2 bilinear = lerp(ChromaAt(base, y), ChromaAt(base + 1, y), f);
    if (mode == 1) return bilinear;
    if (mode == 2) return BicubicChroma(base, y, f);

    if (mode == 3) {
        // Original pre-adaptive algorithm retained exactly as the default quality mode.
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

    float2 bicubic = BicubicChroma(base, y, f);
    int x = (int)floor(sourceX + 0.5);
    float2 edgeCandidate;
    float confidence = EdgeConfidence(x, y, base, targetY, edgeCandidate);
    if (mode == 5) {
        // Explicit adaptive mode: fine/ambiguous detail falls back to bilinear.
        return lerp(bilinear, edgeCandidate, confidence);
    }

    // Conservative luma-guided mode: bicubic is used only as mild smooth-area support;
    // a high-confidence isolated edge switches to almost-nearest chroma.
    float2 smoothChroma = lerp(bilinear, bicubic, 0.25);
    float hardEdge = smoothstep(0.45, 0.8, confidence);
    float2 minimallyMixedEdge = lerp(edgeCandidate, bilinear, 0.08);
    return lerp(smoothChroma, minimallyMixedEdge, hardEdge);
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

float3 SampleSource(float2 pixel, int mode) {
    int2 ip = int2(clamp(floor(pixel + 0.5), 0.0, sourceSize - 1.0));
    float y = LumaAt(ip.x, ip.y);
    float2 uv = ReconstructChroma(pixel.x, ip.y, y, mode);
    return saturate(ToRgb(y, uv));
}

float4 PSConvert(VsOut input) : SV_Target {
    float2 pixel = input.uv * sourceSize - 0.5;
    int activeMode = (splitScreen != 0 && input.uv.x < 0.5) ? 1 : chromaMode;
    return float4(SampleSource(pixel, activeMode), 1.0);
}

float4 CatmullRomWeights(float f) {
    float f2 = f * f;
    float f3 = f2 * f;
    return float4(-0.5*f + f2 - 0.5*f3,
                  1.0 - 2.5*f2 + 1.5*f3,
                  0.5*f + 2.0*f2 - 1.5*f3,
                  -0.5*f2 + 0.5*f3);
}

float3 SampleConverted(int2 pixel) {
    pixel = clamp(pixel, int2(0, 0), int2(sourceSize) - 1);
    return convertedRgb.Load(int3(pixel, 0)).rgb;
}

float3 SampleConvertedBicubic(float2 pixel) {
    int2 base = int2(floor(pixel));
    float4 wx = CatmullRomWeights(frac(pixel.x));
    float4 wy = CatmullRomWeights(frac(pixel.y));
    float3 result = 0;
    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int i = 0; i < 4; ++i) {
            result += SampleConverted(base + int2(i - 1, j - 1)) * wx[i] * wy[j];
        }
    }

    // Catmull-Rom is intentionally sharp, but clamp its overshoot to the central
    // 2x2 neighbourhood so saturated UI edges do not acquire ringing halos.
    float3 lo = min(min(SampleConverted(base), SampleConverted(base + int2(1, 0))),
                    min(SampleConverted(base + int2(0, 1)),
                        SampleConverted(base + int2(1, 1))));
    float3 hi = max(max(SampleConverted(base), SampleConverted(base + int2(1, 0))),
                    max(SampleConverted(base + int2(0, 1)),
                        SampleConverted(base + int2(1, 1))));
    return clamp(result, lo, hi);
}

float4 PSScale(VsOut input) : SV_Target {
    float sourceAspect = sourceSize.x / sourceSize.y;
    float outputAspect = outputSize.x / outputSize.y;
    float2 scale = 1.0;
    if (outputAspect > sourceAspect) scale.x = sourceAspect / outputAspect;
    else scale.y = outputAspect / sourceAspect;
    float2 centered = (input.uv - 0.5) / scale + 0.5;
    if (any(centered < 0.0) || any(centered > 1.0)) return float4(0, 0, 0, 1);

    float2 pixel = centered * sourceSize - 0.5;
    float2 contentOutputSize = max(outputSize * scale, 1.0);
    float2 sourcePerOutput = sourceSize / contentOutputSize;
    float3 rgb;
    if (downscaleAa != 0 && any(sourcePerOutput > 1.001)) {
        rgb = SampleConvertedBicubic(pixel);
    } else {
        rgb = SampleConverted(int2(floor(pixel + 0.5)));
    }
    if (splitScreen != 0 && abs(input.uv.x - 0.5) < 1.25 / outputSize.x) {
        rgb = float3(0.85, 0.85, 0.85);
    }
    return float4(rgb, 1.0);
}
)hlsl";

struct alignas(16) ShaderSettings {
    float sourceSize[2];
    float outputSize[2];
    float chromaOffset;
    float edgeThreshold;
    int chromaMode;
    int limitedRange;
    int splitScreen;
    int downscaleAa;
    float padding[2];
};

static_assert(sizeof(ShaderSettings) == 48);

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
    case ChromaMode::LumaGuided: return L"Luma-guided (original)";
    case ChromaMode::Conservative: return L"Luma-guided conservative";
    case ChromaMode::AdaptiveBlend: return L"Adaptive blend";
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
    const auto conversionPs = Compile("PSConvert", "ps_5_0");
    const auto scalingPs = Compile("PSScale", "ps_5_0");
    CheckHr(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                        &vertexShader_), "CreateVertexShader");
    CheckHr(device_->CreatePixelShader(conversionPs->GetBufferPointer(),
                                       conversionPs->GetBufferSize(), nullptr,
                                       &conversionPixelShader_),
            "CreatePixelShader(conversion)");
    CheckHr(device_->CreatePixelShader(scalingPs->GetBufferPointer(),
                                       scalingPs->GetBufferSize(), nullptr,
                                       &scalingPixelShader_),
            "CreatePixelShader(scaling)");

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
        convertedTarget_.Reset();
        convertedView_.Reset();
        convertedTexture_.Reset();
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

        desc.Width = frame.width;
        desc.Height = frame.height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        CheckHr(device_->CreateTexture2D(&desc, nullptr, &convertedTexture_),
                "CreateTexture2D(converted RGB)");
        CheckHr(device_->CreateRenderTargetView(convertedTexture_.Get(), nullptr,
                                                 &convertedTarget_),
                "CreateRenderTargetView(converted RGB)");
        CheckHr(device_->CreateShaderResourceView(convertedTexture_.Get(), nullptr,
                                                  &convertedView_),
                "CreateShaderResourceView(converted RGB)");
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

    const D3D11_VIEWPORT outputViewport{0, 0, static_cast<float>(outputWidth_),
                                        static_cast<float>(outputHeight_), 0, 1};
    ID3D11RenderTargetView* outputTarget = renderTarget_.Get();
    context_->RSSetViewports(1, &outputViewport);
    context_->OMSetRenderTargets(1, &outputTarget, nullptr);
    const float black[4]{0, 0, 0, 1};
    context_->ClearRenderTargetView(renderTarget_.Get(), black);
    if (sourceView_ && convertedTarget_ && convertedView_) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHr(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                "Map(constants)");
        auto* settings = static_cast<ShaderSettings*>(mapped.pData);
        settings->sourceSize[0] = static_cast<float>(sourceWidth_);
        settings->sourceSize[1] = static_cast<float>(sourceHeight_);
        settings->outputSize[0] = static_cast<float>(outputWidth_);
        settings->outputSize[1] = static_cast<float>(outputHeight_);
        settings->chromaOffset = chromaOffset_;
        settings->edgeThreshold = edgeThreshold_;
        settings->chromaMode = static_cast<int>(chromaMode_);
        settings->limitedRange = limitedRange_ ? 1 : 0;
        settings->splitScreen = splitScreen_ ? 1 : 0;
        settings->downscaleAa = downscaleAa_ ? 1 : 0;
        settings->padding[0] = settings->padding[1] = 0;
        context_->Unmap(constants_.Get(), 0);

        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        ID3D11Buffer* cb = constants_.Get();
        context_->PSSetConstantBuffers(0, 1, &cb);

        // Pass 1: reconstruct chroma and convert YUY2 at the source's native grid.
        ID3D11ShaderResourceView* nullViews[2]{};
        context_->PSSetShaderResources(0, 2, nullViews);
        const D3D11_VIEWPORT sourceViewport{0, 0, static_cast<float>(sourceWidth_),
                                            static_cast<float>(sourceHeight_), 0, 1};
        context_->RSSetViewports(1, &sourceViewport);
        ID3D11RenderTargetView* conversionTarget = convertedTarget_.Get();
        context_->OMSetRenderTargets(1, &conversionTarget, nullptr);
        context_->PSSetShader(conversionPixelShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* packedView = sourceView_.Get();
        context_->PSSetShaderResources(0, 1, &packedView);
        context_->Draw(3, 0);

        // Pass 2: scale the completed RGB frame to the window in the same GPU frame.
        context_->RSSetViewports(1, &outputViewport);
        context_->OMSetRenderTargets(1, &outputTarget, nullptr);
        context_->PSSetShader(scalingPixelShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* rgbView = convertedView_.Get();
        context_->PSSetShaderResources(1, 1, &rgbView);
        context_->Draw(3, 0);
        context_->PSSetShaderResources(0, 2, nullViews);
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

void Renderer::SetEdgeThreshold(float value) {
    edgeThreshold_ = std::clamp(value, 0.01f, 0.20f);
}
