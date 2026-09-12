#include "ui/chrome_bl.h"

#include <cmath>
#include <cstring>

#include "gfx/cssbits.h"

namespace hominka {

namespace {

// Ті самі числа, що й у віконної рамки (chrome.cpp): смужка 22 px, кнопки
// трохи нижчі за неї. Розбіжність тут одразу читалася б як «на Linux усе
// інакше», хоча йдеться про ту саму програму.
// Ті самі числа, що у віконної рамки (ui/chrome.cpp): смужка — це заголовок
// вікна чату, і на двох системах він має бути однаковим, а не «схожим».
const float kBar = 42.0f;
const float kPad = 4.0f;
const float kBtnH = 30.0f;
const float kLogo = 18.0f;
const float kGrip = 14.0f;

const BLRgba32 kBarBg(0x66101014u);
const BLRgba32 kBtnBg(0x33FFFFFFu);
const BLRgba32 kBtnHot(0x55FFFFFFu);
const BLRgba32 kInk(0xFFE8E6EAu);
const BLRgba32 kTrack(0x33000000u);
const BLRgba32 kFill(0xFF9146FFu);

}  // namespace

// Наступний символ рядка як код, а не байт.
//
// Досі всі підписи рамки були латиницею, і байт дорівнював символу. Щойно тут
// з'явилося перше українське слово, це перестало бути правдою: «Hominka
// працює» намалювалося б набором випадкових гліфів. Розбір навмисне
// найпростіший — нам вистачає того, що є в UTF-8.
unsigned next_cp(const char** p) {
    const unsigned char* u = (const unsigned char*)*p;
    unsigned c = *u++;
    int extra = 0;
    if (c >= 0xF0) { c &= 0x07; extra = 3; }
    else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
    else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
    while (extra-- > 0 && (*u & 0xC0) == 0x80) c = (c << 6) | (*u++ & 0x3F);
    *p = (const char*)u;
    return c;
}

ChromeBL::Layout ChromeBL::layout(int w, int h) const {
    Layout L;
    L.strip = {2.0f, 2.0f, (float)w - 4.0f, kBar};

    // Значок відступає від краю на ті самі 14 пікселів, що й у Windows-смужці
    // та в заголовку панелі налаштувань: приклеєний до рамки він виглядав
    // кривим поруч із хрестиком, який стоїть усередині своєї кнопки.
    float x = 14.0f;
    const float y = 2.0f + (kBar - kBtnH) * 0.5f;
    // Значок і назва — першими: вікно чату не має ані заголовка, ані рядка в
    // панелі задач, тож інакше воно ніде себе не називає.
    L.logo = {x, 2.0f + (kBar - kLogo) * 0.5f, kLogo, kLogo};
    x += kLogo + 6.0f;
    // Ширину назви міряємо тим самим шрифтом, яким її малюємо: зашите число
    // трималося б лише на тому, що шрифт випадково саме такої ширини.
    float name_w = 62.0f;
    if (fonts_) {
        if (Face* face = fonts_->face("sans-serif", 600, false, 16.0f)) {
            name_w = 0.0f;
            for (const char* c = "Hominka"; *c;)
                if (const Glyph* g = face->glyph(next_cp(&c))) name_w += g->advance;
        }
    }
    L.name = {x, y, name_w, kBtnH};           x += name_w + 10.0f;
    L.lock = {x, y, 30.0f, kBtnH};            x += 30.0f + kPad;
    L.zoom_out = {x, y, 28.0f, kBtnH};        x += 28.0f + kPad;
    L.zoom_in = {x, y, 28.0f, kBtnH};         x += 28.0f + kPad;

    // Хрестик скраю, шестерня поруч, повзунок займає те, що лишилося.
    const float gear_w = 30.0f;
    const float right = 2.0f + (float)w - 4.0f - kPad;
    L.close = {right - gear_w, y, gear_w, kBtnH};
    L.gear = {L.close.x - 2.0f - gear_w, y, gear_w, kBtnH};
    const float slider_w = L.gear.x - kPad - x;
    L.opacity = {x, y, slider_w > 40.0f ? slider_w : 0.0f, kBtnH};

    L.grip = {(float)w - kGrip - 2.0f, (float)h - kGrip - 2.0f, kGrip, kGrip};
    return L;
}

void ChromeBL::on_motion(int x, int y) {
    mx_ = x;
    my_ = y;
    hovered_ = true;
}

void ChromeBL::on_button(int x, int y, bool down) {
    mx_ = x;
    my_ = y;
    down_ = down;
    if (!down) {
        drag_opacity_ = false;
    }
}

void ChromeBL::on_leave() {
    hovered_ = false;
    mx_ = my_ = -1;
    down_ = false;
    drag_opacity_ = false;
    have_pressed_ = false;
}

bool ChromeBL::wants_mouse() const {
    return drag_opacity_ || have_pressed_;
}

ChromeBL::Hit ChromeBL::hit(int x, int y, int w, int h) const {
    const Layout L = layout(w, h);
    const float fx = (float)x, fy = (float)y;
    if (L.grip.has(fx, fy)) return Hit::Grip;
    if (L.lock.has(fx, fy) || L.zoom_out.has(fx, fy) || L.zoom_in.has(fx, fy) ||
        L.gear.has(fx, fy) || (L.opacity.w > 0 && L.opacity.has(fx, fy)))
        return Hit::Control;
    if (L.strip.has(fx, fy)) return Hit::Strip;
    return Hit::None;
}

void ChromeBL::text(BLContext* ctx, const char* s, float x, float baseline,
                    const BLRgba32& color, float px) const {
    if (!fonts_ || !s) return;
    Face* face = fonts_->face("sans-serif", 600, false, px);
    if (!face) return;
    float pen = x;
    for (const char* p = s; *p;) {
        const Glyph* g = face->glyph(next_cp(&p));
        if (!g) continue;
        if (g->width > 0 && g->height > 0 && !g->color) {
            BLImage mask;
            if (mask.create_from_data(g->width, g->height, BL_FORMAT_A8,
                                      (void*)g->bits.data(), (intptr_t)g->width,
                                      BL_DATA_ACCESS_READ) == BL_SUCCESS)
                ctx->fill_mask(BLPointI((int)lrintf(pen) + g->left,
                                        (int)lrintf(baseline) - g->top),
                               mask, color);
        }
        pen += g->advance;
    }
}

void ChromeBL::button(BLContext* ctx, const Rect& r, const char* label,
                      bool active) const {
    // Підкладка — ЛИШЕ під курсором. Постійні рамки навколо кожної кнопки
    // роблять зі смужки панель приладів; у віконної рамки їх теж немає.
    const bool hot = r.has((float)mx_, (float)my_);
    if (active || hot)
        ctx->fill_round_rect(BLRect(r.x, r.y, r.w, r.h), 5.0, 5.0,
                             active ? kFill : kBtnHot);
    // Напис по центру: ширину рахуємо тим самим шрифтом, яким малюємо.
    float tw = 0;
    if (fonts_) {
        Face* face = fonts_->face("sans-serif", 600, false, 13.0f);
        if (face)
            for (const char* p = label; *p;) {
                const Glyph* g = face->glyph(next_cp(&p));
                if (g) tw += g->advance;
            }
    }
    text(ctx, label, r.x + (r.w - tw) / 2.0f, r.y + r.h * 0.68f, kInk, 13.0f);
}

float ChromeBL::bar_height() { return kBar; }

// Колодка й шестерня — фігурами, а не підписами «[o]» і «*».
//
// Ті два рядки були заглушками з часів, коли рамку тільки починали малювати. На
// смужці, де решта — значок і назва, вони читалися як недоробка, бо нею й були.
void ChromeBL::lock_icon(BLContext* ctx, const Rect& r, bool locked) const {
    const double cx = r.x + r.w * 0.5, cy = r.y + r.h * 0.5 + 2.0;
    const uint32_t c = locked ? 0xFF22C55Eu : 0xFFBEBEC8u;
    ctx->fill_round_rect(BLRect(cx - 5.0, cy - 1.5, 10.0, 6.5), 1.8, 1.8, BLRgba32(c));
    // Дужка: замкнена стоїть над корпусом рівно, розімкнена зсунута вбік і не
    // доходить до нього — тобто відкрита саме з одного боку.
    BLPath arc;
    arc.arc_to(BLPoint(locked ? cx : cx + 2.6, cy - 1.5), BLPoint(3.4, 3.4),
               3.14159265, locked ? 3.14159265 : 2.35, true);
    ctx->set_stroke_width(1.8);
    ctx->stroke_path(arc, BLRgba32(c));
}

// Хрестик — дві риски, а не символ: гліфа «×» у шрифті може й не бути, а дві
// риски однакові скрізь. На Linux він ще й єдиний спосіб закрити програму:
// вікно без рамки, тож ані заголовка, ані системної кнопки в нього немає.
void ChromeBL::close_icon(BLContext* ctx, const Rect& r) const {
    const double cx = r.x + r.w * 0.5, cy = r.y + r.h * 0.5, d = 4.5;
    ctx->set_stroke_width(1.7);
    const BLRgba32 c(0xFFBEBEC8u);
    ctx->stroke_line(BLPoint(cx - d, cy - d), BLPoint(cx + d, cy + d), c);
    ctx->stroke_line(BLPoint(cx + d, cy - d), BLPoint(cx - d, cy + d), c);
}

void ChromeBL::gear_icon(BLContext* ctx, const Rect& r) const {
    const double cx = r.x + r.w * 0.5, cy = r.y + r.h * 0.5;
    const BLRgba32 c(0xFFBEBEC8u);
    ctx->set_stroke_width(2.0);
    for (int i = 0; i < 6; ++i) {
        const double a = i * 3.14159265 / 3.0;
        ctx->stroke_line(BLPoint(cx + cos(a) * 3.0, cy + sin(a) * 3.0),
                         BLPoint(cx + cos(a) * 6.5, cy + sin(a) * 6.5), c);
    }
    ctx->stroke_circle(cx, cy, 4.0, c);
}

// Значок програми — той самий, що у віконної рамки: скруглений квадрат із
// білою бульбашкою і трьома крапками. Малюємо, а не вантажимо картинку.
void ChromeBL::logo(BLContext* ctx, const Rect& r) const {
    ctx->fill_round_rect(BLRect(r.x, r.y, r.w, r.h), r.w * 0.28, r.w * 0.28,
                         BLRgba32(0xFFB45AF0u));
    const double bx = r.x + r.w * 0.17, by = r.y + r.h * 0.25;
    const double bw = r.w * 0.66, bh = r.h * 0.42;
    ctx->fill_round_rect(BLRect(bx, by, bw, bh), bh * 0.42, bh * 0.42,
                         BLRgba32(0xF5FFFFFFu));
    const uint32_t dots[3] = {0xFF6366F1u, 0xFF9333EAu, 0xFFEC4899u};
    for (int i = 0; i < 3; ++i)
        ctx->fill_circle(bx + bw * (0.26 + 0.24 * i), by + bh * 0.5,
                         r.w * 0.055 + 0.4, BLRgba32(dots[i]));
}

void ChromeBL::draw_backdrop(BLContext* ctx, int w, int h, const Look& look) const {
    // Тло й рамка — ДВІ різні речі, і вимикаються окремо. Те саме рішення, що
    // й у віконної рамки: тло веде свій повзунок і лишається завжди, зокрема
    // поверх гри; рамку веде свій перемикач.
    // Замкнене вікно може лишатися зовсім чистим — див. Look::lock_bg.
    if (look.locked && !look.lock_bg) return;
    const uint32_t a = (uint32_t)(look.bg_alpha * 255.0f + 0.5f);
    if (a) ctx->fill_round_rect(BLRect(0, 0, (double)w, (double)h), 10.0, 10.0,
                                BLRgba32((a << 24) | 0x101014u));
    if (look.frameless) return;
    ctx->set_stroke_width(2.0);
    ctx->stroke_round_rect(BLRect(1, 1, (double)w - 2, (double)h - 2), 10.0, 10.0,
                           BLRgba32(look.locked ? 0xFF22C55Eu : 0xFFA855F7u));
}

ChromeEvents ChromeBL::draw_controls(BLContext* ctx, int w, int h, Look* look,
                                     bool header, bool empty) {
    ChromeEvents ev;
    if (!ctx || !look) return ev;

    // Порожня стрічка — не привід показувати порожнє вікно: воно, що зникає
    // разом із курсором, виглядає як несправність, а не як задум.
    if (empty && !look->locked && fonts_) {
        const char* lines[3] = {"Hominka працює",
                                "Тут з'являтимуться повідомлення чату.",
                                "Канали — у налаштуваннях, кнопка згори праворуч."};
        const uint32_t cols[3] = {0xFFE4E4E7u, 0xFF8B8B93u, 0xFF8B8B93u};
        float y = (float)h * 0.5f - 30.0f;
        for (int i = 0; i < 3; ++i) {
            float tw = 0.0f;
            if (Face* face = fonts_->face("sans-serif", 600, false, 14.0f))
                for (const char* c = lines[i]; *c;)
                    if (const Glyph* g = face->glyph(next_cp(&c))) tw += g->advance;
            text(ctx, lines[i], ((float)w - tw) * 0.5f, y, BLRgba32(cols[i]), 14.0f);
            y += i == 0 ? 26.0f : 20.0f;
        }
    }

    // Смужку видно завжди, коли її ввімкнено в налаштуваннях; вимкнену — лише
    // під курсором. Замкнене вікно керування не показує взагалі: миша крізь
    // нього проходить, і кнопки все одно не натиснути.
    if ((look->locked && !look->lock_header) || (!header && !hovered_)) {
        have_pressed_ = false;
        return ev;
    }

    const Layout L = layout(w, h);
    ctx->fill_round_rect(BLRect(L.strip.x, L.strip.y, L.strip.w, L.strip.h),
                         5.0, 5.0, kBarBg);

    logo(ctx, L.logo);
    text(ctx, "Hominka", L.name.x, L.name.y + L.name.h * 0.66f,
         BLRgba32(0xFFE4E4E7u), 16.0f);
    button(ctx, L.lock, "", look->locked);
    lock_icon(ctx, L.lock, look->locked);
    button(ctx, L.zoom_out, "A-", false);
    button(ctx, L.zoom_in, "A+", false);
    button(ctx, L.gear, "", false);
    gear_icon(ctx, L.gear);
    button(ctx, L.close, "", false);
    close_icon(ctx, L.close);

    if (L.opacity.w > 0) {
        const float t = (look->opacity - 0.2f) / 0.8f;
        const float frac = t < 0 ? 0 : (t > 1 ? 1 : t);
        ctx->fill_round_rect(BLRect(L.opacity.x, L.opacity.y + L.opacity.h / 2 - 3,
                                    L.opacity.w, 6.0), 3.0, 3.0, kTrack);
        ctx->fill_round_rect(BLRect(L.opacity.x, L.opacity.y + L.opacity.h / 2 - 3,
                                    L.opacity.w * frac, 6.0), 3.0, 3.0, kFill);
    }

    // --- натискання --------------------------------------------------------
    //
    // Кнопка спрацьовує на ВІДПУСКАННЯ над собою: з'їхав курсор — передумав.
    // Так поводяться кнопки скрізь, і саме цього людина чекає.
    const float fx = (float)mx_, fy = (float)my_;

    if (down_ && L.opacity.w > 0 && (drag_opacity_ || L.opacity.has(fx, fy))) {
        drag_opacity_ = true;
        float t = (fx - L.opacity.x) / L.opacity.w;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        const float want = 0.2f + t * 0.8f;
        if (std::fabs(want - look->opacity) > 0.004f) {
            look->opacity = want;
            ev.look_changed = true;
        }
        return ev;                        // тягнемо повзунок — кнопки не чіпаємо
    }

    if (down_ && !have_pressed_) {
        for (const Rect* r : {&L.lock, &L.zoom_out, &L.zoom_in, &L.gear, &L.close}) {
            if (r->has(fx, fy)) { pressed_ = *r; have_pressed_ = true; break; }
        }
        return ev;
    }
    if (!down_ && have_pressed_) {
        have_pressed_ = false;
        if (!pressed_.has(fx, fy)) return ev;      // відпустили повз — передумали
        if (pressed_.x == L.lock.x) {
            look->locked = !look->locked;
            ev.lock_changed = true;
        } else if (pressed_.x == L.zoom_out.x) {
            look->zoom = zoom_clamp(look->zoom - ZOOM_STEP);
            ev.look_changed = true;
        } else if (pressed_.x == L.zoom_in.x) {
            look->zoom = zoom_clamp(look->zoom + ZOOM_STEP);
            ev.look_changed = true;
        } else if (pressed_.x == L.gear.x) {
            ev.open_settings = true;
        } else if (pressed_.x == L.close.x) {
            ev.close = true;
        }
    }
    return ev;
}

}  // namespace hominka
