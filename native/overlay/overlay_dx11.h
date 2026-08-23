// Малювання поверх кадру гри для DX11.
//
// КРОК 1: єдина задача — намалювати помітний прямокутник у кутку, щоб на власні
// очі побачити, що ми малюємо ПОВЕРХ гри, у її ж свопчейні. Це фундамент: коли
// прямокутник видно, крок 2 замінить його на текстуру з кадром чату.
//
// Робимо це навмисно найдешевшим способом — суцільна заливка через власний
// маленький шейдер і трикутник-«ножиці». Ніякого стану гри не псуємо: свій
// render target, свій viewport, наприкінці нічого за собою не тягнемо (у DX11
// стан context, який ми виставили, лишається, але для кроку 1 цього досить;
// повне збереження/відновлення стану — теж крок 2, коли з'явиться реальний
// вміст і почнуть проявлятися конфлікти станів).
#pragma once

#include <windows.h>
#include <d3d11.h>

#include "../common/log.h"

namespace hominka {

class OverlayDX11 {
public:
    // Викликається з перехопленого Present. swap — свопчейн гри.
    void draw_test_rectangle(IDXGISwapChain* swap) {
        if (!ensure_device(swap)) return;

        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back)
            return;

        ID3D11RenderTargetView* rtv = nullptr;
        HRESULT hr = device_->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
        if (FAILED(hr) || !rtv) return;

        D3D11_TEXTURE2D_DESC bd = {};
        {
            ID3D11Texture2D* b2 = nullptr;
            if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&b2)) && b2) {
                b2->GetDesc(&bd);
                b2->Release();
            }
        }

        context_->OMSetRenderTargets(1, &rtv, nullptr);

        // Прямокутник ~220x54 у лівому верхньому куті, з невеликим відступом.
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = 24.0f;
        vp.TopLeftY = 24.0f;
        vp.Width = 220.0f;
        vp.Height = 54.0f;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);

        context_->VSSetShader(vs_, nullptr, 0);
        context_->PSSetShader(ps_, nullptr, 0);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->OMSetBlendState(blend_, nullptr, 0xffffffff);
        // Вершини задає сам вершинний шейдер за SV_VertexID — буфер не потрібен.
        context_->Draw(4, 0);

        rtv->Release();
        (void)bd;
    }

    void release() {
        if (blend_) blend_->Release();
        if (ps_) ps_->Release();
        if (vs_) vs_->Release();
        if (context_) context_->Release();
        if (device_) device_->Release();
        blend_ = nullptr; ps_ = nullptr; vs_ = nullptr;
        context_ = nullptr; device_ = nullptr;
        ready_ = false;
    }

private:
    // Пристрій беремо з самого свопчейна гри — той самий, яким малює гра.
    bool ensure_device(IDXGISwapChain* swap) {
        if (ready_) return true;

        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&device_)) || !device_) {
            log("overlay: не вдалося дістати ID3D11Device зі свопчейна");
            return false;
        }
        device_->GetImmediateContext(&context_);
        if (!context_) { log("overlay: немає immediate context"); return false; }

        if (!build_shaders()) return false;

        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &blend_);

        ready_ = true;
        log("overlay: DX11-малювання готове");
        return true;
    }

    // Шейдери у вигляді байткоду тут не тримаємо: D3DCompile тягнув би d3dcompiler,
    // якого в mingw немає під рукою. Замість цього компілюємо крихітний HLSL у
    // рантаймі через d3dcompiler_47, ЯКЩО він є; якщо ні — падаємо назад на
    // готовий байткод (див. shaders.h).
    bool build_shaders();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    bool ready_ = false;
};

}  // namespace hominka
