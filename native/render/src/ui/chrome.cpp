#include "ui/chrome.h"

#include <cmath>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

#include "ui/settings_ui.h"
#include "ui/uifont.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace hominka {

namespace {

// Ті самі кольори, що й у Qt-вікні (hominka/styles.py): фіолетовий акцент,
// зелений — коли вікно замкнене (клік-крізь). Люди впізнають вікно по них.
const ImU32 ACCENT       = IM_COL32(168, 85, 247, 255);   // #a855f7
const ImU32 ACCENT_LOCK  = IM_COL32(34, 197, 94, 255);    // #22c55e
const ImU32 TEXT_DIM     = IM_COL32(190, 190, 200, 255);

// Висота смужки згори. Та сама, що в DragBar.
const float BAR_H = 22.0f;
// Куточок для розтягування.
const float GRIP = 16.0f;

D2D1_COLOR_F d2d_color(float r, float g, float b, float a) {
    // Малюємо в premultiplied-ціль, тож колір теж має бути премножений —
    // інакше напівпрозора підкладка світилася б яскравіше, ніж задано.
    return D2D1::ColorF(r * a, g * a, b * a, a);
}

}  // namespace

namespace {
// Контекст рамки — окремо, для статичного обробника повідомлень: «себе» він не
// має, а повідомлення вікна чату мусять потрапити саме сюди.
ImGuiContext* g_chrome_ctx = nullptr;
}  // namespace

bool Chrome::init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    IMGUI_CHECKVERSION();
    ctx_ = ImGui::CreateContext();
    g_chrome_ctx = ctx_;
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    // Ані ini, ані log: оверлей не має лишати файлів у теці, звідки його
    // запустили.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Клавіатури тут немає навмисно: вікно створене з WS_EX_NOACTIVATE, щоб
    // не забирати фокус у гри, а отже й клавіш воно не отримує.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // Шрифт із системи, з кирилицею — див. uifont.h.
    load_ui_font(15.0f);

    // Той самий вигляд, що й у панелі налаштувань: кольори, скруглення,
    // повзунки. Рамка чату й панель — одна програма, і синій повзунок ImGui за
    // замовчуванням посеред фіолетового робив із них дві.
    settings_style();
    ImGuiStyle& st = ImGui::GetStyle();
    st.FramePadding = ImVec2(6, 3);          // смужка заввишки 22 пікселі
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);   // тло малює Direct2D

    if (!ImGui_ImplWin32_Init(hwnd)) return false;
    if (!ImGui_ImplDX11_Init(dev, ctx)) return false;
    ready_ = true;
    return true;
}

void Chrome::shutdown() {
    if (!ready_) return;
    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(ctx_);
    ctx_ = nullptr;
    g_chrome_ctx = nullptr;
    ready_ = false;
}

LRESULT Chrome::msg_hook(HWND h, UINT m, WPARAM w, LPARAM l, bool* handled) {
    *handled = false;
    if (!g_chrome_ctx) return 0;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_chrome_ctx);
    const LRESULT r = ImGui_ImplWin32_WndProcHandler(h, m, w, l);
    ImGui::SetCurrentContext(prev);
    if (r) *handled = true;
    return r;
}

bool Chrome::wants_mouse() const {
    if (!ready_ || !ctx_) return false;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx_);
    const bool want = ImGui::GetIO().WantCaptureMouse;
    ImGui::SetCurrentContext(prev);
    return want || dragging_ || resizing_;
}

void Chrome::draw_backdrop(ID2D1DeviceContext* d2d, int w, int h, const Look& look) const {
    if (look.frameless) return;      // «лише повідомлення» — жодної підкладки

    // Підкладка й рамка — точно як у Qt-вікні (hominka/look.py:_apply_border):
    // rgba(12,12,15, bg_alpha), рамка 2 px акцентом, скруглення 11 px.
    const float a = look.bg_alpha * look.opacity;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(d2d->CreateSolidColorBrush(d2d_color(12 / 255.0f, 12 / 255.0f, 15 / 255.0f, a),
                                          &brush)))
        return;
    const D2D1_ROUNDED_RECT rr =
        D2D1::RoundedRect(D2D1::RectF(1.0f, 1.0f, (float)w - 1.0f, (float)h - 1.0f), 11.0f, 11.0f);
    d2d->FillRoundedRectangle(rr, brush);

    const bool lock = look.locked;
    brush->SetColor(d2d_color(lock ? 34 / 255.0f : 168 / 255.0f,
                              lock ? 197 / 255.0f : 85 / 255.0f,
                              lock ? 94 / 255.0f : 247 / 255.0f, look.opacity));
    d2d->DrawRoundedRectangle(rr, brush, 2.0f);
    brush->Release();
}

// Наведення рахуємо САМІ, а не питаємо ImGui.
//
// Поки вікно клік-крізь (WS_EX_TRANSPARENT), воно не отримує жодного
// повідомлення миші, а вбудований у ImGui шлях бере позицію лише у
// сфокусованого вікна — наше ж фокусу не бере ніколи (WS_EX_NOACTIVATE).
// Інакше курсор для ImGui назавжди лишався б «у нескінченності»: рамка не
// з'явилася б, клік-крізь не вимкнувся — і вікном не можна було б
// скористатися взагалі.
bool Chrome::poll_hover(HWND hwnd) {
    POINT cur;
    RECT wr;
    if (!GetCursorPos(&cur) || !GetWindowRect(hwnd, &wr)) {
        hovered_ = false;
        return false;
    }
    mouse_.x = cur.x - wr.left;
    mouse_.y = cur.y - wr.top;
    hovered_ = PtInRect(&wr, cur) != FALSE;
    return hovered_;
}

ChromeEvents Chrome::draw_controls(int w, int h, Look* look, HWND hwnd) {
    ChromeEvents ev;
    if (!ready_) return ev;

    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Позицію курсора віддаємо ImGui самі — див. poll_hover().
    ImGui::GetIO().AddMousePosEvent((float)mouse_.x, (float)mouse_.y);
    ImGui::NewFrame();

    // Поки вікно замкнене (клік-крізь) або миші немає — керувати нічим:
    // постійна смужка поверх гри це шум, а не зручність.
    const bool show = hovered_ && !look->locked;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::Begin("##chrome", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (show) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Смужка згори — і фон під неї, щоб кнопки читалися поверх чату.
        dl->AddRectFilled(ImVec2(2, 2), ImVec2((float)w - 2, BAR_H),
                          IM_COL32(18, 18, 24, 230), 9.0f, ImDrawFlags_RoundCornersTop);

        ImGui::SetCursorPos(ImVec2(6, 3));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 40));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 70));

        // Замок: вимикає й вмикає клік-крізь. Малюємо колодку, а не пишемо
        // «[o]»: на смужці в двадцять два пікселі підпис прочитати нема коли,
        // а замкнену колодку видно з першого погляду.
        {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton("##lock", ImVec2(26, BAR_H - 6));
            const bool hot = ImGui::IsItemHovered();
            if (pressed) {
                look->locked = !look->locked;
                ev.lock_changed = true;
            }
            if (hot)
                ImGui::SetTooltip(look->locked ? "Розімкнути (миша знову діє)"
                                               : "Замкнути (миша проходить крізь)");
            if (hot)
                dl->AddRectFilled(p0, ImVec2(p0.x + 26, p0.y + BAR_H - 6),
                                  IM_COL32(255, 255, 255, 40), 4.0f);
            const ImU32 tint = look->locked ? ACCENT_LOCK
                                            : (hot ? IM_COL32(255, 255, 255, 255) : TEXT_DIM);
            const float cx = p0.x + 13.0f, cy = p0.y + (BAR_H - 6) * 0.5f;
            // Корпус і дужка. Розімкнена — дужка зсунута вбік і не замикається.
            dl->AddRectFilled(ImVec2(cx - 4.5f, cy - 1.0f), ImVec2(cx + 4.5f, cy + 5.5f),
                              tint, 1.5f);
            dl->PathArcTo(ImVec2(look->locked ? cx : cx + 3.0f, cy - 1.0f), 3.2f,
                          3.14159265f, 6.2831853f, 12);
            dl->PathStroke(tint, 0, 1.6f);
        }

        ImGui::SameLine(0, 2);
        if (ImGui::Button("A−", ImVec2(26, BAR_H - 6))) {
            look->zoom = look->zoom - 0.1f < 0.5f ? 0.5f : look->zoom - 0.1f;
            ev.look_changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Дрібніший текст");
        ImGui::SameLine(0, 2);
        if (ImGui::Button("A+", ImVec2(26, BAR_H - 6))) {
            look->zoom = look->zoom + 0.1f > 3.0f ? 3.0f : look->zoom + 0.1f;
            ev.look_changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Більший текст");

        // Повзунки прозорості — найчастіше крутять саме їх, тож вони під рукою,
        // а не в налаштуваннях. У відсотках: «0.60» ні про що не каже.
        ImGui::SameLine(0, 8);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 25));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ACCENT);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(192, 132, 252, 255));
        {
            float op = look->opacity * 100.0f;
            ImGui::SetNextItemWidth(66);
            if (ImGui::SliderFloat("##op", &op, 20.0f, 100.0f, "%.0f%%")) {
                look->opacity = op / 100.0f;
                ev.look_changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Прозорість вікна");

            ImGui::SameLine(0, 4);
            float bg = look->bg_alpha * 100.0f;
            ImGui::SetNextItemWidth(66);
            if (ImGui::SliderFloat("##bg", &bg, 0.0f, 100.0f, "%.0f%%")) {
                look->bg_alpha = bg / 100.0f;
                ev.look_changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Тло під чатом");
        }
        ImGui::PopStyleColor(3);

        // Шестерня — вхід у налаштування. Малюємо її самі: гліфа шестерні в
        // Segoe UI немає, а «*» на її місці нічого не означає — на нього просто
        // не натискали.
        ImGui::SameLine(0, 8);
        {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton("##gear", ImVec2(24, BAR_H - 6));
            const bool hot = ImGui::IsItemHovered();
            if (pressed) ev.open_settings = true;
            if (hot) ImGui::SetTooltip("Налаштування");
            if (hot)
                dl->AddRectFilled(p0, ImVec2(p0.x + 24, p0.y + BAR_H - 6),
                                  IM_COL32(255, 255, 255, 40), 4.0f);

            const ImVec2 c(p0.x + 12.0f, p0.y + (BAR_H - 6) * 0.5f);
            const ImU32 tint = hot ? IM_COL32(255, 255, 255, 255) : TEXT_DIM;
            // Шість зубців по колу плюс кільце: дрібно, але впізнавано.
            for (int i = 0; i < 6; ++i) {
                const float a = (float)i * 3.14159265f / 3.0f;
                const float cs = cosf(a), sn = sinf(a);
                dl->AddLine(ImVec2(c.x + cs * 3.0f, c.y + sn * 3.0f),
                            ImVec2(c.x + cs * 6.5f, c.y + sn * 6.5f), tint, 2.0f);
            }
            dl->AddCircle(c, 4.0f, tint, 12, 2.0f);
        }

        ImGui::PopStyleColor(3);

        // Смужка перетягування — уся вільна частина зверху.
        const float used = ImGui::GetCursorPosX();
        ImGui::SetCursorPos(ImVec2(used + 4, 3));
        ImGui::InvisibleButton("##drag",
                               ImVec2((float)w - used - 10 > 8 ? (float)w - used - 10 : 8,
                                      BAR_H - 6));
        if (ImGui::IsItemActive()) {
            if (!dragging_) {
                dragging_ = true;
                GetCursorPos(&drag_anchor_);
                GetWindowRect(hwnd, &drag_origin_);
            }
            POINT now;
            GetCursorPos(&now);
            SetWindowPos(hwnd, nullptr,
                         drag_origin_.left + (now.x - drag_anchor_.x),
                         drag_origin_.top + (now.y - drag_anchor_.y),
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (dragging_) {
            dragging_ = false;
            ev.geometry_changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Перетягнути вікно");

        // Куточок для розтягування. Розмір вікна веде Python (він же його й
        // зберігає), тож тут ми лише повідомляємо новий — а застосує його
        // наступний «config».
        dl->AddTriangleFilled(ImVec2((float)w - 3, (float)h - 3),
                              ImVec2((float)w - GRIP, (float)h - 3),
                              ImVec2((float)w - 3, (float)h - GRIP),
                              look->locked ? ACCENT_LOCK : ACCENT);
        ImGui::SetCursorPos(ImVec2((float)w - GRIP - 2, (float)h - GRIP - 2));
        ImGui::InvisibleButton("##grip", ImVec2(GRIP, GRIP));
        if (ImGui::IsItemActive()) {
            if (!resizing_) {
                resizing_ = true;
                GetCursorPos(&resize_anchor_);
                resize_w_ = w;
                resize_h_ = h;
            }
            POINT now;
            GetCursorPos(&now);
            const int nw = resize_w_ + (now.x - resize_anchor_.x);
            const int nh = resize_h_ + (now.y - resize_anchor_.y);
            SetWindowPos(hwnd, nullptr, 0, 0, nw < 180 ? 180 : nw, nh < 120 ? 120 : nh,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (resizing_) {
            resizing_ = false;
            ev.geometry_changed = true;
        }
    }

    ImGui::End();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    return ev;
}

}  // namespace hominka
