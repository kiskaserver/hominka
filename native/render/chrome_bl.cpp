#include "chrome_bl.h"

#include <cmath>
#include <cstring>

#include "cssbits.h"

namespace hominka {

namespace {

// Ті самі числа, що й у віконної рамки (chrome.cpp): смужка 22 px, кнопки
// трохи нижчі за неї. Розбіжність тут одразу читалася б як «на Linux усе
// інакше», хоча йдеться про ту саму програму.
const float kBar = 22.0f;
const float kPad = 3.0f;
const float kBtnH = kBar - 6.0f;
const float kGrip = 14.0f;

const BLRgba32 kBarBg(0x66101014u);
const BLRgba32 kBtnBg(0x33FFFFFFu);
const BLRgba32 kBtnHot(0x55FFFFFFu);
const BLRgba32 kInk(0xFFE8E6EAu);
const BLRgba32 kTrack(0x33000000u);
const BLRgba32 kFill(0xFF9146FFu);

}  // namespace

ChromeBL::Layout ChromeBL::layout(int w, int h) const {
    Layout L;
    L.strip = {2.0f, 2.0f, (float)w - 4.0f, kBar};

    float x = 2.0f + kPad;
    const float y = 2.0f + 3.0f;
    L.lock = {x, y, 28.0f, kBtnH};            x += 28.0f + kPad;
    L.zoom_out = {x, y, 26.0f, kBtnH};        x += 26.0f + kPad;
    L.zoom_in = {x, y, 26.0f, kBtnH};         x += 26.0f + kPad;

    // Шестерня притиснута до правого краю, повзунок займає те, що лишилося.
    const float gear_w = 24.0f;
    const float right = 2.0f + (float)w - 4.0f - kPad;
    L.gear = {right - gear_w, y, gear_w, kBtnH};
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
    for (const char* p = s; *p; ++p) {
        const Glyph* g = face->glyph((unsigned char)*p);
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
    const bool hot = r.has((float)mx_, (float)my_);
    ctx->fill_round_rect(BLRect(r.x, r.y, r.w, r.h), 4.0, 4.0,
                         active ? kFill : (hot ? kBtnHot : kBtnBg));
    // Напис по центру: ширину рахуємо тим самим шрифтом, яким малюємо.
    float tw = 0;
    if (fonts_) {
        Face* face = fonts_->face("sans-serif", 600, false, 11.0f);
        if (face)
            for (const char* p = label; *p; ++p) {
                const Glyph* g = face->glyph((unsigned char)*p);
                if (g) tw += g->advance;
            }
    }
    text(ctx, label, r.x + (r.w - tw) / 2.0f, r.y + r.h * 0.72f, kInk, 11.0f);
}

void ChromeBL::draw_backdrop(BLContext* ctx, int w, int h, const Look& look) const {
    if (look.frameless) return;          // «лише повідомлення»
    const uint32_t a = (uint32_t)(look.bg_alpha * 255.0f + 0.5f);
    if (a == 0) return;
    ctx->fill_round_rect(BLRect(0, 0, (double)w, (double)h), 10.0, 10.0,
                         BLRgba32((a << 24) | 0x101014u));
}

ChromeEvents ChromeBL::draw_controls(BLContext* ctx, int w, int h, Look* look) {
    ChromeEvents ev;
    if (!ctx || !look) return ev;

    // Поки миша не над вікном — рамки немає взагалі. Смужка поверх гри, яка
    // висить постійно, це шум, а не зручність.
    if (!hovered_) {
        have_pressed_ = false;
        return ev;
    }

    const Layout L = layout(w, h);
    ctx->fill_round_rect(BLRect(L.strip.x, L.strip.y, L.strip.w, L.strip.h),
                         5.0, 5.0, kBarBg);

    button(ctx, L.lock, look->locked ? "[#]" : "[o]", look->locked);
    button(ctx, L.zoom_out, "A-", false);
    button(ctx, L.zoom_in, "A+", false);
    button(ctx, L.gear, "*", false);

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
        for (const Rect* r : {&L.lock, &L.zoom_out, &L.zoom_in, &L.gear}) {
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
            look->zoom = look->zoom > 0.4f ? look->zoom - 0.1f : 0.3f;
            ev.look_changed = true;
        } else if (pressed_.x == L.zoom_in.x) {
            look->zoom = look->zoom < 3.9f ? look->zoom + 0.1f : 4.0f;
            ev.look_changed = true;
        } else if (pressed_.x == L.gear.x) {
            ev.open_settings = true;
        }
    }
    return ev;
}

}  // namespace hominka
