// Малювання кадру чату поверх кадру гри для DirectX 9.
//
// Багато одиночних та старих ігор (Source-двигун, тобто CS:GO старий, L4D2,
// TF2; купа інді й емуляторів) — саме DX9. Хук той самий за духом, що й у DX11:
// IDirect3DDevice9 — COM-обʼєкт зі спільною на процес vtable, тож підмінивши в
// ній EndScene, ми перехоплюємо кадр будь-якого пристрою гри.
//
// Малюємо фіксованим конвеєром (без шейдерів): текстура A8R8G8B8 (у памʼяті це
// ті самі BGRA, що дає Python) і два трикутники з екранними координатами.
// Зберігаємо й повертаємо рівно той стан рендера, який чіпаємо, — Get перед,
// Set після. Саме «рівно той»: блок стану (CreateStateBlock) тут не можна, його
// заборонено викликати між BeginScene і EndScene, і на справжній грі це вішало
// драйвер (див. blit).
#pragma once

#include <windows.h>
#include <d3d9.h>

#include "../common/log.h"
#include "shared_frame_reader.h"

namespace hominka {

struct D3D9Vertex {
    float x, y, z, rhw;
    float u, v;
};
#define HOMINKA_D3D9_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

class OverlayDX9 {
public:
    // Викликається з перехопленого EndScene. device — пристрій гри.
    void draw(IDirect3DDevice9* device) {
        if (!reader_.ensure_open()) return;

        FrameView f;
        bool got = reader_.read(&f);
        if (got && !logged_) {
            logged_ = true;
            log("overlay(dx9): кадр — enabled=%u target=%u ми=%u розмір=%ux%u",
                (unsigned)f.enabled, f.target_pid, (unsigned)GetCurrentProcessId(),
                f.width, f.height);
        }
        if (got) {
            // Малюємо лише у процесі-цілі: інакше чат зʼявився б у кожному
            // вікні, куди DLL випадково потрапила.
            if (f.target_pid && f.target_pid != GetCurrentProcessId()) {
                if (!pid_warned_) { pid_warned_ = true;
                    log("overlay(dx9): НЕ малюю — ціль pid=%u, а ми pid=%u",
                        f.target_pid, (unsigned)GetCurrentProcessId()); }
                return;
            }
            if (!f.enabled) { enabled_ = false; return; }
            enabled_ = true;
            // Перезаливаємо і коли текстури немає: після Reset пристрою (зміна
            // роздільної здатності) стара D3DPOOL_DEFAULT-текстура недійсна, і
            // upload звільнить її — а тут ми відновимося без хука на Reset.
            if (f.seq != tex_seq_ || f.width != tex_w_ || f.height != tex_h_ || !tex_)
                if (!upload(device, f)) return;
            last_ = f;
        } else if (!enabled_ || tex_seq_ == 0) {
            return;   // ще нема узгодженого кадру — і попереднього теж
        }
        if (!tex_ || tex_w_ == 0) return;

        blit(device);
    }

    void release() {
        release_texture();
        reader_.close();
        ready_ = false;
    }

    // Перед Reset пристрою (зміна роздільної здатності/режиму) ресурси
    // D3DPOOL_DEFAULT стають недійсними — звільняємо текстуру, вона пересоздасться
    // сама на наступному кадрі.
    void on_lost() {
        release_texture();
    }

private:
    bool upload(IDirect3DDevice9* device, const FrameView& f) {
        if (!tex_ || f.width != tex_w_ || f.height != tex_h_) {
            release_texture();
            // D3DPOOL_MANAGED, а не DEFAULT: КЛЮЧОВЕ для Alt-Tab. Коли гра
            // втрачає пристрій (Alt-Tab у DX9), вона може відновитися лише якщо
            // ВСІ ресурси D3DPOOL_DEFAULT звільнено — інакше її Reset() падає, і
            // гра зависає (а потім і не закривається). MANAGED переживає Reset
            // сам, рантайм відновлює його — тож наша текстура грі не заважає.
            // Керовану текстуру теж можна лочити й оновлювати (без DYNAMIC).
            HRESULT hr = device->CreateTexture(f.width, f.height, 1,
                0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex_, nullptr);
            if (FAILED(hr)) {
                log("overlay(dx9): CreateTexture не вдалося, hr=0x%lx", (unsigned long)hr);
                return false;
            }
            tex_w_ = f.width;
            tex_h_ = f.height;
        }
        D3DLOCKED_RECT lr;
        if (FAILED(tex_->LockRect(0, &lr, nullptr, 0))) {
            // Не вдалося залочити — викидаємо текстуру, наступний кадр створить нову.
            release_texture();
            return false;
        }
        const uint8_t* src = f.pixels;
        uint8_t* dst = reinterpret_cast<uint8_t*>(lr.pBits);
        uint32_t row = f.width * 4;
        for (uint32_t y = 0; y < f.height; ++y)
            memcpy(dst + (size_t)y * lr.Pitch, src + (size_t)y * row, row);
        tex_->UnlockRect(0);
        tex_seq_ = f.seq;
        return true;
    }

    void blit(IDirect3DDevice9* device) {
        D3DVIEWPORT9 vp;
        if (FAILED(device->GetViewport(&vp))) return;
        float sw = (float)vp.Width, sh = (float)vp.Height;
        float ow = (float)tex_w_, oh = (float)tex_h_;

        float x, y;
        switch (last_.anchor) {
            case ANCHOR_TOP_RIGHT:    x = sw - ow - last_.margin_x; y = (float)last_.margin_y; break;
            case ANCHOR_BOTTOM_LEFT:  x = (float)last_.margin_x; y = sh - oh - last_.margin_y; break;
            case ANCHOR_BOTTOM_RIGHT: x = sw - ow - last_.margin_x; y = sh - oh - last_.margin_y; break;
            default:                  x = (float)last_.margin_x; y = (float)last_.margin_y; break;
        }

        // Зберігаємо і повертаємо ЛИШЕ той стан, який чіпаємо. Раніше тут був
        // CreateStateBlock — а його НЕ МОЖНА викликати між BeginScene і
        // EndScene (а ми саме там), і на справжній грі це вішало драйвер
        // намертво (порожній тест-хост це пробачав, L4D2 — ні).
        IDirect3DVertexShader9* oldVS = nullptr; device->GetVertexShader(&oldVS);
        IDirect3DPixelShader9* oldPS = nullptr; device->GetPixelShader(&oldPS);
        IDirect3DBaseTexture9* oldTex = nullptr; device->GetTexture(0, &oldTex);
        DWORD oldFVF = 0; device->GetFVF(&oldFVF);

        struct RS { D3DRENDERSTATETYPE s; DWORD v; };
        RS rs[] = {
            {D3DRS_LIGHTING,0},{D3DRS_ZENABLE,0},{D3DRS_CULLMODE,0},
            {D3DRS_ALPHABLENDENABLE,0},{D3DRS_SRCBLEND,0},{D3DRS_DESTBLEND,0},
            {D3DRS_ALPHATESTENABLE,0},{D3DRS_FOGENABLE,0},{D3DRS_STENCILENABLE,0},
            {D3DRS_SCISSORTESTENABLE,0},{D3DRS_COLORWRITEENABLE,0},
            {D3DRS_SRGBWRITEENABLE,0},{D3DRS_TEXTUREFACTOR,0},{D3DRS_SHADEMODE,0},
        };
        for (auto& r : rs) device->GetRenderState(r.s, &r.v);

        struct TS { D3DTEXTURESTAGESTATETYPE s; DWORD v; };
        TS ts[] = {
            {D3DTSS_COLOROP,0},{D3DTSS_COLORARG1,0},
            {D3DTSS_ALPHAOP,0},{D3DTSS_ALPHAARG1,0},{D3DTSS_ALPHAARG2,0},
        };
        for (auto& t : ts) device->GetTextureStageState(0, t.s, &t.v);

        struct SS { D3DSAMPLERSTATETYPE s; DWORD v; };
        SS ss[] = {
            {D3DSAMP_MINFILTER,0},{D3DSAMP_MAGFILTER,0},
            {D3DSAMP_ADDRESSU,0},{D3DSAMP_ADDRESSV,0},
        };
        for (auto& s : ss) device->GetSamplerState(0, s.s, &s.v);

        DWORD alpha = last_.opacity & 0xFF;

        device->SetPixelShader(nullptr);
        device->SetVertexShader(nullptr);
        device->SetTexture(0, tex_);
        device->SetFVF(HOMINKA_D3D9_FVF);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(alpha, 255, 255, 255));
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        // -0.5 — правило вирівнювання текселів у D3D9.
        const float o = -0.5f;
        D3D9Vertex v[4] = {
            {x + o,      y + o,      0.0f, 1.0f, 0.0f, 0.0f},
            {x + ow + o, y + o,      0.0f, 1.0f, 1.0f, 0.0f},
            {x + o,      y + oh + o, 0.0f, 1.0f, 0.0f, 1.0f},
            {x + ow + o, y + oh + o, 0.0f, 1.0f, 1.0f, 1.0f},
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(D3D9Vertex));

        // Повертаємо все як було.
        for (auto& r : rs) device->SetRenderState(r.s, r.v);
        for (auto& t : ts) device->SetTextureStageState(0, t.s, t.v);
        for (auto& s : ss) device->SetSamplerState(0, s.s, s.v);
        device->SetFVF(oldFVF);
        device->SetTexture(0, oldTex);
        device->SetVertexShader(oldVS);
        device->SetPixelShader(oldPS);
        // Get* додає посилання — знімаємо їх.
        if (oldTex) oldTex->Release();
        if (oldVS) oldVS->Release();
        if (oldPS) oldPS->Release();
    }

    void release_texture() {
        if (tex_) { tex_->Release(); tex_ = nullptr; }
        tex_w_ = tex_h_ = 0;
        tex_seq_ = 0;
    }

    SharedFrameReader reader_;
    IDirect3DTexture9* tex_ = nullptr;
    uint32_t tex_w_ = 0, tex_h_ = 0, tex_seq_ = 0;
    bool enabled_ = false;
    bool ready_ = false;
    bool logged_ = false;
    bool pid_warned_ = false;
    FrameView last_;
};

}  // namespace hominka
