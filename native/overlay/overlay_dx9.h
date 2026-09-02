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
    // Читає кадр і оновлює текстуру. Повертає true, якщо є що малювати. obs —
    // чи OBS зараз захоплює (визначає режим приховування). Викликається з
    // hooked_endscene ДО original EndScene (усе — в СЦЕНІ ГРИ, без своєї сцени).
    bool prepare(IDirect3DDevice9* device, bool obs) {
        hide_ = false;
        if (!reader_.ensure_open()) return false;

        FrameView f;
        bool got = reader_.read(&f);
        if (got && !logged_) {
            logged_ = true;
            log("overlay(dx9): кадр — enabled=%u target=%u ми=%u розмір=%ux%u",
                (unsigned)f.enabled, f.target_pid, (unsigned)GetCurrentProcessId(),
                f.width, f.height);
        }
        if (got) {
            if (f.target_pid && f.target_pid != GetCurrentProcessId()) {
                if (!pid_warned_) { pid_warned_ = true;
                    log("overlay(dx9): НЕ малюю — ціль pid=%u, а ми pid=%u",
                        f.target_pid, (unsigned)GetCurrentProcessId()); }
                return false;
            }
            if (!f.enabled) { enabled_ = false; return false; }
            enabled_ = true;
            if (f.seq != tex_seq_ || f.width != tex_w_ || f.height != tex_h_ || !tex_)
                if (!upload(device, f)) return false;
            last_ = f;
        } else if (!enabled_ || tex_seq_ == 0) {
            return false;
        }
        if (!tex_ || tex_w_ == 0) return false;

        hide_ = (last_.hide_from_obs != 0) && obs;
        return true;
    }

    bool wants_hide() const { return hide_; }
    IDirect3DSurface9* obs_backbuffer() const { return last_bb_; }

    // Показ (не ховаємо): чат прямо в сцену гри. Викликається ДО original EndScene.
    void draw(IDirect3DDevice9* device) { blit(device); }

    // Приховування, крок А (в СЦЕНІ ГРИ, ДО чату): знімок ЧИСТОГО кадру. StretchRect
    // у сцені заборонений, лише якщо бекбуфер — поточний RT; тож на мить уводимо RT
    // на запасну поверхню (і повертаємо RT+viewport). MSAA-бекбуфер StretchRect сам
    // розхлопує в non-MSAA знімок. Fail-safe: не вдалось — просто не ховаємо.
    void hide_snapshot_clean(IDirect3DDevice9* device) {
        last_bb_ = nullptr;
        IDirect3DSurface9* bb = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
        if (!ensure_surfaces(device, bb)) { bb->Release(); return; }
        IDirect3DSurface9* saved = nullptr; device->GetRenderTarget(0, &saved);
        D3DVIEWPORT9 vp; bool haveVp = SUCCEEDED(device->GetViewport(&vp));
        // Уводимо RT на запасну → бекбуфер більше не поточний RT → StretchRect можна.
        if (SUCCEEDED(device->SetRenderTarget(0, dummy_))) {
            device->StretchRect(bb, nullptr, clean_, nullptr, D3DTEXF_NONE);
            device->SetRenderTarget(0, saved);          // повертаємо RT гри
            if (haveVp) device->SetViewport(&vp);        // SetRenderTarget скинув viewport
        }
        if (saved) saved->Release();
        stash_bb_ = bb;   // для кроку B (знімок з чатом); Release там
    }

    // Крок Б (ПІСЛЯ original EndScene, поза сценою): знімок кадру З ЧАТОМ.
    void hide_snapshot_dirty(IDirect3DDevice9* device) {
        if (!stash_bb_) return;
        device->StretchRect(stash_bb_, nullptr, dirty_, nullptr, D3DTEXF_NONE);
        last_bb_ = stash_bb_;   // сирий покажчик для звірки в хуку копії OBS
        stash_bb_->Release();
        stash_bb_ = nullptr;
    }

    // У хуку копії OBS: ПЕРЕД копією — чистий кадр у бекбуфер, ПІСЛЯ — з чатом.
    void obs_copy_before(IDirect3DDevice9* device, IDirect3DSurface9* bb) {
        if (hide_ready_ && clean_) device->StretchRect(clean_, nullptr, bb, nullptr, D3DTEXF_NONE);
    }
    void obs_copy_after(IDirect3DDevice9* device, IDirect3DSurface9* bb) {
        if (hide_ready_ && dirty_) device->StretchRect(dirty_, nullptr, bb, nullptr, D3DTEXF_NONE);
    }

    void release() {
        release_texture();
        release_surfaces();
        reader_.close();
        ready_ = false;
    }

    // Перед Reset пристрою ресурси D3DPOOL_DEFAULT недійсні — звільняємо текстуру
    // Й знімки, вони пересоздадуться самі.
    void on_lost() {
        release_texture();
        release_surfaces();
        last_bb_ = nullptr;
        if (stash_bb_) { stash_bb_->Release(); stash_bb_ = nullptr; }
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
        float x, y, ow, oh;   // рамка чату — частки кадру, масштабуємо під гру
        last_.rect((float)vp.Width, (float)vp.Height, &x, &y, &ow, &oh);

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

    // Три RT-поверхні розміром із бекбуфер (non-MSAA): clean_ (чистий знімок),
    // dirty_ (з чатом), dummy_ (щоб на мить увести RT для StretchRect у сцені).
    // MSAA у бекбуфера не заважає: StretchRect у non-MSAA знімок сам розхлопує.
    bool ensure_surfaces(IDirect3DDevice9* device, IDirect3DSurface9* bb) {
        D3DSURFACE_DESC d;
        if (FAILED(bb->GetDesc(&d))) return false;
        if (hide_ready_ && d.Width == surf_w_ && d.Height == surf_h_) return true;
        release_surfaces();
        IDirect3DSurface9** surfs[] = { &clean_, &dirty_, &dummy_ };
        for (auto pp : surfs) {
            if (FAILED(device->CreateRenderTarget(d.Width, d.Height, d.Format,
                    D3DMULTISAMPLE_NONE, 0, FALSE, pp, nullptr))) {
                release_surfaces(); return false;
            }
        }
        surf_w_ = d.Width; surf_h_ = d.Height;
        hide_ready_ = true;
        log("overlay(dx9): приховування від OBS готове (знімки %ux%u, формат=%d)",
            d.Width, d.Height, (int)d.Format);
        return true;
    }

    void release_surfaces() {
        if (clean_) { clean_->Release(); clean_ = nullptr; }
        if (dirty_) { dirty_->Release(); dirty_ = nullptr; }
        if (dummy_) { dummy_->Release(); dummy_ = nullptr; }
        surf_w_ = surf_h_ = 0;
        hide_ready_ = false;
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

    // Приховування від OBS (див. hide_snapshot_*): знімки + запасний RT.
    IDirect3DSurface9* clean_ = nullptr;
    IDirect3DSurface9* dirty_ = nullptr;
    IDirect3DSurface9* dummy_ = nullptr;
    IDirect3DSurface9* stash_bb_ = nullptr;   // бекбуфер між кроком А і Б
    IDirect3DSurface9* last_bb_ = nullptr;    // для звірки в хуку копії OBS
    uint32_t surf_w_ = 0, surf_h_ = 0;
    bool hide_ = false;
    bool hide_ready_ = false;
};

}  // namespace hominka
