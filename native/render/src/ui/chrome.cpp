#include "ui/chrome.h"

#include <cmath>
#include <cstdio>

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

// Смужка згори. 28 пікселів замість колишніх 22: у 22 не вміщалися ані кнопка
// з підписом, ані повзунок — усе доводилося тиснути, і ряд виглядав кривим.
const float BAR_H = 28.0f;
const float BTN_W = 26.0f;
const float BTN_H = 22.0f;
const float BTN_Y = (BAR_H - BTN_H) * 0.5f;
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
    st.FramePadding = ImVec2(6, 3);          // під смужку заввишки 28 пікселів
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
    // Тло й рамка — ДВІ різні речі, і вимикаються вони окремо.
    //
    // Доки це був один прапорець «без рамки», разом із рамкою зникало й тло, а
    // повзунок «Тло» переставав робити будь-що — при тому, що людині якраз і
    // потрібне було тло без рамки, зокрема поверх гри. Тепер тло веде свій
    // повзунок (нуль — немає), рамку веде свій перемикач.
    const bool bg = look.bg_alpha > 0.001f;
    if (!bg && look.frameless) return;

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(d2d->CreateSolidColorBrush(
            d2d_color(12 / 255.0f, 12 / 255.0f, 15 / 255.0f, look.bg_alpha * look.opacity),
            &brush)))
        return;
    const D2D1_ROUNDED_RECT rr =
        D2D1::RoundedRect(D2D1::RectF(1.0f, 1.0f, (float)w - 1.0f, (float)h - 1.0f), 11.0f, 11.0f);
    if (bg) d2d->FillRoundedRectangle(rr, brush);

    if (!look.frameless) {
        const bool lock = look.locked;
        brush->SetColor(d2d_color(lock ? 34 / 255.0f : 168 / 255.0f,
                                  lock ? 197 / 255.0f : 85 / 255.0f,
                                  lock ? 94 / 255.0f : 247 / 255.0f, look.opacity));
        d2d->DrawRoundedRectangle(rr, brush, 2.0f);
    }
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

void Chrome::begin_intro(int ms) { intro_until_ = GetTickCount64() + (unsigned long long)ms; }

bool Chrome::visible(bool locked) const {
    if (locked) return false;          // замкнене вікно керування не показує
    return hovered_ || intro_active();
}

bool Chrome::intro_active() const { return GetTickCount64() < intro_until_; }

namespace {

// Одна кнопка смужки: підкладка при наведенні і місце, у якому малювати
// значок. Раніше кожна кнопка робила це по-своєму — звідси й ряд, у якому
// замок висів вище за «A−», а той не збігався з шестернею.
struct Slot {
    ImVec2 a, b;          // кути кнопки на екрані
    ImVec2 c;             // середина — по ній центрують значок
    bool hot = false;
    bool pressed = false;
};

Slot slot(const char* id, float x, float y, float w, float h, ImU32 hot_bg) {
    Slot s;
    ImGui::SetCursorPos(ImVec2(x, y));
    s.a = ImGui::GetCursorScreenPos();
    s.pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    s.hot = ImGui::IsItemHovered();
    s.b = ImVec2(s.a.x + w, s.a.y + h);
    s.c = ImVec2(s.a.x + w * 0.5f, s.a.y + h * 0.5f);
    if (s.hot)
        ImGui::GetWindowDrawList()->AddRectFilled(s.a, s.b, hot_bg, 5.0f);
    return s;
}

// Підпис усередині кнопки — рівно посередині, а не «як ляже».
void slot_text(const Slot& s, const char* text, ImU32 color) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(s.c.x - sz.x * 0.5f, s.c.y - sz.y * 0.5f), color, text);
}

// Повзунок смужки — свій, а не ImGui::SliderFloat.
//
// Стандартний малює доріжку на всю висоту рядка: у смужці виходила порожня
// коробка з фіолетовою пігулкою всередині, і на повзунок це схоже не було.
// Тут доріжка — риска в три пікселі, пройдена частина підсвічена, ручка —
// кружечок, який більшає під курсором.
bool bar_slider(const char* id, float x, float y, float w, float h, float* v,
                float lo, float hi) {
    ImGui::SetCursorPos(ImVec2(x, y));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool active = ImGui::IsItemActive();
    const bool hot = ImGui::IsItemHovered() || active;

    const float r = 5.5f;
    const float x0 = p.x + r, x1 = p.x + w - r;
    bool changed = false;
    if (active && x1 > x0) {
        float t = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float nv = lo + t * (hi - lo);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    const float t = hi > lo ? (*v - lo) / (hi - lo) : 0.0f;
    const float cy = p.y + h * 0.5f;
    const float kx = x0 + t * (x1 - x0);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), IM_COL32(255, 255, 255, hot ? 64 : 44), 3.0f);
    if (kx > x0) dl->AddLine(ImVec2(x0, cy), ImVec2(kx, cy), ACCENT, 3.0f);
    dl->AddCircleFilled(ImVec2(kx, cy), hot ? r : r - 1.0f, IM_COL32(242, 238, 255, 255));
    return changed;
}

// Значок програми — той самий, що на ярлику: фіолетово-рожевий квадрат зі
// скругленням, у ньому біла бульбашка з трьома крапками. Малюємо, а не
// вантажимо картинку: на вісімнадцяти пікселях від цього нічого не втрачається,
// зате не треба ані файлу поруч, ані текстури в атласі.
void logo(ImDrawList* dl, ImVec2 p, float s) {
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + s, p.y + s),
                                IM_COL32(150, 90, 240, 255), IM_COL32(214, 70, 190, 255),
                                IM_COL32(214, 70, 190, 255), IM_COL32(150, 90, 240, 255));
    // Скруглення підробляємо зверху: AddRectFilledMultiColor кутів не вміє.
    dl->AddRect(p, ImVec2(p.x + s, p.y + s), IM_COL32(0, 0, 0, 0), s * 0.28f, 0, 0.0f);

    const float bx = p.x + s * 0.17f, by = p.y + s * 0.25f;
    const float bw = s * 0.66f, bh = s * 0.42f;
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), IM_COL32(255, 255, 255, 245),
                      bh * 0.42f);
    // Хвостик бульбашки.
    dl->AddTriangleFilled(ImVec2(bx + bw * 0.22f, by + bh - 0.5f),
                          ImVec2(bx + bw * 0.52f, by + bh - 0.5f),
                          ImVec2(bx + bw * 0.24f, by + bh + s * 0.22f),
                          IM_COL32(255, 255, 255, 245));
    const ImU32 dots[3] = {IM_COL32(99, 102, 241, 255), IM_COL32(147, 51, 234, 255),
                           IM_COL32(236, 72, 153, 255)};
    for (int i = 0; i < 3; ++i)
        dl->AddCircleFilled(ImVec2(bx + bw * (0.26f + 0.24f * (float)i), by + bh * 0.5f),
                            s * 0.055f + 0.4f, dots[i], 8);
}

void separator(ImDrawList* dl, float x, float y, float h) {
    dl->AddLine(ImVec2(x, y), ImVec2(x, y + h), IM_COL32(255, 255, 255, 26));
}

}  // namespace

ChromeEvents Chrome::draw_controls(int w, int h, Look* look, HWND hwnd,
                                   const std::string& viewers, bool can_close, bool empty) {
    ChromeEvents ev;
    if (!ready_) return ev;

    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Позицію курсора віддаємо ImGui самі — див. poll_hover().
    ImGui::GetIO().AddMousePosEvent((float)mouse_.x, (float)mouse_.y);
    ImGui::NewFrame();

    const bool show = visible(look->locked);
    // Замкнене вікно кнопок не показує — але в перші секунди після запуску воно
    // все одно має сказати, що воно тут. Інакше замкнена й порожня Hominka на
    // вигляд нічим не відрізняється від незапущеної.
    const bool intro_note = !show && empty && look->locked && intro_active();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::Begin("##chrome", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (show) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 HOT = IM_COL32(255, 255, 255, 30);

        // Смужка згори — і фон під неї, щоб кнопки читалися поверх чату.
        dl->AddRectFilled(ImVec2(2, 2), ImVec2((float)w - 2, BAR_H),
                          IM_COL32(16, 16, 22, 238), 9.0f, ImDrawFlags_RoundCornersTop);
        dl->AddLine(ImVec2(2, BAR_H), ImVec2((float)w - 2, BAR_H),
                    IM_COL32(255, 255, 255, 20));

        // Розкладка ведеться числами, а не ланцюжком SameLine: у ланцюжку
        // висота кожного елемента своя, і ряд «їде» — саме це й було видно.
        // А правий край рахуємо ПЕРШИМ, ще до лівого.
        //
        // Вікно чату буває вузьким — 300 пікселів завширшки цілком звична річ.
        // Доки ряд просто ріс ліворуч, у такому вікні «60%» налізало на
        // шестерню. Тепер спершу відомо, де починається правий край, і ліві
        // елементи додаються, лише поки до нього лишається місце; що не
        // вмістилося — того просто немає, а не намальовано одне поверх одного.
        const ImVec2 vsz = viewers.empty() ? ImVec2(0, 0) : ImGui::CalcTextSize(viewers.c_str());
        float right_w = 6.0f + BTN_W;                       // шестерня
        if (can_close) right_w += BTN_W + 2.0f;
        const bool show_viewers = !viewers.empty() && (float)w > right_w + vsz.x + 150.0f;
        if (show_viewers) right_w += vsz.x + 24.0f;
        const float right_x = (float)w - right_w;

        float x = 6.0f;
        // Скільки ще влізе ліворуч, лишивши місце під смужку перетягування.
        auto fits = [&](float need) { return x + need <= right_x - 24.0f; };

        // Значок і назва — першими. Вікно чату не має ані заголовка, ані рядка
        // в панелі задач, тож інакше воно ніде себе не називає: людина бачить
        // темний прямокутник і кілька кнопок.
        {
            const float LOGO = 16.0f;
            const ImVec2 nsz = ImGui::CalcTextSize("Hominka");
            const bool with_name = fits(LOGO + 6.0f + nsz.x + 10.0f + BTN_W * 3.0f);
            const ImVec2 org = ImGui::GetWindowPos();
            logo(dl, ImVec2(org.x + x, org.y + BTN_Y + (BTN_H - LOGO) * 0.5f), LOGO);
            x += LOGO + 6.0f;
            if (with_name) {
                dl->AddText(ImVec2(org.x + x, org.y + BTN_Y + (BTN_H - nsz.y) * 0.5f),
                            IM_COL32(228, 228, 231, 255), "Hominka");
                x += nsz.x;
            }
            x += 8.0f;
            separator(dl, x, BTN_Y + 3.0f, BTN_H - 6.0f);
            x += 7.0f;
        }

        // Замок: вимикає й вмикає клік-крізь. Малюємо колодку, а не пишемо
        // «[o]»: на смужці підпис прочитати нема коли, а замкнену колодку
        // видно з першого погляду.
        {
            const Slot s = slot("##lock", x, BTN_Y, BTN_W, BTN_H, HOT);
            if (s.pressed) {
                look->locked = !look->locked;
                ev.lock_changed = true;
            }
            if (s.hot)
                ImGui::SetTooltip(look->locked ? "Розімкнути (миша знову діє)"
                                               : "Замкнути (миша проходить крізь)");
            const ImU32 tint = look->locked ? ACCENT_LOCK
                                            : (s.hot ? IM_COL32(255, 255, 255, 255) : TEXT_DIM);
            // Корпус і дужка. Замкнена — дужка стоїть над корпусом рівно;
            // розімкнена — зсунута вбік і не доходить до нього, тобто відкрита
            // саме з одного боку, як у справжньої колодки.
            const float cx = s.c.x, cy = s.c.y + 2.0f;
            dl->AddRectFilled(ImVec2(cx - 5.0f, cy - 1.5f), ImVec2(cx + 5.0f, cy + 5.0f),
                              tint, 1.8f);
            if (look->locked)
                dl->PathArcTo(ImVec2(cx, cy - 1.5f), 3.4f, 3.14159265f, 6.2831853f, 14);
            else
                dl->PathArcTo(ImVec2(cx + 2.6f, cy - 1.5f), 3.4f, 3.14159265f,
                              5.4977871f, 12);
            dl->PathStroke(tint, 0, 1.8f);
            x += BTN_W;
        }

        // Хто поступається місцем, коли вікно вузьке.
        //
        // Питання не абстрактне: у вікні на 370 пікселів разом усе не вміщається
        // ніколи. Повзунки прозорості крутять постійно й лише звідси, а кегль є
        // ще й у налаштуваннях — тож першими рахуємо повзунки, а «A−/A+» беруть
        // те, що лишилося.
        const float ZOOM_NEED = 4.0f + BTN_W * 2.0f + 2.0f;
        const float SLIDER_W = 58.0f;
        const bool values = (float)w >= 470.0f;
        const float ONE = SLIDER_W + (values ? 6.0f + 34.0f : 0.0f) + 10.0f;
        const float SLIDERS_NEED = 11.0f + ONE * 2.0f;
        const bool show_sliders = fits(SLIDERS_NEED);
        const bool show_zoom = fits((show_sliders ? SLIDERS_NEED : 0.0f) + ZOOM_NEED);

        // Кегль. Підпис малюємо самі й по центру кнопки: ImGui::Button ставив
        // текст за своїм відступом, і «A−» з «A+» стояли не на одній лінії з
        // рештою ряду.
        if (show_zoom) {
            x += 4.0f;

            const Slot a = slot("##smaller", x, BTN_Y, BTN_W, BTN_H, HOT);
            if (a.pressed) {
                look->zoom = look->zoom - 0.1f < 0.5f ? 0.5f : look->zoom - 0.1f;
                ev.look_changed = true;
            }
            if (a.hot) ImGui::SetTooltip("Дрібніший текст");
            slot_text(a, "A-", a.hot ? IM_COL32(255, 255, 255, 255) : TEXT_DIM);
            x += BTN_W + 2.0f;

            const Slot b = slot("##bigger", x, BTN_Y, BTN_W, BTN_H, HOT);
            if (b.pressed) {
                look->zoom = look->zoom + 0.1f > 3.0f ? 3.0f : look->zoom + 0.1f;
                ev.look_changed = true;
            }
            if (b.hot) ImGui::SetTooltip("Більший текст");
            slot_text(b, "A+", b.hot ? IM_COL32(255, 255, 255, 255) : TEXT_DIM);
            x += BTN_W;
        }

        // Повзунки прозорості — найчастіше крутять саме їх, тож вони під рукою,
        // а не в налаштуваннях. Число поруч показуємо, лише коли для нього є
        // місце; у підказці воно є завжди.
        {
            if (show_sliders) {
                x += 5.0f;
                separator(dl, x, BTN_Y + 3.0f, BTN_H - 6.0f);
                x += 8.0f;

                struct Bar { const char* id; float* v; float lo, hi; const char* tip; };
                float op = look->opacity * 100.0f, bg = look->bg_alpha * 100.0f;
                const Bar bars[2] = {{"##op", &op, 20.0f, 100.0f, "Прозорість вікна"},
                                     {"##bg", &bg, 0.0f, 100.0f, "Тло під чатом"}};
                for (int i = 0; i < 2; ++i) {
                    if (bar_slider(bars[i].id, x, BTN_Y, SLIDER_W, BTN_H, bars[i].v,
                                   bars[i].lo, bars[i].hi))
                        ev.look_changed = true;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s — %.0f%%", bars[i].tip, *bars[i].v);
                    x += SLIDER_W;

                    if (values) {
                        x += 6.0f;
                        char buf[16];
                        snprintf(buf, sizeof buf, "%.0f%%", *bars[i].v);
                        const ImVec2 sz = ImGui::CalcTextSize(buf);
                        dl->AddText(
                            ImVec2(ImGui::GetWindowPos().x + x,
                                   ImGui::GetWindowPos().y + BTN_Y + (BTN_H - sz.y) * 0.5f),
                            IM_COL32(216, 194, 255, 255), buf);
                        x += 34.0f;
                    }
                    x += 10.0f;
                }
                look->opacity = op / 100.0f;
                look->bg_alpha = bg / 100.0f;
            }
        }

        const float left_end = x;

        // Правий край: хрестик скраю, поруч шестерня, перед ними глядачі.
        float rx = (float)w - 6.0f;

        if (can_close) {
            rx -= BTN_W;
            const Slot s = slot("##close", rx, BTN_Y, BTN_W, BTN_H,
                                IM_COL32(239, 68, 68, 190));
            if (s.pressed) ev.close = true;
            if (s.hot) ImGui::SetTooltip("Закрити Hominka");
            const ImU32 tint = s.hot ? IM_COL32(255, 255, 255, 255) : TEXT_DIM;
            const float r = 4.0f;
            dl->AddLine(ImVec2(s.c.x - r, s.c.y - r), ImVec2(s.c.x + r, s.c.y + r), tint, 1.6f);
            dl->AddLine(ImVec2(s.c.x + r, s.c.y - r), ImVec2(s.c.x - r, s.c.y + r), tint, 1.6f);
            rx -= 2.0f;
        }

        // Шестерня — вхід у налаштування. Малюємо її самі: гліфа шестерні в
        // Segoe UI немає, а «*» на її місці нічого не означає — на нього просто
        // не натискали.
        {
            rx -= BTN_W;
            const Slot s = slot("##gear", rx, BTN_Y, BTN_W, BTN_H, HOT);
            if (s.pressed) ev.open_settings = true;
            if (s.hot) ImGui::SetTooltip("Налаштування");
            const ImU32 tint = s.hot ? IM_COL32(255, 255, 255, 255) : TEXT_DIM;
            // Шість зубців по колу плюс кільце: дрібно, але впізнавано.
            for (int i = 0; i < 6; ++i) {
                const float a = (float)i * 3.14159265f / 3.0f;
                const float cs = cosf(a), sn = sinf(a);
                dl->AddLine(ImVec2(s.c.x + cs * 3.0f, s.c.y + sn * 3.0f),
                            ImVec2(s.c.x + cs * 6.5f, s.c.y + sn * 6.5f), tint, 2.0f);
            }
            dl->AddCircle(s.c, 4.0f, tint, 12, 2.0f);
        }

        // Глядачі — ліворуч від шестерні. Саме тут, а не в налаштуваннях:
        // дивитися на це число хочуть під час ефіру, а не тоді, коли щось
        // налаштовують.
        if (show_viewers) {
            rx -= vsz.x + 18.0f;
            ImGui::SetCursorPos(ImVec2(rx, BTN_Y));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##viewers", ImVec2(vsz.x + 14.0f, BTN_H));
            dl->AddCircleFilled(ImVec2(p.x + 3.0f, p.y + BTN_H * 0.5f), 3.0f,
                                IM_COL32(239, 68, 68, 255));
            dl->AddText(ImVec2(p.x + 12.0f, p.y + (BTN_H - vsz.y) * 0.5f), TEXT_DIM,
                        viewers.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Глядачів зараз");
            rx -= 6.0f;
        }

        // Смужка перетягування — уся вільна частина посередині.
        const float drag_w = rx - left_end - 8.0f;
        if (drag_w > 8.0f) {
            ImGui::SetCursorPos(ImVec2(left_end + 4.0f, BTN_Y));
            ImGui::InvisibleButton("##drag", ImVec2(drag_w, BTN_H));
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
        }

        // Куточок для розтягування. Розмір вікна веде Python (він же його й
        // зберігає), тож тут ми лише повідомляємо новий — а застосує його
        // наступний «config».
        {
            ImGui::SetCursorPos(ImVec2((float)w - GRIP - 2, (float)h - GRIP - 2));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##grip", ImVec2(GRIP, GRIP));
            const bool hot = ImGui::IsItemHovered() || resizing_;
            // Дві риски замість суцільного трикутника: суцільний фіолетовий кут
            // перетягував на себе увагу з чату, заради якого вікно й відкрите.
            const ImU32 tint = look->locked ? ACCENT_LOCK : ACCENT;
            const ImU32 c = hot ? tint : (tint & 0x00FFFFFF) | 0x70000000;
            for (int i = 0; i < 2; ++i) {
                const float o = 4.0f + (float)i * 5.0f;
                dl->AddLine(ImVec2(p.x + GRIP - 2.0f, p.y + GRIP - o),
                            ImVec2(p.x + GRIP - o, p.y + GRIP - 2.0f), c, 2.0f);
            }
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
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Розтягнути вікно");
        }
    }

    // Порожня стрічка — не привід показувати порожнє вікно. Поки нічого не
    // приїхало, пишемо просто, що це і куди натиснути. Замкненому вікну
    // натискати нема куди, тож йому кажемо, як розімкнутися.
    if ((show && empty) || intro_note) {
        const char* lines[3] = {
            "Hominka працює",
            intro_note ? "Вікно замкнене — миша проходить крізь нього."
                       : "Тут з'являтимуться повідомлення чату.",
            intro_note ? "Ctrl+Alt+Space — розімкнути."
                       : "Канали — у налаштуваннях, кнопка згори праворуч."};
        const ImU32 cols[3] = {IM_COL32(228, 228, 231, 255), TEXT_DIM, TEXT_DIM};
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = (float)h * 0.5f - 30.0f;
        for (int i = 0; i < 3; ++i) {
            const ImVec2 sz = ImGui::CalcTextSize(lines[i]);
            dl->AddText(ImVec2(ImGui::GetWindowPos().x + ((float)w - sz.x) * 0.5f,
                               ImGui::GetWindowPos().y + y),
                        cols[i], lines[i]);
            y += sz.y + (i == 0 ? 10.0f : 4.0f);
        }
    }

    ImGui::End();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    return ev;
}

}  // namespace hominka
