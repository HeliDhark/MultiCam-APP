#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include "viewer_core.hpp"

// Direct3D 11 YUY2 renderer. window == nullptr renders offscreen (tests).
class Renderer {
public:
    bool init(HWND window, std::wstring& error);
    void resize(int width, int height);
    // yuy2: width*height*2 bytes, width even. Returns false on invalid input.
    bool upload(const uint8_t* yuy2, int width, int height);
    void draw(viewer::Fit fit);
    // False when Present failed.
    bool present();
    // The GPU was reset or removed (driver update, TDR): this renderer cannot draw any more.
    bool lost() const { return device_ && FAILED(device_->GetDeviceRemovedReason()); }
    // Forget the current picture: the next draw() shows black until upload() runs again.
    void clear() { hasFrame_ = false; }
    // Top-down BGRA copy of the target. Call before present().
    bool readback(std::vector<uint8_t>& bgra, int& width, int& height);

private:
    bool createTarget();
    bool createSource(int width, int height);

    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    HWND window_ = nullptr;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    Ptr<IDXGISwapChain> swap_;
    Ptr<ID3D11Texture2D> target_;
    Ptr<ID3D11RenderTargetView> targetView_;
    Ptr<ID3D11Texture2D> packed_, rgb_;
    Ptr<ID3D11ShaderResourceView> packedView_, rgbView_;
    Ptr<ID3D11RenderTargetView> rgbTarget_;
    Ptr<ID3D11VertexShader> vs_;
    Ptr<ID3D11PixelShader> psConvert_, psBlit_;
    Ptr<ID3D11Buffer> params_;
    Ptr<ID3D11SamplerState> sampler_;
    int targetW_ = 0, targetH_ = 0, srcW_ = 0, srcH_ = 0;
    bool hasFrame_ = false;
};
