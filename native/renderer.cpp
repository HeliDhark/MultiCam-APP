#include "renderer.hpp"
#include <algorithm>
#include <cstring>
#include "vs_main.h"
#include "ps_convert.h"
#include "ps_blit.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
struct Params { float srcW, srcH, useBt709, pad; };
}

bool Renderer::init(HWND window, std::wstring& error) {
    window_ = window;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) { error = L"Não foi possível criar o dispositivo Direct3D 11."; return false; }

    if (window_) {
        RECT rc{};
        GetClientRect(window_, &rc);
        targetW_ = std::max<LONG>(1, rc.right - rc.left);
        targetH_ = std::max<LONG>(1, rc.bottom - rc.top);
        Ptr<IDXGIDevice> dxgiDevice;
        Ptr<IDXGIAdapter> adapter;
        Ptr<IDXGIFactory> factory;
        if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            error = L"Não foi possível acessar o adaptador de vídeo.";
            return false;
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = targetW_;
        desc.BufferDesc.Height = targetH_;
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = window_;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (FAILED(factory->CreateSwapChain(device_.Get(), &desc, &swap_))) {
            error = L"Não foi possível criar a superfície de exibição.";
            return false;
        }
        factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }

    if (FAILED(device_->CreateVertexShader(g_vs_main, sizeof(g_vs_main), nullptr, &vs_)) ||
        FAILED(device_->CreatePixelShader(g_ps_convert, sizeof(g_ps_convert), nullptr, &psConvert_)) ||
        FAILED(device_->CreatePixelShader(g_ps_blit, sizeof(g_ps_blit), nullptr, &psBlit_))) {
        error = L"Não foi possível carregar os shaders.";
        return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &params_)) ||
        FAILED(device_->CreateSamplerState(&sd, &sampler_))) {
        error = L"Não foi possível criar os recursos de vídeo.";
        return false;
    }
    if (window_ && !createTarget()) { error = L"Não foi possível criar o destino de vídeo."; return false; }
    return true;
}

bool Renderer::createTarget() {
    targetView_.Reset();
    target_.Reset();
    if (targetW_ <= 0 || targetH_ <= 0) return false;
    if (swap_) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        if (FAILED(swap_->ResizeBuffers(1, targetW_, targetH_, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) return false;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&target_)))) return false;
    } else {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = targetW_;
        td.Height = targetH_;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &target_))) return false;
    }
    return SUCCEEDED(device_->CreateRenderTargetView(target_.Get(), nullptr, &targetView_));
}

void Renderer::resize(int width, int height) {
    if (!device_ || width <= 0 || height <= 0) return;
    if (width == targetW_ && height == targetH_ && target_) return;
    targetW_ = width;
    targetH_ = height;
    createTarget();
}

bool Renderer::createSource(int width, int height) {
    packed_.Reset(); rgb_.Reset(); packedView_.Reset(); rgbView_.Reset(); rgbTarget_.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width / 2;
    td.Height = height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &packed_)) ||
        FAILED(device_->CreateShaderResourceView(packed_.Get(), nullptr, &packedView_))) return false;
    td.Width = width;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.CPUAccessFlags = 0;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &rgb_)) ||
        FAILED(device_->CreateShaderResourceView(rgb_.Get(), nullptr, &rgbView_)) ||
        FAILED(device_->CreateRenderTargetView(rgb_.Get(), nullptr, &rgbTarget_))) return false;
    srcW_ = width;
    srcH_ = height;
    return true;
}

bool Renderer::upload(const uint8_t* yuy2, int width, int height) {
    if (!device_ || !yuy2 || width < 2 || height < 1 || (width & 1)) return false;
    if (width != srcW_ || height != srcH_ || !packed_) {
        hasFrame_ = false;
        if (!createSource(width, height)) { srcW_ = srcH_ = 0; return false; }
    }
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context_->Map(packed_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
    const size_t rowBytes = size_t(width) * 2;
    for (int y = 0; y < height; ++y)
        std::memcpy(static_cast<uint8_t*>(map.pData) + size_t(y) * map.RowPitch, yuy2 + size_t(y) * rowBytes, rowBytes);
    context_->Unmap(packed_.Get(), 0);

    Params p{float(width), float(height), height >= 720 ? 1.0f : 0.0f, 0.0f};
    context_->UpdateSubresource(params_.Get(), 0, nullptr, &p, 0, 0);
    ID3D11RenderTargetView* rtv = rgbTarget_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    context_->RSSetViewports(1, &vp);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(psConvert_.Get(), nullptr, 0);
    ID3D11Buffer* cb = params_.Get();
    context_->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srv = packedView_.Get();
    context_->PSSetShaderResources(0, 1, &srv);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);
    hasFrame_ = true;
    return true;
}

void Renderer::draw(viewer::Fit fit) {
    if (!targetView_) return;
    ID3D11RenderTargetView* rtv = targetView_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    const float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(rtv, black);
    if (!hasFrame_) return;
    const viewer::Rect r = viewer::fitRect({srcW_, srcH_}, {targetW_, targetH_}, fit);
    D3D11_VIEWPORT vp{float(r.x), float(r.y), float(r.w), float(r.h), 0, 1};
    context_->RSSetViewports(1, &vp);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(psBlit_.Get(), nullptr, 0);
    ID3D11SamplerState* ss = sampler_.Get();
    context_->PSSetSamplers(0, 1, &ss);
    ID3D11ShaderResourceView* srv = rgbView_.Get();
    context_->PSSetShaderResources(0, 1, &srv);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);
}

bool Renderer::present() {
    return !swap_ || SUCCEEDED(swap_->Present(0, 0));
}

bool Renderer::readback(std::vector<uint8_t>& bgra, int& width, int& height) {
    if (!target_) return false;
    D3D11_TEXTURE2D_DESC td{};
    target_->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    Ptr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging))) return false;
    context_->CopyResource(staging.Get(), target_.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    width = int(td.Width);
    height = int(td.Height);
    bgra.resize(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y)
        std::memcpy(&bgra[size_t(y) * width * 4], static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, size_t(width) * 4);
    context_->Unmap(staging.Get(), 0);
    return true;
}
