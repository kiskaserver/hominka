#include "ui/gui_win.h"

#include <dwmapi.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"
#include "ui/settings_ui.h"
#include "ui/uifont.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace hominka {

namespace {

// Скільки лишати від краю екрана, коли вікно не влазить поруч із чатом.
const int kScreenPad = 8;
// Відступ від вікна чату — той самий, що був у Qt-панелі.
const int kGap = 10;

}  // namespace

GuiWindow::~GuiWindow() { destroy(); }

LRESULT CALLBACK GuiWindow::wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    GuiWindow* self = (GuiWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    // Повідомлення віддаємо ImGui в ЙОГО контексті: у процесі їх два (рамка
    // чату й ця панель), і без перемикання натискання прилітали б не туди.
    if (self && self->imgui_) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(self->imgui_);
        const LRESULT r = ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        ImGui::SetCurrentContext(prev);
        if (r) return r;
    }
    switch (m) {
    case WM_NCCALCSIZE:
        // Неклієнтської області немає взагалі: клієнт займає все вікно.
        //
        // Це і є та «біла смужка згори» в редакторі теми. Вікно з рамкою для
        // розтягування (WS_THICKFRAME) отримує від Windows неклієнтську смугу
        // в кілька пікселів, і система малює її СВОЇМ кольором — у світлій темі
        // майже білим. Колір рамки DWM тут ні до чого: то лінія в один піксель,
        // а це ціла смуга. У вікна налаштувань її не було саме тому, що воно
        // не розтягується.
        if (w) return 0;
        break;

    case WM_NCHITTEST: {
        // Розтягування лишається, але межі доводиться рахувати самим: системних
        // більше немає, а віддати мишу цілком клієнту означало б вікно, яке не
        // змінити в розмірі.
        if (!self || !self->resizable_) break;
        RECT r;
        if (!GetWindowRect(h, &r)) break;
        const int x = (int)(short)LOWORD(l), y = (int)(short)HIWORD(l);
        const int E = 6;                       // ширина смуги для захоплення
        const bool L = x < r.left + E, R = x >= r.right - E;
        const bool T = y < r.top + E, B = y >= r.bottom - E;
        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT;
        if (R) return HTRIGHT;
        if (T) return HTTOP;
        if (B) return HTBOTTOM;
        return HTCLIENT;
    }

    case WM_CLOSE:
        // Панель не закривають назовсім — її ховають: вікно чату живе далі.
        if (self) self->hide();
        return 0;
    case WM_SIZE:
        if (self && self->swap_ && w != SIZE_MINIMIZED)
            self->ensure_size(LOWORD(l), HIWORD(l));
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool GuiWindow::create(const wchar_t* cls, const wchar_t* title, int w, int h,
                       bool resizable, bool mono) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    width_ = w;
    height_ = h;
    resizable_ = resizable;
    // Без рамки: заголовок і хрестик малюємо самі — так само, як це робила
    // Qt-панель, і так вікно виглядає однією річчю з чатом, а не гостем із
    // системного оформлення.
    // WS_THICKFRAME без заголовка — це саме «рамка, за яку тягнуть»: розміром
    // керує система, а виглядом вікна далі керуємо ми.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls, title,
                            WS_POPUP | (resizable ? WS_THICKFRAME : 0),
                            100, 100, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    // Далі всюди йдеться про КЛІЄНТСЬКУ частину: саме її розкладає ImGui і саме
    // такий свопчейн робить DXGI. У вікна з рамкою вона менша за саме вікно, і
    // сплутати їх означає малювати повз екран, а читати — повз буфер.
    {
        RECT rc = {0, 0, 0, 0};
        if (GetClientRect(hwnd_, &rc)) {
            width_ = rc.right - rc.left;
            height_ = rc.bottom - rc.top;
        }
    }
    // OBS не бачить і вікно налаштувань: показувати глядачам, як крутять
    // повзунки, ні до чого.
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    {
        // Скруглені кути там, де система це вміє (Windows 11). На 10-й виклик
        // просто нічого не робить.
        const DWORD kRound = 2;   // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                              &kRound, sizeof kRound);

        // І тонка рамка DWM — теж геть. Це не та сама «біла смужка», що була у
        // редакторі теми (ту малювала неклієнтська область, див. WM_NCCALCSIZE),
        // але лінія по краю тут однаково зайва. На Windows 10 виклик просто
        // нічого не робить.
        const COLORREF kNone = 0xFFFFFFFE;   // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(hwnd_, 34 /*DWMWA_BORDER_COLOR*/, &kNone, sizeof kNone);
    }
    return init_gfx(mono);
}

bool GuiWindow::init_gfx(bool mono) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL got;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                             want, 2, D3D11_SDK_VERSION, &sd, &swap_, &dev_,
                                             &got, &ctx_)))
        return false;

    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        dev_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
    }

    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;      // оверлей не лишає файлів там, звідки його запустили
    io.LogFilename = nullptr;
    // 17 пікселів у панелях: тут читають речення, а не підписи на кнопках, і
    // саме тут дрібний шрифт втомлює найшвидше.
    load_ui_font(17.0f);
    if (mono) load_mono_font(16.0f);
    settings_style();
    const bool ok = ImGui_ImplWin32_Init(hwnd_) && ImGui_ImplDX11_Init(dev_, ctx_);
    ImGui::SetCurrentContext(prev);
    return ok && rtv_ != nullptr;
}

void GuiWindow::release_rtv() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

bool GuiWindow::ensure_size(int w, int h) {
    if (w <= 0 || h <= 0 || !swap_) return false;
    if (w == width_ && h == height_ && rtv_) return true;
    release_rtv();
    if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return false;
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        dev_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
    }
    width_ = w;
    height_ = h;
    return rtv_ != nullptr;
}

void GuiWindow::want_height(int h) {
    if (!hwnd_ || h <= 0) return;
    // Не вище екрана: панель, що вилізла за нижній край, ховає власні кнопки.
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) {
        const int avail = (mi.rcWork.bottom - mi.rcWork.top) - kScreenPad * 2;
        if (h > avail) h = avail;
    }
    if (h == height_) return;
    SetWindowPos(hwnd_, nullptr, 0, 0, width_, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void GuiWindow::show_beside(const RECT& anchor) {
    if (!hwnd_) return;
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);

    // Праворуч від чату, а якщо там не влазить — ліворуч. Поверх чату не
    // кладемо ніколи: налаштування крутять, дивлячись у чат.
    int x = anchor.right + kGap;
    if (x + width_ > mi.rcWork.right - kScreenPad) {
        x = anchor.left - kGap - width_;
        if (x < mi.rcWork.left + kScreenPad) x = mi.rcWork.right - width_ - kScreenPad;
    }
    int y = anchor.top;
    if (y + height_ > mi.rcWork.bottom - kScreenPad) y = mi.rcWork.bottom - height_ - kScreenPad;
    if (y < mi.rcWork.top + kScreenPad) y = mi.rcWork.top + kScreenPad;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    visible_ = true;
}

void GuiWindow::drag(bool active) {
    if (!hwnd_) return;
    if (!active) { dragging_ = false; return; }
    if (!dragging_) {
        dragging_ = true;
        GetCursorPos(&drag_anchor_);
        GetWindowRect(hwnd_, &drag_origin_);
        return;
    }
    POINT now;
    GetCursorPos(&now);
    SetWindowPos(hwnd_, nullptr,
                 drag_origin_.left + (now.x - drag_anchor_.x),
                 drag_origin_.top + (now.y - drag_anchor_.y),
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void GuiWindow::hide() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

bool GuiWindow::begin() {
    if (!visible_ || !imgui_ || !rtv_) return false;
    ImGui::SetCurrentContext(imgui_);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    return true;
}

void GuiWindow::end(bool present_now) {
    ImGui::Render();
    const float clear[4] = {0.055f, 0.059f, 0.070f, 1.0f};
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (present_now) present();
}

void GuiWindow::present() {
    if (swap_) swap_->Present(1, 0);
}

bool GuiWindow::capture(std::vector<uint8_t>* bgra, int* out_w, int* out_h) {
    if (!swap_ || !dev_ || !ctx_) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    back->GetDesc(&desc);
    // Розмір беремо з САМОГО буфера, а не з нашого уявлення про вікно.
    const int bw = (int)desc.Width, bh = (int)desc.Height;
    *out_w = bw;
    *out_h = bh;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev_->CreateTexture2D(&desc, nullptr, &staging)) && staging) {
        ctx_->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            bgra->assign((size_t)bw * bh * 4, 0);
            for (int y = 0; y < bh; ++y) {
                const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                uint8_t* dst = bgra->data() + (size_t)y * bw * 4;
                // Буфер тут RGBA (на відміну від вікна чату) — переставляємо
                // канали, бо PNG ми пишемо як BGRA.
                for (int x = 0; x < bw; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
            ctx_->Unmap(staging, 0);
            ok = true;
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

void GuiWindow::destroy() {
    if (imgui_) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(imgui_);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(imgui_);
        // Той, що був до нас, міг бути саме нашим — тоді відновлювати нічого.
        ImGui::SetCurrentContext(prev == imgui_ ? nullptr : prev);
        imgui_ = nullptr;
    }
    release_rtv();
    if (swap_) { swap_->Release(); swap_ = nullptr; }
    if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
    if (dev_) { dev_->Release(); dev_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    visible_ = false;
}

}  // namespace hominka
