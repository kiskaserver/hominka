// Вікно оверлея: D3D11 + DirectComposition + Direct2D, приховане від OBS.
//
// Винесено з dcomp/dcomp_overlay.cpp, бо тепер таких процесів два: старий
// (показує готовий кадр зі спільної памʼяті) і новий рендер чату (малює сам).
// Вікно в них однакове до останнього прапорця, а прапорці ці — не дрібниця:
// кожен тут стоїть із причини, і половина причин з'ясувалася боляче.
//
// Чому саме DirectComposition. У безрамковому повноекранному (Independent
// Flip) звичайне «слоєне» вікно зникає: DWM сканує кадр гри повз композицію
// робочого столу. Вміст DComp-вікна малює flip-свопчейн, і таке вікно DWM може
// покласти на окрему АПАРАТНУ overlay-площину поверх гри. У гру при цьому
// нічого не вкладається — безпечно для античитів.
//
// WDA_EXCLUDEFROMCAPTURE ховає вікно від OBS. Це ІНША річ, ніж видимість
// поверх гри: одне одному не заважає.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d2d1_1.h>

#include <cstring>
#include <vector>

namespace hominka {

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

// Куди віддавати повідомлення вікна до того, як їх візьме DefWindowProc.
// Потрібне для ImGui: його обробник миші живе в render/, а вікно — тут, і
// тягнути ImGui у common/ заради одного виклику не варто. Вікно в процесі
// одне, тож один вказівник — чесно й досить.
using WndMsgHook = LRESULT (*)(HWND, UINT, WPARAM, LPARAM, bool* handled);

class DCompWindow {
public:
    ~DCompWindow() { destroy(); }

    static void set_msg_hook(WndMsgHook hook) { msg_hook_ = hook; }

    // Створює вікно. Клас потрібен назовні: Python шукає вікно саме за ним,
    // щоб поставити його туди ж, де вікно чату.
    bool create(HINSTANCE inst, const wchar_t* cls, const wchar_t* title) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = inst;
        wc.lpszClassName = cls;
        // Курсор класу — звичайна стрілка, і це не дрібниця.
        //
        // Без нього клас віддає NULL, а Windows на NULL не міняє курсор узагалі:
        // над вікном лишається той, що був. Одразу після запуску це «зачекайте»
        // від оболонки — і на вікні чату крутився нескінченний кружечок, доки
        // курсор не сходив на інше вікно й не повертався.
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(
            // TOPMOST      — поверх усього;
            // TOOLWINDOW   — не в панелі задач і не в Alt-Tab;
            // NOACTIVATE   — не забирає фокус у гри;
            // TRANSPARENT  — миша проходить крізь (поки не вимкнемо);
            // NOREDIRECTIONBITMAP — вміст іде лише через DComp, без застарілої
            //                поверхні перенаправлення (інакше DComp не працює).
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT
                | WS_EX_NOREDIRECTIONBITMAP,
            cls, title, WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, inst, nullptr);
        if (!hwnd_) return false;
        SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);   // OBS не бачить
        return true;
    }

    // D3D11 + DirectComposition + Direct2D. Пристрій один на все: Direct2D
    // малює прямо в текстуру свопчейна, тож жодного копіювання між CPU і GPU.
    bool init_gfx() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // без цього D2D не стане
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                     nullptr, 0, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_)))
            return false;

        IDXGIDevice* dxdev = nullptr;
        if (FAILED(dev_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev))) return false;

        IDXGIAdapter* adapter = nullptr;
        dxdev->GetAdapter(&adapter);
        if (adapter) {
            adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory_);
            adapter->Release();
        }

        D2D1_FACTORY_OPTIONS opt = {};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     __uuidof(ID2D1Factory1), &opt, (void**)&d2d_factory_)) ||
            FAILED(d2d_factory_->CreateDevice(dxdev, &d2d_dev_)) ||
            FAILED(d2d_dev_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_))) {
            dxdev->Release();
            return false;
        }

        const bool ok = SUCCEEDED(DCompositionCreateDevice(dxdev, __uuidof(IDCompositionDevice),
                                                           (void**)&dcomp_));
        dxdev->Release();
        if (!ok) return false;
        if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &target_))) return false;
        dcomp_->CreateVisual(&visual_);
        target_->SetRoot(visual_);
        return factory_ != nullptr;
    }

    // Готує свопчейн під розмір w×h і підганяє РОЗМІР вікна (позицію лишаємо
    // тому, хто нас запустив). Свопчейн створюється раз, далі ResizeBuffers.
    //
    // Повертає false і при невдачі, і коли розмір нікчемний.
    bool ensure_size(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (swap_ && w == width_ && h == height_) return true;
        release_target_bitmap();

        if (!swap_) {
            DXGI_SWAP_CHAIN_DESC1 sd = {};
            sd.Width = (UINT)w;
            sd.Height = (UINT)h;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            // Премножена альфа: саме її чекає композитор, і саме тому всі наші
            // пікселі йдуть premultiplied від самого декодування картинок.
            sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            if (FAILED(factory_->CreateSwapChainForComposition(dev_, &sd, nullptr, &swap_)))
                return false;
            visual_->SetContent(swap_);
            dcomp_->Commit();
        } else if (FAILED(swap_->ResizeBuffers(2, (UINT)w, (UINT)h,
                                               DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            return false;
        }

        // Вид для D3D11 — ним малює ImGui вже ПІСЛЯ того, як Direct2D закінчив
        // із чатом. Обидва пишуть у той самий задній буфер, і це нормально,
        // доки між ними стоїть EndDraw (він і скидає все накопичене в D2D).
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
            dev_->CreateRenderTargetView(back, nullptr, &rtv_);
            back->Release();
        }

        IDXGISurface* surface = nullptr;
        if (FAILED(swap_->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surface)) || !surface)
            return false;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        const HRESULT hr = d2d_ctx_->CreateBitmapFromDxgiSurface(surface, &props, &target_bmp_);
        surface->Release();
        if (FAILED(hr)) return false;
        d2d_ctx_->SetTarget(target_bmp_);

        width_ = w;
        height_ = h;
        SetWindowPos(hwnd_, nullptr, 0, 0, w, h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }

    ID2D1DeviceContext* d2d() { return d2d_ctx_; }
    ID3D11Device* d3d() { return dev_; }
    ID3D11DeviceContext* d3d_ctx() { return ctx_; }
    ID3D11RenderTargetView* rtv() { return rtv_; }
    HWND hwnd() const { return hwnd_; }
    int width() const { return width_; }
    int height() const { return height_; }

    void begin_draw() { d2d_ctx_->BeginDraw(); }

    // Закінчує малювання Direct2D і скидає його в задній буфер. Після цього —
    // і тільки після — у той самий буфер можна малювати через D3D11 (ImGui).
    HRESULT end_draw() { return d2d_ctx_->EndDraw(); }

    // Показує намальоване. Present(1,0) — з вертикальною синхронізацією: кадр
    // чату однаково рідкісний, а без неї оверлей смикав би відеокарту.
    HRESULT present() {
        const HRESULT hr = swap_->Present(1, 0);
        dcomp_->Commit();
        return hr;
    }

    // Один прозорий кадр — щоб прибрати з екрана те, що вже показано, і далі
    // не чіпати відеокарту взагалі.
    void present_transparent() {
        if (!swap_ || !d2d_ctx_ || !target_bmp_) return;
        d2d_ctx_->BeginDraw();
        d2d_ctx_->Clear(D2D1::ColorF(0, 0, 0, 0));
        d2d_ctx_->EndDraw();
        swap_->Present(1, 0);
        dcomp_->Commit();
    }

    void show() {
        if (shown_) return;
        ShowWindow(hwnd_, SW_SHOWNA);       // без активації: фокус лишається грі
        shown_ = true;
    }
    void hide() {
        if (!shown_) return;
        ShowWindow(hwnd_, SW_HIDE);
        shown_ = false;
    }
    bool shown() const { return shown_; }

    // Гра в повноекранному сидить у вищому z-band, тож зверху доводиться
    // триматися повторно. Але НЕ щокадру: раз на ~250 мс досить, а 60 разів на
    // секунду лише засмічувало чергу вікон.
    void keep_topmost() {
        if (!shown_) return;
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Клік-крізь. Поки ввімкнено, миша проходить наскрізь і вікно не заважає
    // ані грі, ані робочому столу; вимкнене — вікном можна керувати.
    void set_click_through(bool on) {
        if (on == click_through_) return;
        LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
        click_through_ = on;
    }
    bool click_through() const { return click_through_; }

    // Пересунути вікно, не міняючи розміру. Потрібне для перетягування за
    // смужку: розмір веде вміст, позицію — рука.
    void move_to(int x, int y) {
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Знімає те, що ЗАРАЗ у задньому буфері, — рівно те, що бачить людина.
    //
    // Навіщо: вікно приховане від захоплення екрана (WDA_EXCLUDEFROMCAPTURE),
    // тож звичайним скриншотом його не перевіриш. А перемальовувати вміст
    // окремо «для знімка» — означало б перевіряти не те, що показано: рамку
    // ImGui малює D3D11, і в обхідному шляху її б не було.
    //
    // Викликати ПІСЛЯ present(): у flip-моделі після показу задній буфер уже
    // інший, тому знімаємо перед ним.
    bool capture(std::vector<uint8_t>* bgra) {
        if (!swap_ || !dev_ || !ctx_ || width_ <= 0 || height_ <= 0) return false;
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back)
            return false;

        D3D11_TEXTURE2D_DESC td = {};
        back->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        bool ok = false;
        if (SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &staging)) && staging) {
            ctx_->CopyResource(staging, back);
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                bgra->assign((size_t)width_ * height_ * 4, 0);
                for (int y = 0; y < height_; ++y)
                    memcpy(bgra->data() + (size_t)y * width_ * 4,
                           (const uint8_t*)m.pData + (size_t)y * m.RowPitch,
                           (size_t)width_ * 4);
                ctx_->Unmap(staging, 0);
                ok = true;
            }
            staging->Release();
        }
        back->Release();
        return ok;
    }

    // Рамка вікна як частки СВОГО монітора: (x, y, w, h) ∈ [0..1].
    //
    // Саме вона стає розкладкою чату всередині гри: де й якого розміру вікно
    // стоїть на своєму екрані — там і такого ж розміру чат у кадрі гри. На
    // одному моніторі виходить точь-у-точь; якщо вікно на іншому екрані, ніж
    // гра, та сама частка застосовується до монітора гри.
    void monitor_fraction(float* x, float* y, float* w, float* h) const {
        *x = 0.72f; *y = 0.06f; *w = 0.24f; *h = 0.40f;   // якщо монітор не впізнали
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!mon || !GetMonitorInfoW(mon, &mi)) return;
        RECT r = {0, 0, 0, 0};
        if (!GetWindowRect(hwnd_, &r)) return;
        const float mw = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
        const float mh = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
        if (mw <= 0 || mh <= 0) return;
        auto clamp = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
        *x = clamp((r.left - mi.rcMonitor.left) / mw);
        *y = clamp((r.top - mi.rcMonitor.top) / mh);
        *w = clamp((r.right - r.left) / mw);
        *h = clamp((r.bottom - r.top) / mh);
    }

    // Де вікно зараз на екрані.
    RECT screen_rect() const {
        RECT r = {0, 0, 0, 0};
        GetWindowRect(hwnd_, &r);
        return r;
    }

    void destroy() {
        release_target_bitmap();
        if (d2d_ctx_) { d2d_ctx_->Release(); d2d_ctx_ = nullptr; }
        if (d2d_dev_) { d2d_dev_->Release(); d2d_dev_ = nullptr; }
        if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }
        if (visual_) { visual_->Release(); visual_ = nullptr; }
        if (target_) { target_->Release(); target_ = nullptr; }
        if (dcomp_) { dcomp_->Release(); dcomp_ = nullptr; }
        if (swap_) { swap_->Release(); swap_ = nullptr; }
        if (factory_) { factory_->Release(); factory_ = nullptr; }
        if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
        if (dev_) { dev_->Release(); dev_ = nullptr; }
    }

private:
    static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (msg_hook_) {
            bool handled = false;
            const LRESULT r = msg_hook_(h, m, w, l, &handled);
            if (handled) return r;
        }
        if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(h, m, w, l);
    }

    static WndMsgHook msg_hook_;

    void release_target_bitmap() {
        if (d2d_ctx_) d2d_ctx_->SetTarget(nullptr);
        if (target_bmp_) { target_bmp_->Release(); target_bmp_ = nullptr; }
        if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    }

    HWND hwnd_ = nullptr;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGIFactory2* factory_ = nullptr;
    IDXGISwapChain1* swap_ = nullptr;
    IDCompositionDevice* dcomp_ = nullptr;
    IDCompositionTarget* target_ = nullptr;
    IDCompositionVisual* visual_ = nullptr;
    ID2D1Factory1* d2d_factory_ = nullptr;
    ID2D1Device* d2d_dev_ = nullptr;
    ID2D1DeviceContext* d2d_ctx_ = nullptr;
    ID2D1Bitmap1* target_bmp_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    int width_ = 0, height_ = 0;
    bool shown_ = false;
    // Вікно створюється клік-крізь (WS_EX_TRANSPARENT у стилях) — тримаємо це
    // в полі, щоб зайвий SetWindowLongPtr не смикав вікно щокадру.
    bool click_through_ = true;
};

inline WndMsgHook DCompWindow::msg_hook_ = nullptr;

}  // namespace hominka
