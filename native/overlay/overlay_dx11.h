// Малювання кадру чату поверх кадру гри для DX11.
//
// КРОК 2: беремо готову картинку чату зі спільної памʼяті (BGRA, з альфою),
// заливаємо в текстуру й малюємо у вибраному куті з альфа-змішуванням. Текстуру
// оновлюємо ЛИШЕ коли змінився номер кадру (seq) — між оновленнями та сама
// текстура малюється хоч 240 разів на секунду майже задарма.
//
// Свій стан рендера ми виставляємо (шейдери, blend, семплер, viewport), але
// стан гри після себе НЕ відновлюємо в повному обсязі — для оверлея, що малює
// останнім, цього досить, а повний зліпок/відновлення контексту тут лише
// роздуло б крок. Якщо колись зʼявиться гра, якій це заважає, — це вже точкове
// відновлення, а не переробка.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include "../common/log.h"
#include "../common/shared_frame.h"
#include "shared_frame_reader.h"

namespace hominka {

class OverlayDX11 {
public:
    // Викликається з перехопленого Present. swap — свопчейн гри.
    void draw(IDXGISwapChain* swap) {
        if (!reader_.ensure_open()) return;   // Python ще не запустив чат
        if (!ensure_device(swap)) return;

        FrameView f;
        if (!reader_.read(&f)) {
            // Не вдалося зняти узгоджений кадр саме зараз — малюємо попередній,
            // якщо він у нас уже є.
            if (tex_seq_ == 0 || !tex_enabled_) return;
        } else {
            if (!f.enabled) { tex_enabled_ = false; return; }
            tex_enabled_ = true;
            if (f.seq != tex_seq_ || f.width != tex_w_ || f.height != tex_h_) {
                if (!upload(f)) return;
            }
            last_ = f;   // кут, відступи, прозорість беремо з останнього кадру
        }
        if (!srv_ || tex_w_ == 0) return;

        blit();
    }

    void release() {
        release_pipeline();
        release_texture();
        reader_.close();
    }

private:
    bool ensure_device(IDXGISwapChain* swap) {
        if (ready_) return true;
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&device_)) || !device_)
            return false;
        device_->GetImmediateContext(&context_);
        if (!context_) return false;
        if (!build_shaders()) return false;

        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &blend_);

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device_->CreateSamplerState(&sd, &sampler_);

        ready_ = true;
        log("overlay: DX11-конвеєр готовий");
        return true;
    }

    // Заливає BGRA-кадр у динамічну текстуру. Пересоздаємо її, лише коли
    // змінився розмір; інакше просто оновлюємо вміст через Map.
    bool upload(const FrameView& f) {
        if (!tex_ || f.width != tex_w_ || f.height != tex_h_) {
            release_texture();
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = f.width;
            td.Height = f.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex_))) return false;
            if (FAILED(device_->CreateShaderResourceView(tex_, nullptr, &srv_))) {
                release_texture();
                return false;
            }
            tex_w_ = f.width;
            tex_h_ = f.height;
        }

        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(context_->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        const uint8_t* src = f.pixels;
        uint8_t* dst = reinterpret_cast<uint8_t*>(m.pData);
        uint32_t row = f.width * 4;
        for (uint32_t y = 0; y < f.height; ++y)
            memcpy(dst + (size_t)y * m.RowPitch, src + (size_t)y * row, row);
        context_->Unmap(tex_, 0);
        tex_seq_ = f.seq;
        return true;
    }

    void blit() {
        // Розмір заднього буфера — щоб перевести кут+відступ у viewport.
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap_get_back(&back)) || !back) return;
        D3D11_TEXTURE2D_DESC bd;
        back->GetDesc(&bd);

        ID3D11RenderTargetView* rtv = nullptr;
        HRESULT hr = device_->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
        if (FAILED(hr) || !rtv) return;

        float ow = (float)tex_w_, oh = (float)tex_h_;
        float x, y;
        switch (last_.anchor) {
            case ANCHOR_TOP_RIGHT:    x = bd.Width - ow - last_.margin_x; y = (float)last_.margin_y; break;
            case ANCHOR_BOTTOM_LEFT:  x = (float)last_.margin_x; y = bd.Height - oh - last_.margin_y; break;
            case ANCHOR_BOTTOM_RIGHT: x = bd.Width - ow - last_.margin_x; y = bd.Height - oh - last_.margin_y; break;
            default:                  x = (float)last_.margin_x; y = (float)last_.margin_y; break;
        }

        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = x;
        vp.TopLeftY = y;
        vp.Width = ow;
        vp.Height = oh;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

        // Загальна прозорість у константний буфер (0..1).
        if (cbuf_) {
            D3D11_MAPPED_SUBRESOURCE cm;
            if (SUCCEEDED(context_->Map(cbuf_, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                float v[4] = {last_.opacity / 255.0f, 0, 0, 0};
                memcpy(cm.pData, v, sizeof(v));
                context_->Unmap(cbuf_, 0);
            }
        }

        float blend_factor[4] = {1, 1, 1, 1};
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->RSSetViewports(1, &vp);
        context_->VSSetShader(vs_, nullptr, 0);
        context_->PSSetShader(ps_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &srv_);
        context_->PSSetSamplers(0, 1, &sampler_);
        context_->PSSetConstantBuffers(0, 1, &cbuf_);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->OMSetBlendState(blend_, blend_factor, 0xffffffff);
        context_->Draw(4, 0);

        rtv->Release();
    }

    // Задній буфер поточного свопчейна. Тримаємо свопчейн у полі, бо blit() його
    // не отримує аргументом.
    HRESULT swap_get_back(ID3D11Texture2D** out) {
        if (!swap_) return E_FAIL;
        return swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)out);
    }

public:
    void set_swap(IDXGISwapChain* s) { swap_ = s; }

private:
    bool build_shaders();   // shaders.cpp

    void release_texture() {
        if (srv_) { srv_->Release(); srv_ = nullptr; }
        if (tex_) { tex_->Release(); tex_ = nullptr; }
        tex_w_ = tex_h_ = 0;
        tex_seq_ = 0;
    }
    void release_pipeline() {
        if (cbuf_) cbuf_->Release();
        if (sampler_) sampler_->Release();
        if (blend_) blend_->Release();
        if (ps_) ps_->Release();
        if (vs_) vs_->Release();
        if (context_) context_->Release();
        if (device_) device_->Release();
        cbuf_ = nullptr; sampler_ = nullptr; blend_ = nullptr; ps_ = nullptr; vs_ = nullptr;
        context_ = nullptr; device_ = nullptr;
        ready_ = false;
    }

    SharedFrameReader reader_;
    IDXGISwapChain* swap_ = nullptr;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11Buffer* cbuf_ = nullptr;

    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    uint32_t tex_w_ = 0, tex_h_ = 0, tex_seq_ = 0;
    bool tex_enabled_ = false;

    FrameView last_;
    bool ready_ = false;
};

}  // namespace hominka
