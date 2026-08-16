#include "d3d11_presenter.h"
#include <d3dcompiler.h>
#include <shlobj.h>
#include <cstdio>
#include <iostream>

namespace litewp {

struct ScalingData {
    float uv_scale[2];
    float uv_offset[2];
};

static const char* g_vs_source = R"(
cbuffer ScalingBuffer : register(b0) {
    float2 uv_scale;
    float2 uv_offset;
};

struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint vertexID : SV_VertexID) {
    VS_OUT o;
    float2 raw_uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.pos = float4(raw_uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = raw_uv * uv_scale + uv_offset;
    return o;
}
)";

// Pixel shader for Texture2D (staging copy path)
static const char* g_ps_tex2d_source = R"(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float y = texY.Sample(samp, uv);
    float2 uv_val = texUV.Sample(samp, uv);
    
    float u = uv_val.x - 0.5f;
    float v = uv_val.y - 0.5f;
    
    // BT.709 YUV to RGB
    float r = y + 1.5748f * v;
    float g = y - 0.1873f * u - 0.4681f * v;
    float b = y + 1.8556f * u;
    
    return float4(saturate(float3(r, g, b)), 1.0f);
}
)";

// Pixel shader for Texture2DArray (zero-copy path)
static const char* g_ps_array_source = R"(
Texture2DArray<float>  texY  : register(t0);
Texture2DArray<float2> texUV : register(t1);
SamplerState           samp  : register(s0);

cbuffer SliceBuffer : register(b1) {
    uint arraySlice;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 uvw = float3(uv, (float)arraySlice);
    float y = texY.Sample(samp, uvw);
    float2 uv_val = texUV.Sample(samp, uvw);
    
    float u = uv_val.x - 0.5f;
    float v = uv_val.y - 0.5f;
    
    // BT.709 YUV to RGB
    float r = y + 1.5748f * v;
    float g = y - 0.1873f * u - 0.4681f * v;
    float b = y + 1.8556f * u;
    
    return float4(saturate(float3(r, g, b)), 1.0f);
}
)";

struct SliceData {
    uint32_t arraySlice;
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
};

D3D11Presenter::D3D11Presenter() = default;

D3D11Presenter::~D3D11Presenter() {
    Cleanup();
}

bool D3D11Presenter::Init(HWND hwnd, int width, int height) {
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
    };

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL actualFeatureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_device,
        &actualFeatureLevel,
        &m_context
    );

    if (FAILED(hr)) {
        return false;
    }

    // Enable multithreaded protection for safe concurrent video decodes/presents
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(m_device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    if (!CreateSwapChain(hwnd, width, height)) {
        return false;
    }

    if (!CreateShaders()) {
        return false;
    }

    return true;
}

bool D3D11Presenter::CreateSwapChain(HWND hwnd, int width, int height) {
    m_rtv.Reset();
    m_swapchain.Reset();

    // Flip-model swap chain (DirectComposition) with double buffering for minimal VRAM
    {
        ComPtr<IDXGIFactory2> dxgiFactory2;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> dxgiAdapter;
        if (SUCCEEDED(m_device.As(&dxgiDevice)) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) &&
            SUCCEEDED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory2)))) {

            DXGI_SWAP_CHAIN_DESC1 sd = {};
            sd.Width = (UINT)width;
            sd.Height = (UINT)height;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.Stereo = FALSE;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.Scaling = DXGI_SCALING_STRETCH;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            sd.Flags = 0;

            ComPtr<IDXGISwapChain1> sc1;
            if (SUCCEEDED(dxgiFactory2->CreateSwapChainForHwnd(
                    m_device.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
                m_swapchain = sc1;
                dxgiFactory2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            }
        }
    }

    // Fallback: legacy blt-model swap chain (Windows 7 SP1+ compatible)
    if (!m_swapchain) {
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        scDesc.BufferCount = 2;
        scDesc.BufferDesc.Width = width;
        scDesc.BufferDesc.Height = height;
        scDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.BufferDesc.RefreshRate.Numerator = 60;
        scDesc.BufferDesc.RefreshRate.Denominator = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.OutputWindow = hwnd;
        scDesc.SampleDesc.Count = 1;
        scDesc.SampleDesc.Quality = 0;
        scDesc.Windowed = TRUE;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_device.As(&dxgiDevice))) return false;

        ComPtr<IDXGIAdapter> dxgiAdapter;
        if (FAILED(dxgiDevice->GetAdapter(&dxgiAdapter))) return false;

        ComPtr<IDXGIFactory> dxgiFactory;
        if (FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) return false;

        if (FAILED(dxgiFactory->CreateSwapChain(m_device.Get(), &scDesc, &m_swapchain))) {
            return false;
        }
        dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    }

    // Create Render Target View
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }

    if (FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv))) {
        return false;
    }

    return true;
}

bool D3D11Presenter::CreateShaders() {
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(
        g_vs_source, strlen(g_vs_source), "fullscreen_quad.hlsl",
        nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &errorBlob
    );
    if (FAILED(hr)) return false;

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_fullscreen_vs);
    if (FAILED(hr)) return false;

    // Compile Texture2D pixel shader (staging copy path)
    ComPtr<ID3DBlob> psTex2dBlob;
    hr = D3DCompile(
        g_ps_tex2d_source, strlen(g_ps_tex2d_source), "nv12_tex2d.hlsl",
        nullptr, nullptr, "main", "ps_4_0", 0, 0, &psTex2dBlob, &errorBlob
    );
    if (FAILED(hr)) return false;

    hr = m_device->CreatePixelShader(psTex2dBlob->GetBufferPointer(), psTex2dBlob->GetBufferSize(), nullptr, &m_nv12_ps);
    if (FAILED(hr)) return false;

    // Compile Texture2DArray pixel shader (zero-copy path)
    ComPtr<ID3DBlob> psArrayBlob;
    hr = D3DCompile(
        g_ps_array_source, strlen(g_ps_array_source), "nv12_array.hlsl",
        nullptr, nullptr, "main", "ps_4_0", 0, 0, &psArrayBlob, &errorBlob
    );
    if (FAILED(hr)) return false;

    hr = m_device->CreatePixelShader(psArrayBlob->GetBufferPointer(), psArrayBlob->GetBufferSize(), nullptr, &m_nv12_array_ps);
    if (FAILED(hr)) return false;

    // Create Sampler State (Linear Clamp for video scaling)
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = m_device->CreateSamplerState(&sampDesc, &m_sampler);
    if (FAILED(hr)) return false;

    // Create Constant Buffer for UV Scaling
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ScalingData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_scaling_cb);
    if (FAILED(hr)) return false;

    // Create Constant Buffer for Array Slice index
    D3D11_BUFFER_DESC sliceDesc = {};
    sliceDesc.ByteWidth = sizeof(SliceData);
    sliceDesc.Usage = D3D11_USAGE_DYNAMIC;
    sliceDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    sliceDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = m_device->CreateBuffer(&sliceDesc, nullptr, &m_slice_cb);
    return SUCCEEDED(hr);
}

void D3D11Presenter::RenderFrame(ID3D11Texture2D* nv12_texture, int array_index, int scaling_mode) {
    if (!m_context || !m_rtv || !nv12_texture) return;

    D3D11_TEXTURE2D_DESC texDesc;
    nv12_texture->GetDesc(&texDesc);

    // One-time diagnostic log
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        char buf[512];
        snprintf(buf, sizeof(buf),
            "RenderFrame diag: tex=%p W=%u H=%u ArraySize=%u Format=%u BindFlags=0x%X MiscFlags=0x%X Usage=%u idx=%d screen=%dx%d",
            nv12_texture, texDesc.Width, texDesc.Height, texDesc.ArraySize,
            texDesc.Format, texDesc.BindFlags, texDesc.MiscFlags, texDesc.Usage,
            array_index, m_width, m_height);
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
        // Also write to log file
        FILE* f = nullptr;
        char logpath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, logpath))) {
            strcat_s(logpath, "\\LiteWallpaper\\render_diag.log");
            fopen_s(&f, logpath, "a");
            if (f) { fprintf(f, "%s\n", buf); fclose(f); }
        }
    }

    bool has_srv_flag = (texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    bool use_zero_copy = false;

    if (has_srv_flag && texDesc.ArraySize > 1) {
        // === ZERO-COPY PATH ===
        // Cache SRVs: only recreate when the source texture pointer or dimensions change
        if (nv12_texture != m_zero_copy_tex || m_srv_width != texDesc.Width || m_srv_height != texDesc.Height) {
            m_srv_y.Reset();
            m_srv_uv.Reset();

            D3D11_SHADER_RESOURCE_VIEW_DESC yDesc = {};
            yDesc.Format = DXGI_FORMAT_R8_UNORM;
            yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            yDesc.Texture2DArray.FirstArraySlice = 0;
            yDesc.Texture2DArray.ArraySize = texDesc.ArraySize;
            yDesc.Texture2DArray.MipLevels = 1;

            HRESULT hr = m_device->CreateShaderResourceView(nv12_texture, &yDesc, &m_srv_y);
            if (SUCCEEDED(hr)) {
                D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc = {};
                uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
                uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                uvDesc.Texture2DArray.FirstArraySlice = 0;
                uvDesc.Texture2DArray.ArraySize = texDesc.ArraySize;
                uvDesc.Texture2DArray.MipLevels = 1;

                hr = m_device->CreateShaderResourceView(nv12_texture, &uvDesc, &m_srv_uv);
                if (SUCCEEDED(hr)) {
                    m_zero_copy_tex = nv12_texture;
                    m_srv_width = texDesc.Width;
                    m_srv_height = texDesc.Height;
                    // Clear staging texture since we don't need it
                    m_srv_texture.Reset();
                } else {
                    m_srv_y.Reset();
                    m_srv_uv.Reset();
                }
            }
        }
        use_zero_copy = (m_srv_y && m_srv_uv && m_zero_copy_tex == nv12_texture);
    }

    static bool s_logged_path = false;
    if (!s_logged_path) {
        s_logged_path = true;
        char buf[256];
        snprintf(buf, sizeof(buf), "RenderFrame path: %s (srv_y=%p srv_uv=%p)",
                 use_zero_copy ? "ZERO-COPY (Texture2DArray)" : "STAGING-COPY (Fallback Texture2D)",
                 m_srv_y.Get(), m_srv_uv.Get());
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
        FILE* f = nullptr;
        char logpath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, logpath))) {
            strcat_s(logpath, "\\LiteWallpaper\\render_diag.log");
            fopen_s(&f, logpath, "a");
            if (f) { fprintf(f, "%s\n", buf); fclose(f); }
        }
    }

    if (!use_zero_copy) {
        // === STAGING COPY PATH (fallback) ===
        m_zero_copy_tex = nullptr;

        // Create or resize intermediate GPU staging texture with BIND_SHADER_RESOURCE
        if (!m_srv_texture || m_srv_width != texDesc.Width || m_srv_height != texDesc.Height) {
            m_srv_texture.Reset();
            m_srv_y.Reset();
            m_srv_uv.Reset();

            // Build fresh desc - do NOT copy texDesc which may have incompatible flags
            D3D11_TEXTURE2D_DESC srvDesc = {};
            srvDesc.Width = texDesc.Width;
            srvDesc.Height = texDesc.Height;
            srvDesc.MipLevels = 1;
            srvDesc.ArraySize = 1;
            srvDesc.Format = DXGI_FORMAT_NV12;
            srvDesc.SampleDesc.Count = 1;
            srvDesc.Usage = D3D11_USAGE_DEFAULT;
            srvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            if (FAILED(m_device->CreateTexture2D(&srvDesc, nullptr, &m_srv_texture))) {
                return;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC yDesc = {};
            yDesc.Format = DXGI_FORMAT_R8_UNORM;
            yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            yDesc.Texture2D.MipLevels = 1;

            if (FAILED(m_device->CreateShaderResourceView(m_srv_texture.Get(), &yDesc, &m_srv_y))) {
                return;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc = {};
            uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
            uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            uvDesc.Texture2D.MipLevels = 1;

            if (FAILED(m_device->CreateShaderResourceView(m_srv_texture.Get(), &uvDesc, &m_srv_uv))) {
                return;
            }

            m_srv_width = texDesc.Width;
            m_srv_height = texDesc.Height;
        }

        // Copy the specific slice from the decoded texture array into the single-slice staging texture
        // Source subresource = D3D11CalcSubresource(0, array_index, texDesc.MipLevels)
        UINT srcSubresource = D3D11CalcSubresource(0, (UINT)array_index, texDesc.MipLevels);
        m_context->CopySubresourceRegion(m_srv_texture.Get(), 0, 0, 0, 0, nv12_texture, srcSubresource, nullptr);
    }

    // Calculate Aspect Ratio / Scaling Transformation
    float screenAspect = (m_height > 0) ? (static_cast<float>(m_width) / static_cast<float>(m_height)) : 1.0f;
    float videoAspect = (texDesc.Height > 0) ? (static_cast<float>(texDesc.Width) / static_cast<float>(texDesc.Height)) : 1.0f;

    float uvScaleX = 1.0f;
    float uvScaleY = 1.0f;
    float uvOffsetX = 0.0f;
    float uvOffsetY = 0.0f;

    D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};

    if (scaling_mode == 0) {
        // Mode 0: Auto Aspect Fill (Cover)
        if (screenAspect > videoAspect) {
            uvScaleY = videoAspect / screenAspect;
            uvOffsetY = (1.0f - uvScaleY) * 0.5f;
        } else {
            uvScaleX = screenAspect / videoAspect;
            uvOffsetX = (1.0f - uvScaleX) * 0.5f;
        }
    } else if (scaling_mode == 1) {
        // Mode 1: Aspect Fit (Letterbox)
        if (screenAspect > videoAspect) {
            float vpWidth = static_cast<float>(m_height) * videoAspect;
            vp.TopLeftX = (static_cast<float>(m_width) - vpWidth) * 0.5f;
            vp.Width = vpWidth;
        } else {
            float vpHeight = static_cast<float>(m_width) / videoAspect;
            vp.TopLeftY = (static_cast<float>(m_height) - vpHeight) * 0.5f;
            vp.Height = vpHeight;
        }

        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        m_context->ClearRenderTargetView(m_rtv.Get(), black);
    }

    // Update UV Scaling Constant Buffer
    if (m_scaling_cb) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_scaling_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ScalingData* data = static_cast<ScalingData*>(mapped.pData);
            data->uv_scale[0] = uvScaleX;
            data->uv_scale[1] = uvScaleY;
            data->uv_offset[0] = uvOffsetX;
            data->uv_offset[1] = uvOffsetY;
            m_context->Unmap(m_scaling_cb.Get(), 0);
        }
    }

    // Set Render Target & Viewport
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    m_context->RSSetViewports(1, &vp);

    // Bind Vertex Shader and its constant buffer
    m_context->VSSetShader(m_fullscreen_vs.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_scaling_cb.GetAddressOf());

    if (use_zero_copy) {
        // Zero-copy path: use array pixel shader + slice constant buffer
        m_context->PSSetShader(m_nv12_array_ps.Get(), nullptr, 0);
        
        // Update slice index
        if (m_slice_cb) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(m_slice_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                SliceData* sd = static_cast<SliceData*>(mapped.pData);
                sd->arraySlice = static_cast<uint32_t>(array_index);
                m_context->Unmap(m_slice_cb.Get(), 0);
            }
        }
        ID3D11Buffer* psCBs[] = { m_scaling_cb.Get(), m_slice_cb.Get() };
        m_context->PSSetConstantBuffers(0, 2, psCBs);
    } else {
        // Staging copy path: use Texture2D pixel shader
        m_context->PSSetShader(m_nv12_ps.Get(), nullptr, 0);
        m_context->PSSetConstantBuffers(0, 1, m_scaling_cb.GetAddressOf());
    }

    ID3D11ShaderResourceView* srvs[] = { m_srv_y.Get(), m_srv_uv.Get() };
    m_context->PSSetShaderResources(0, 2, srvs);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    // Draw Fullscreen Triangle (3 vertices)
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->Draw(3, 0);

    // Unbind SRVs to allow FFmpeg to write to them next frame
    ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
    m_context->PSSetShaderResources(0, 2, null_srvs);
}

HRESULT D3D11Presenter::Present(UINT syncInterval) {
    if (!m_swapchain) return E_POINTER;
    return m_swapchain->Present(syncInterval, 0);
}

void D3D11Presenter::ClearAndPresent(float r, float g, float b) {
    if (!m_context || !m_rtv) return;
    const float color[4] = { r, g, b, 1.0f };
    m_context->ClearRenderTargetView(m_rtv.Get(), color);
    Present(0);
}

void D3D11Presenter::Resize(int width, int height) {
    if (!m_swapchain || width <= 0 || height <= 0) return;
    
    m_width = width;
    m_height = height;
    m_rtv.Reset();

    m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

    ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
    }
}

ID3D11Device* D3D11Presenter::GetDevice() const {
    return m_device.Get();
}

ID3D11DeviceContext* D3D11Presenter::GetContext() const {
    return m_context.Get();
}

void D3D11Presenter::Cleanup() {
    m_srv_uv.Reset();
    m_srv_y.Reset();
    m_srv_texture.Reset();
    m_zero_copy_tex = nullptr;
    m_srv_width = 0;
    m_srv_height = 0;
    m_slice_cb.Reset();
    m_scaling_cb.Reset();
    m_sampler.Reset();
    m_nv12_ps.Reset();
    m_nv12_array_ps.Reset();
    m_fullscreen_vs.Reset();
    m_rtv.Reset();
    m_swapchain.Reset();
    m_context.Reset();
    m_device.Reset();
}

} // namespace litewp
