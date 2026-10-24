#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "d3d11_presenter.h"
#include "core/win_utils.h"
#include "fullscreen_quad_vs.h"
#include "nv12_to_rgb_ps.h"
#include <cmath>
#include <cstring>

namespace miniwp {

static ID3D11Device*        g_shared_device = nullptr;
static ID3D11DeviceContext* g_shared_context = nullptr;

ID3D11Device* GetSharedD3DDevice() { return g_shared_device; }
ID3D11DeviceContext* GetSharedD3DDeviceContext() { return g_shared_context; }

static bool CreateNV12SliceSRVs(
    ID3D11Device* device,
    ID3D11Texture2D* tex,
    UINT slice_index,
    ComPtr<ID3D11ShaderResourceView>& out_y,
    ComPtr<ID3D11ShaderResourceView>& out_uv
) {
    D3D11_SHADER_RESOURCE_VIEW_DESC yDesc = {};
    yDesc.Format = DXGI_FORMAT_R8_UNORM;
    yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    yDesc.Texture2DArray.MostDetailedMip = 0;
    yDesc.Texture2DArray.MipLevels = 1;
    yDesc.Texture2DArray.FirstArraySlice = slice_index;
    yDesc.Texture2DArray.ArraySize = 1;

    if (FAILED(device->CreateShaderResourceView(tex, &yDesc, &out_y))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc = yDesc;
    uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device->CreateShaderResourceView(tex, &uvDesc, &out_uv))) {
        out_y.Reset();
        return false;
    }
    return true;
}

D3D11Presenter::D3D11Presenter() = default;

D3D11Presenter::~D3D11Presenter() {
    Cleanup();
}

void D3D11Presenter::CloseWaitableHandle() noexcept {
    if (m_frame_latency_waitable_object) {
        CloseHandle(m_frame_latency_waitable_object);
        m_frame_latency_waitable_object = nullptr;
    }
}

bool D3D11Presenter::Init(HWND hwnd, int width, int height, int gpu_index) {
    Cleanup();

    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    if (gpu_index == -1) {
        m_is_software_mode = true;
        m_adapter_name = "CPU: Software Processor Decode (swscale)";
        m_adapter_type = "CPU Software";
    } else {
        m_is_software_mode = false;
    }

    static constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    ComPtr<IDXGIAdapter> targetAdapter;
    D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_HARDWARE;

    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        if (gpu_index >= 0) {
            int match_idx = 0;
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> adapter1;
                if (factory->EnumAdapters1(i, &adapter1) == DXGI_ERROR_NOT_FOUND) break;

                DXGI_ADAPTER_DESC1 desc = {};
                adapter1->GetDesc1(&desc);
                if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    if (match_idx == gpu_index) {
                        targetAdapter = adapter1;
                        driverType = D3D_DRIVER_TYPE_UNKNOWN;
                        break;
                    }
                    match_idx++;
                }
            }
        } else {
            // CPU Software Mode (gpu_index == -1): route presentation to iGPU when available
            // so discrete high-power GPU stays powered down.
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> adapter1;
                if (factory->EnumAdapters1(i, &adapter1) == DXGI_ERROR_NOT_FOUND) break;

                DXGI_ADAPTER_DESC1 desc = {};
                adapter1->GetDesc1(&desc);
                if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    if (desc.VendorId == 0x8086 || (desc.VendorId == 0x1002 && desc.DedicatedVideoMemory <= 512ULL * 1024 * 1024)) {
                        targetAdapter = adapter1;
                        driverType = D3D_DRIVER_TYPE_UNKNOWN;
                        break;
                    }
                }
            }
        }
    }

    D3D_FEATURE_LEVEL actualFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        targetAdapter.Get(),
        driverType,
        nullptr,
        flags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_device,
        &actualFeatureLevel,
        &m_context
    );

    if (FAILED(hr) && targetAdapter) {
        hr = D3D11CreateDevice(
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
    }

    int retry_count = 0;
    while (FAILED(hr) && retry_count < 5) {
        Sleep(200);
        hr = D3D11CreateDevice(
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
        retry_count++;
    }

    if (FAILED(hr)) {
        return false;
    }

    g_shared_device = m_device.Get();
    g_shared_context = m_context.Get();

    if (!m_is_software_mode) {
        ComPtr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(m_device.As(&dxgiDev))) {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc = {};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    m_adapter_name = WideToUtf8(desc.Description);
                    size_t dedicated_mb = desc.DedicatedVideoMemory / (1024 * 1024);
                    if (desc.VendorId == 0x10DE) {
                        m_adapter_type = "NVIDIA Dedicated";
                    } else if (desc.VendorId == 0x8086) {
                        m_adapter_type = (dedicated_mb > 2048) ? "Intel Arc Dedicated" : "Intel iGPU";
                    } else if (desc.VendorId == 0x1002) {
                        m_adapter_type = (dedicated_mb > 512) ? "AMD Radeon Dedicated" : "AMD Radeon iGPU";
                    } else {
                        m_adapter_type = (dedicated_mb > 1024) ? "Dedicated GPU" : "Integrated GPU";
                    }
                }
            }
        }
    }

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(m_device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    if (!CreateSwapChain(hwnd, width, height)) {
        Cleanup();
        return false;
    }

    if (!CreateShaders()) {
        Cleanup();
        return false;
    }

    return true;
}

bool D3D11Presenter::CreateSwapChain(HWND hwnd, int width, int height) {
    CloseWaitableHandle();
    m_rtv.Reset();
    m_swapchain.Reset();

    {
        ComPtr<IDXGIFactory2> dxgiFactory2;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> dxgiAdapter;
        if (SUCCEEDED(m_device.As(&dxgiDevice)) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) &&
            SUCCEEDED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory2)))) {

            DXGI_SWAP_CHAIN_DESC1 sd = {};
            sd.Width = static_cast<UINT>(width);
            sd.Height = static_cast<UINT>(height);
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.Stereo = FALSE;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.Scaling = DXGI_SCALING_STRETCH;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

            ComPtr<IDXGISwapChain1> sc1;
            HRESULT hr_sc = dxgiFactory2->CreateSwapChainForHwnd(
                m_device.Get(), hwnd, &sd, nullptr, nullptr, &sc1
            );

            if (FAILED(hr_sc)) {
                sd.Flags = 0;
                hr_sc = dxgiFactory2->CreateSwapChainForHwnd(
                    m_device.Get(), hwnd, &sd, nullptr, nullptr, &sc1
                );
            }

            if (SUCCEEDED(hr_sc) && sc1) {
                m_swapchain = sc1;
                dxgiFactory2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

                if (sd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
                    ComPtr<IDXGISwapChain2> sc2;
                    if (SUCCEEDED(sc1.As(&sc2))) {
                        sc2->SetMaximumFrameLatency(1);
                        m_frame_latency_waitable_object = sc2->GetFrameLatencyWaitableObject();
                    }
                }
            }
        }
    }

    if (!m_swapchain) {
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        scDesc.BufferCount = 2;
        scDesc.BufferDesc.Width = static_cast<UINT>(width);
        scDesc.BufferDesc.Height = static_cast<UINT>(height);
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

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }

    return SUCCEEDED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv));
}

bool D3D11Presenter::CreateShaders() {
    // Load build-time fxc-compiled shader bytecode (zero runtime D3DCompile / d3dcompiler_47.dll!)
    HRESULT hr = m_device->CreateVertexShader(
        g_vs_bytecode, sizeof(g_vs_bytecode), nullptr, &m_fullscreen_vs
    );
    if (FAILED(hr)) return false;

    hr = m_device->CreatePixelShader(
        g_ps_bytecode, sizeof(g_ps_bytecode), nullptr, &m_nv12_ps
    );
    if (FAILED(hr)) return false;

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

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ScalingData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    m_cb_valid = false;
    return SUCCEEDED(m_device->CreateBuffer(&cbDesc, nullptr, &m_scaling_cb));
}

void D3D11Presenter::CaptureStartFrame(ID3D11Texture2D* nv12_texture, int array_index) {
    if (!m_device || !m_context || !nv12_texture || array_index < 0) return;
    D3D11_TEXTURE2D_DESC desc;
    nv12_texture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) return;

    if (!m_start_texture || m_start_width != desc.Width || m_start_height != desc.Height) {
        ResetStartFrame();

        D3D11_TEXTURE2D_DESC srvDesc = desc;
        srvDesc.ArraySize = 1;
        srvDesc.MipLevels = 1;
        srvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        srvDesc.MiscFlags = 0;
        srvDesc.Usage = D3D11_USAGE_DEFAULT;
        srvDesc.CPUAccessFlags = 0;

        if (FAILED(m_device->CreateTexture2D(&srvDesc, nullptr, &m_start_texture))) {
            return;
        }
        if (!CreateNV12SliceSRVs(m_device.Get(), m_start_texture.Get(), 0, m_start_srv_y, m_start_srv_uv)) {
            ResetStartFrame();
            return;
        }

        m_start_width = desc.Width;
        m_start_height = desc.Height;
    }

    m_context->CopySubresourceRegion(
        m_start_texture.Get(), 0, 0, 0, 0,
        nv12_texture, static_cast<UINT>(array_index), nullptr
    );
}

void D3D11Presenter::ResetStartFrame() {
    m_start_srv_uv.Reset();
    m_start_srv_y.Reset();
    m_start_texture.Reset();
    m_start_width = 0;
    m_start_height = 0;
    m_cb_valid = false;
}

void D3D11Presenter::ReleaseVideoResources() {
    ResetStartFrame();
    for (auto& pair : m_slice_srvs) {
        pair.y.Reset();
        pair.uv.Reset();
    }
    m_cached_source_tex = nullptr;
    m_cached_source_w = 0;
    m_cached_source_h = 0;
    m_zero_copy_rejected = false;

    m_fallback_srv_uv.Reset();
    m_fallback_srv_y.Reset();
    m_fallback_texture.Reset();
    m_fallback_width = 0;
    m_fallback_height = 0;
    m_cb_valid = false;
}

bool D3D11Presenter::AcquireFrameSRVs(
    ID3D11Texture2D* nv12_texture,
    const D3D11_TEXTURE2D_DESC& texDesc,
    int array_index,
    ID3D11ShaderResourceView*& out_y,
    ID3D11ShaderResourceView*& out_uv
) {
    out_y = nullptr;
    out_uv = nullptr;
    if (array_index < 0) return false;

    // Reset slice cache if the underlying texture pool changed
    if (m_cached_source_tex != nv12_texture ||
        m_cached_source_w != texDesc.Width ||
        m_cached_source_h != texDesc.Height) {
        for (auto& pair : m_slice_srvs) {
            pair.y.Reset();
            pair.uv.Reset();
        }
        m_cached_source_tex = nv12_texture;
        m_cached_source_w = texDesc.Width;
        m_cached_source_h = texDesc.Height;
        m_zero_copy_rejected = false;
    }

    // 1. Fast Path: True Zero-Copy SRV directly on the D3D11VA Texture2DArray slice!
    const UINT slice = static_cast<UINT>(array_index);
    if (!m_zero_copy_rejected &&
        (texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
        slice < MAX_ZERO_COPY_SLICES &&
        slice < texDesc.ArraySize) {

        auto& entry = m_slice_srvs[slice];
        if (!entry.y || !entry.uv) {
            if (!CreateNV12SliceSRVs(m_device.Get(), nv12_texture, slice, entry.y, entry.uv)) {
                m_zero_copy_rejected = true;
            }
        }
        if (!m_zero_copy_rejected && entry.y && entry.uv) {
            out_y = entry.y.Get();
            out_uv = entry.uv.Get();
            return true;
        }
    }

    // 2. Fallback Path: 1-slice GPU copy (used only on drivers disallowing decoder SRVs)
    if (!m_fallback_texture || m_fallback_width != texDesc.Width || m_fallback_height != texDesc.Height) {
        m_fallback_srv_uv.Reset();
        m_fallback_srv_y.Reset();
        m_fallback_texture.Reset();

        D3D11_TEXTURE2D_DESC srvDesc = texDesc;
        srvDesc.ArraySize = 1;
        srvDesc.MipLevels = 1;
        srvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        srvDesc.Usage = D3D11_USAGE_DEFAULT;
        srvDesc.CPUAccessFlags = 0;
        srvDesc.MiscFlags = 0;

        if (FAILED(m_device->CreateTexture2D(&srvDesc, nullptr, &m_fallback_texture))) {
            return false;
        }
        if (!CreateNV12SliceSRVs(m_device.Get(), m_fallback_texture.Get(), 0, m_fallback_srv_y, m_fallback_srv_uv)) {
            m_fallback_texture.Reset();
            return false;
        }
        m_fallback_width = texDesc.Width;
        m_fallback_height = texDesc.Height;
    }

    m_context->CopySubresourceRegion(m_fallback_texture.Get(), 0, 0, 0, 0, nv12_texture, slice, nullptr);
    out_y = m_fallback_srv_y.Get();
    out_uv = m_fallback_srv_uv.Get();
    return true;
}

void D3D11Presenter::RenderFrame(
    ID3D11Texture2D* nv12_texture,
    int array_index,
    int scaling_mode,
    const std::vector<DisplayViewport>& target_viewports,
    float blend_alpha,
    int valid_width,
    int valid_height
) {
    if (!m_context || !m_rtv || !nv12_texture) return;

    D3D11_TEXTURE2D_DESC texDesc;
    nv12_texture->GetDesc(&texDesc);
    if (texDesc.Width == 0 || texDesc.Height == 0) return;

    ID3D11ShaderResourceView* srv_y = nullptr;
    ID3D11ShaderResourceView* srv_uv = nullptr;
    if (!AcquireFrameSRVs(nv12_texture, texDesc, array_index, srv_y, srv_uv)) {
        return;
    }

    const float content_w = (valid_width > 0 && static_cast<UINT>(valid_width) <= texDesc.Width)
        ? static_cast<float>(valid_width)
        : static_cast<float>(texDesc.Width);
    const float content_h = (valid_height > 0 && static_cast<UINT>(valid_height) <= texDesc.Height)
        ? static_cast<float>(valid_height)
        : static_cast<float>(texDesc.Height);

    const float uvMaxX = content_w / static_cast<float>(texDesc.Width);
    const float uvMaxY = content_h / static_cast<float>(texDesc.Height);

    DisplayViewport defaultVp = { 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height) };
    const DisplayViewport* pVps = &defaultVp;
    size_t vpCount = 1;

    if (!target_viewports.empty()) {
        pVps = target_viewports.data();
        vpCount = target_viewports.size();
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        m_context->ClearRenderTargetView(m_rtv.Get(), black);
    } else if (scaling_mode == 1) {
        // Clear letterbox/pillarbox bars in Aspect Fit mode
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        m_context->ClearRenderTargetView(m_rtv.Get(), black);
    }

    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    m_context->VSSetShader(m_fullscreen_vs.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_scaling_cb.GetAddressOf());
    m_context->PSSetShader(m_nv12_ps.Get(), nullptr, 0);
    m_context->PSSetConstantBuffers(0, 1, m_scaling_cb.GetAddressOf());

    ID3D11ShaderResourceView* srvs[4] = {
        srv_y,
        srv_uv,
        (m_start_srv_y ? m_start_srv_y.Get() : srv_y),
        (m_start_srv_uv ? m_start_srv_uv.Get() : srv_uv)
    };
    m_context->PSSetShaderResources(0, 4, srvs);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);

    const float videoAspect = (content_h > 0.0f) ? (content_w / content_h) : 1.0f;
    const bool has_start = (m_start_srv_y && m_start_srv_uv && blend_alpha > 0.001f);

    for (size_t i = 0; i < vpCount; ++i) {
        const auto& dispVp = pVps[i];
        const float screenAspect = (dispVp.height > 0.0f) ? (dispVp.width / dispVp.height) : 1.0f;
        float uvScaleX = 1.0f;
        float uvScaleY = 1.0f;
        float uvOffsetX = 0.0f;
        float uvOffsetY = 0.0f;

        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = dispVp.x;
        vp.TopLeftY = dispVp.y;
        vp.Width = dispVp.width;
        vp.Height = dispVp.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

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
                float vpWidth = dispVp.height * videoAspect;
                vp.TopLeftX = dispVp.x + (dispVp.width - vpWidth) * 0.5f;
                vp.Width = vpWidth;
            } else {
                float vpHeight = dispVp.width / videoAspect;
                vp.TopLeftY = dispVp.y + (dispVp.height - vpHeight) * 0.5f;
                vp.Height = vpHeight;
            }
        }

        ScalingData nextCb = {
            { uvScaleX, uvScaleY },
            { uvOffsetX, uvOffsetY },
            { uvMaxX, uvMaxY },
            has_start ? blend_alpha : 0.0f,
            has_start ? 1.0f : 0.0f
        };

        // Only Map/Unmap constant buffer when parameters actually change
        if (m_scaling_cb && (!m_cb_valid || std::memcmp(&m_last_cb_data, &nextCb, sizeof(ScalingData)) != 0)) {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(m_context->Map(m_scaling_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, &nextCb, sizeof(ScalingData));
                m_context->Unmap(m_scaling_cb.Get(), 0);
                m_last_cb_data = nextCb;
                m_cb_valid = true;
            }
        }

        m_context->RSSetViewports(1, &vp);
        m_context->Draw(3, 0);
    }

    // Unbind SRVs immediately so D3D11VA decoder can freely write to any pool slice on the next frame
    ID3D11ShaderResourceView* null_srvs[4] = { nullptr, nullptr, nullptr, nullptr };
    m_context->PSSetShaderResources(0, 4, null_srvs);
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

    UINT flags = (m_frame_latency_waitable_object != nullptr) ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    m_swapchain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, flags);

    ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
    }
    m_cb_valid = false;
}

size_t D3D11Presenter::GetVramUsageMB() const {
    if (m_is_software_mode || !m_device) {
        return 0;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(m_device.As(&dxgiDevice))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
#if __has_include(<dxgi1_4.h>)
            ComPtr<IDXGIAdapter3> adapter3;
            if (SUCCEEDED(adapter.As(&adapter3))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                    size_t vram_mb = static_cast<size_t>(memInfo.CurrentUsage / (1024 * 1024));
                    if (vram_mb > 0) return vram_mb;
                }
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &memInfo))) {
                    size_t shared_mb = static_cast<size_t>(memInfo.CurrentUsage / (1024 * 1024));
                    if (shared_mb > 0) return shared_mb;
                }
            }
#endif
        }
    }
    return 0;
}

void D3D11Presenter::Cleanup() {
    g_shared_device = nullptr;
    g_shared_context = nullptr;

    ReleaseVideoResources();
    CloseWaitableHandle();

    m_scaling_cb.Reset();
    m_sampler.Reset();
    m_nv12_ps.Reset();
    m_fullscreen_vs.Reset();
    m_rtv.Reset();
    m_swapchain.Reset();
    m_context.Reset();
    m_device.Reset();
}

} // namespace miniwp
