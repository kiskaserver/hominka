#include "gfx/container_bl.h"

#include <cmath>
#include <cstring>

namespace hominka {

namespace {

inline BLRgba32 to_bl(const Color& c) {
    auto q = [](float v) -> uint32_t {
        const float x = v < 0 ? 0 : (v > 1 ? 1 : v);
        return (uint32_t)(x * 255.0f + 0.5f);
    };
    // Blend2D бере НЕпомножені на альфу складові й множить сам.
    return BLRgba32(q(c.r), q(c.g), q(c.b), q(c.a));
}

inline BLRgba32 to_bl(const litehtml::web_color& c) {
    return BLRgba32(c.red, c.green, c.blue, c.alpha);
}

inline BLRect to_rect(const litehtml::position& p) {
    return BLRect(p.x, p.y, p.width, p.height);
}

}  // namespace

ContainerBL::ContainerBL(FontStore* fonts, ImageCache* images)
    : fonts_(fonts), images_(images) {}

ContainerBL::~ContainerBL() {
    for (Font* f : fonts_owned_) delete f;
    for (Layer* l : layers_) delete l;
}

void ContainerBL::begin(BLContext* ctx, int width, int height) {
    ctx_ = ctx;
    width_ = width;
    height_ = height;
    clip_depth_ = 0;
}

void ContainerBL::end() {
    // Недомальований документ міг лишити відкрите відсікання чи шар — догортаємо,
    // щоб наступний малюнок починався з чистого стану.
    while (clip_depth_ > 0) del_clip();
    while (!layers_.empty()) pop_opacity();
    while (!transforms_.empty()) pop_transform();
    ctx_ = nullptr;
}

std::vector<std::string> ContainerBL::take_missing() {
    std::vector<std::string> out;
    out.swap(missing_);
    return out;
}

std::vector<Sprite> ContainerBL::take_sprites() {
    std::vector<Sprite> out;
    out.swap(sprites_);
    return out;
}

// --- шрифти ----------------------------------------------------------------

litehtml::uint_ptr ContainerBL::create_font(const litehtml::font_description& descr,
                                            const litehtml::document* doc,
                                            litehtml::font_metrics* fm) {
    dtrace("create_font: family=[%s] size=%.2f weight=%d", descr.family.c_str(),
           (double)descr.size, descr.weight);
    Font* f = new Font();
    f->shadow = shadow_;
    TextShadow parsed;
    if (shadow_from_channel(descr.emphasis_style, &parsed)) f->shadow = parsed;
    f->size = descr.size > 0 ? (float)descr.size : base_font_px_;
    f->decoration = descr.decoration_line;
    f->weight = descr.weight > 0 ? descr.weight : 400;
    f->italic = descr.style == litehtml::font_style_italic;

    // Список родин передаємо цілим: на відміну від Windows, підбір робимо ми
    // самі, і перебрати весь список — саме те, що потрібно. Узагальнені імена
    // fontconfig розуміє й сам («sans-serif»), а «Segoe UI» на Linux немає —
    // він підставить щось розумне.
    f->families = descr.family.empty() ? "sans-serif" : descr.family;
    f->face = fonts_ ? fonts_->face(f->families, f->weight, f->italic, f->size) : nullptr;

    if (fm) {
        fm->font_size = f->size;
        if (f->face) {
            fm->ascent = f->face->ascent();
            fm->descent = f->face->descent();
            fm->x_height = f->face->x_height();
            fm->ch_width = f->face->ch_width();
        } else {
            // Шрифту немає зовсім — беремо правдоподібні числа, щоб розкладка
            // не розсипалася на нуль.
            fm->ascent = f->size * 0.8f;
            fm->descent = f->size * 0.2f;
            fm->x_height = f->size * 0.5f;
            fm->ch_width = f->size * 0.5f;
        }
        // Висота — рівно ascent+descent, без line_gap: рівно з тієї самої
        // причини, що й у віконному контейнері (базова лінія інакше поїде).
        fm->height = fm->ascent + fm->descent;
        fm->draw_spaces = f->decoration != 0;
        fm->sub_shift = fm->descent;
        fm->super_shift = fm->ascent / 3.0f;
    }

    dtrace("  готово: face=%p", (void*)f->face);
    fonts_owned_.push_back(f);
    return (litehtml::uint_ptr)f;
}

void ContainerBL::delete_font(litehtml::uint_ptr hFont) {
    Font* f = (Font*)hFont;
    if (!f) return;
    for (size_t i = 0; i < fonts_owned_.size(); ++i) {
        if (fonts_owned_[i] == f) {
            fonts_owned_.erase(fonts_owned_.begin() + i);
            break;
        }
    }
    delete f;
}

Face* ContainerBL::face_for(const Font* f, unsigned cp) {
    if (f->face && f->face->has(cp)) return f->face;
    if (!fonts_) return f->face;
    Face* fb = fonts_->fallback(cp, f->weight, f->italic, f->size);
    return fb ? fb : f->face;
}

litehtml::pixel_t ContainerBL::text_width(const char* text, litehtml::uint_ptr hFont) {
    const Font* f = (const Font*)hFont;
    if (!f || !text) return 0;
    const std::string s(text);
    float w = 0;
    for (size_t i = 0; i < s.size(); ) {
        int len = 0;
        const unsigned cp = utf8_cp(s, i, &len);
        i += len;
        Face* face = face_for(f, cp);
        if (!face) continue;
        const Glyph* g = face->glyph(cp);
        if (g) w += g->advance;
    }
    return (litehtml::pixel_t)w;
}

void ContainerBL::draw_run(const std::string& text, const Font* f, const Color& c,
                           float x, float baseline) {
    if (!ctx_ || !f) return;
    const BLRgba32 col = to_bl(c);
    float pen = x;
    for (size_t i = 0; i < text.size(); ) {
        int len = 0;
        const unsigned cp = utf8_cp(text, i, &len);
        i += len;
        Face* face = face_for(f, cp);
        if (!face) continue;
        const Glyph* g = face->glyph(cp);
        if (!g) continue;
        if (g->width > 0 && g->height > 0) {
            const int gx = (int)lrintf(pen) + g->left;
            const int gy = (int)lrintf(baseline) - g->top;
            BLImage img;
            // create_from_data не копіює: байти гліфа лежать у кеші накреслення
            // й переживуть цей виклик.
            if (g->color) {
                // Кольорова емодзі: власний колір, наш ігнорується — так само
                // поводиться й DirectWrite.
                if (img.create_from_data(g->width, g->height, BL_FORMAT_PRGB32,
                                         (void*)g->bits.data(), (intptr_t)g->width * 4,
                                         BL_DATA_ACCESS_READ) == BL_SUCCESS)
                    ctx_->blit_image(BLPointI(gx, gy), img);
            } else {
                // Звичайний гліф: растр — це маска, крізь яку кладемо колір.
                if (img.create_from_data(g->width, g->height, BL_FORMAT_A8,
                                         (void*)g->bits.data(), (intptr_t)g->width,
                                         BL_DATA_ACCESS_READ) == BL_SUCCESS)
                    ctx_->fill_mask(BLPointI(gx, gy), img, col);
            }
        }
        pen += g->advance;
    }

    // Підкреслення й закреслення litehtml лишає контейнеру.
    if (f->decoration && f->face) {
        const float thick = f->size / 14.0f > 1.0f ? f->size / 14.0f : 1.0f;
        const float w = pen - x;
        if (f->decoration & litehtml::text_decoration_line_underline)
            ctx_->fill_rect(BLRect(x, baseline + f->face->descent() * 0.4f, w, thick), col);
        if (f->decoration & litehtml::text_decoration_line_line_through)
            ctx_->fill_rect(BLRect(x, baseline - f->face->x_height() * 0.5f, w, thick), col);
    }
}

void ContainerBL::draw_text(litehtml::uint_ptr hdc, const char* text,
                            litehtml::uint_ptr hFont, litehtml::web_color color,
                            const litehtml::position& pos) {
    const Font* f = (const Font*)hFont;
    if (!ctx_ || !f || !text || !*text) return;

    // Базову лінію кладемо від НИЗУ прямокутника — точно як у віконному
    // контейнері, і з тієї самої причини: інакше при різних кеглях у рядку
    // кожен з'їжджав би на свою частку.
    const float baseline = pos.y + pos.height - (f->face ? f->face->descent()
                                                         : f->size * 0.2f);
    const std::string s(text);

    const TextShadow& sh = f->shadow;
    if (sh.on && sh.color.a > 0.0f) {
        // Розмиття — кількома зсувами по колу замість справжнього гауса, так
        // само як під Windows: тінь тут заради читабельності поверх гри.
        const float r = sh.blur * 0.5f;
        Color sc = sh.color;
        if (r < 0.5f) {
            draw_run(s, f, sc, pos.x + sh.dx, baseline + sh.dy);
        } else {
            const float taps[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                      {0.7f, 0.7f}, {-0.7f, 0.7f},
                                      {0.7f, -0.7f}, {-0.7f, -0.7f}};
            sc.a = sh.color.a * 0.45f;
            for (const auto& t : taps)
                draw_run(s, f, sc, pos.x + sh.dx + t[0] * r, baseline + sh.dy + t[1] * r);
            sc.a = sh.color.a * 0.8f;
            draw_run(s, f, sc, pos.x + sh.dx, baseline + sh.dy);
        }
    }
    draw_run(s, f, to_color(color), pos.x, baseline);
}

litehtml::pixel_t ContainerBL::pt_to_px(float pt) const { return pt * 96.0f / 72.0f; }
litehtml::pixel_t ContainerBL::get_default_font_size() const { return base_font_px_; }
const char* ContainerBL::get_default_font_name() const { return "sans-serif"; }

void ContainerBL::draw_list_marker(litehtml::uint_ptr hdc,
                                   const litehtml::list_marker& marker) {
    if (!ctx_) return;
    const float r = marker.pos.height / 3.0f;
    ctx_->fill_circle(marker.pos.x + marker.pos.width / 2.0,
                      marker.pos.y + marker.pos.height / 2.0, r,
                      to_bl(marker.color));
}

// --- картинки --------------------------------------------------------------

void ContainerBL::load_image(const char* src, const char* baseurl, bool redraw_on_ready) {
    if (!src || !*src) return;
    const std::string url(src);
    if (images_->get(url)) return;
    if (images_->known(url)) return;
    for (const auto& m : missing_) if (m == url) return;
    missing_.push_back(url);
}

void ContainerBL::get_image_size(const char* src, const char* baseurl, litehtml::size& sz) {
    sz.width = 0;
    sz.height = 0;
    if (!src || !*src) return;
    const Image* img = images_->get(src);
    if (!img) return;
    sz.width = (litehtml::pixel_t)img->nat_width;
    sz.height = (litehtml::pixel_t)img->nat_height;
}

void ContainerBL::blit(const litehtml::background_layer& layer, const Image* img) {
    if (!ctx_ || !img || !img->ok()) return;
    const ImageFrame& fr = img->frames[0];

    BLImage src;
    if (src.create_from_data(fr.width, fr.height, BL_FORMAT_PRGB32,
                             (void*)fr.bgra.data(), (intptr_t)fr.width * 4,
                             BL_DATA_ACCESS_READ) != BL_SUCCESS)
        return;

    ctx_->save();
    ctx_->clip_to_rect(to_rect(layer.clip_box));

    const float w = layer.origin_box.width, h = layer.origin_box.height;
    if (w > 0 && h > 0) {
        const bool rx = layer.repeat == litehtml::background_repeat_repeat ||
                        layer.repeat == litehtml::background_repeat_repeat_x;
        const bool ry = layer.repeat == litehtml::background_repeat_repeat ||
                        layer.repeat == litehtml::background_repeat_repeat_y;
        const BLRectI whole(0, 0, fr.width, fr.height);
        if (!rx && !ry) {
            ctx_->blit_image(to_rect(layer.origin_box), src, whole);
        } else {
            const BLRect clip = to_rect(layer.clip_box);
            const float x0 = rx ? (float)(clip.x - std::fmod(clip.x - layer.origin_box.x, w) - w)
                                : (float)layer.origin_box.x;
            const float y0 = ry ? (float)(clip.y - std::fmod(clip.y - layer.origin_box.y, h) - h)
                                : (float)layer.origin_box.y;
            for (float y = y0; y < (ry ? clip.y + clip.h : layer.origin_box.y + 0.5f); y += h) {
                for (float x = x0; x < (rx ? clip.x + clip.w : layer.origin_box.x + 0.5f); x += w) {
                    ctx_->blit_image(BLRect(x, y, w, h), src, whole);
                    if (!rx) break;
                }
                if (!ry) break;
            }
        }
    }
    ctx_->restore();
}

void ContainerBL::draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                             const std::string& url, const std::string& base_url) {
    if (!ctx_) return;
    const Image* img = images_->get(url);
    if (!img) { load_image(url.c_str(), base_url.c_str(), true); return; }

    // Анімований емоут у картинку рядка не запікаємо — див. однойменне місце
    // в container_d2d.cpp: рядок малюється раз, а емоут має жити далі.
    if (record_sprites_ && img->animated()) {
        Sprite s;
        s.url = url;
        s.x = (float)layer.origin_box.x;
        s.y = (float)layer.origin_box.y;
        s.w = (float)layer.origin_box.width;
        s.h = (float)layer.origin_box.height;
        if (s.w > 0 && s.h > 0) sprites_.push_back(std::move(s));
        return;
    }
    blit(layer, img);
}

// --- заливки ---------------------------------------------------------------

void ContainerBL::fill_box(const BLRect& r, float radius, const Color& c) {
    if (!ctx_ || c.a <= 0.0f) return;
    if (radius > 0.5f) ctx_->fill_round_rect(r, radius, radius, to_bl(c));
    else ctx_->fill_rect(r, to_bl(c));
}

void ContainerBL::draw_solid_fill(litehtml::uint_ptr hdc,
                                  const litehtml::background_layer& layer,
                                  const litehtml::web_color& color) {
    if (!ctx_ || color.alpha == 0) return;
    const float r = max_radius(layer.border_radius);
    if (r > 0.5f && !same_radius(layer.border_radius)) {
        // Кути різні — заливаємо по відсіканню найбільшим радіусом: краще
        // трохи не той кут, ніж прямокутник поверх скругленої рамки.
        ctx_->save();
        ctx_->clip_to_rect(to_rect(layer.clip_box));
        fill_box(to_rect(layer.border_box), r, to_color(color));
        ctx_->restore();
        return;
    }
    fill_box(to_rect(layer.border_box), r, to_color(color));
}

void ContainerBL::draw_linear_gradient(litehtml::uint_ptr hdc,
                                       const litehtml::background_layer& layer,
                                       const litehtml::background_layer::linear_gradient& g) {
    if (!ctx_) return;
    BLGradient grad(BLLinearGradientValues(g.start.x, g.start.y, g.end.x, g.end.y));
    for (const auto& cp : g.color_points) grad.add_stop(cp.offset, to_bl(cp.color));
    ctx_->save();
    ctx_->clip_to_rect(to_rect(layer.clip_box));
    ctx_->fill_rect(to_rect(layer.border_box), grad);
    ctx_->restore();
}

void ContainerBL::draw_radial_gradient(litehtml::uint_ptr hdc,
                                       const litehtml::background_layer& layer,
                                       const litehtml::background_layer::radial_gradient& g) {
    if (!ctx_) return;
    const double r = g.radius.x > g.radius.y ? g.radius.x : g.radius.y;
    BLGradient grad(BLRadialGradientValues(g.position.x, g.position.y,
                                           g.position.x, g.position.y, r));
    for (const auto& cp : g.color_points) grad.add_stop(cp.offset, to_bl(cp.color));
    ctx_->save();
    ctx_->clip_to_rect(to_rect(layer.clip_box));
    ctx_->fill_rect(to_rect(layer.border_box), grad);
    ctx_->restore();
}

void ContainerBL::draw_conic_gradient(litehtml::uint_ptr hdc,
                                      const litehtml::background_layer& layer,
                                      const litehtml::background_layer::conic_gradient& g) {
    if (!ctx_) return;
    // litehtml дає кут у градусах, Blend2D чекає радіани.
    BLGradient grad(BLConicGradientValues(g.position.x, g.position.y,
                                          g.angle * 3.14159265358979 / 180.0));
    for (const auto& cp : g.color_points) grad.add_stop(cp.offset, to_bl(cp.color));
    ctx_->save();
    ctx_->clip_to_rect(to_rect(layer.clip_box));
    ctx_->fill_rect(to_rect(layer.border_box), grad);
    ctx_->restore();
}

void ContainerBL::draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders,
                               const litehtml::position& draw_pos, bool root) {
    if (!ctx_) return;
    const float l = borders.left.width, r = borders.right.width;
    const float t = borders.top.width, b = borders.bottom.width;
    const float rad = max_radius(borders.radius);

    // Однакова рамка з усіх боків і скруглення — малюємо як рамку скругленого
    // прямокутника; це найчастіший випадок у темах чату.
    if (rad > 0.5f && l > 0 && std::fabs(l - r) < 0.5f && std::fabs(l - t) < 0.5f &&
        std::fabs(l - b) < 0.5f) {
        ctx_->set_stroke_width(l);
        ctx_->stroke_round_rect(BLRect(draw_pos.x + l / 2, draw_pos.y + l / 2,
                                       draw_pos.width - l, draw_pos.height - l),
                                rad, rad, to_bl(borders.left.color));
        return;
    }

    // Інакше — по смузі на бік. Так само робить і віконний контейнер.
    if (l > 0 && borders.left.color.alpha)
        ctx_->fill_rect(BLRect(draw_pos.x, draw_pos.y, l, draw_pos.height),
                        to_bl(borders.left.color));
    if (r > 0 && borders.right.color.alpha)
        ctx_->fill_rect(BLRect(draw_pos.right() - r, draw_pos.y, r, draw_pos.height),
                        to_bl(borders.right.color));
    if (t > 0 && borders.top.color.alpha)
        ctx_->fill_rect(BLRect(draw_pos.x, draw_pos.y, draw_pos.width, t),
                        to_bl(borders.top.color));
    if (b > 0 && borders.bottom.color.alpha)
        ctx_->fill_rect(BLRect(draw_pos.x, draw_pos.bottom() - b, draw_pos.width, b),
                        to_bl(borders.bottom.color));
}

// --- відсікання, прозорість, перетворення ----------------------------------

void ContainerBL::set_clip(const litehtml::position& pos,
                           const litehtml::border_radiuses& bdr_radius) {
    if (!ctx_) return;
    // Blend2D вміє відсікати лише прямокутником — як і PushAxisAlignedClip у
    // Direct2D, тож поведінка збігається.
    ctx_->save();
    ctx_->clip_to_rect(to_rect(pos));
    ++clip_depth_;
}

void ContainerBL::del_clip() {
    if (!ctx_ || clip_depth_ <= 0) return;
    ctx_->restore();
    --clip_depth_;
}

void ContainerBL::push_opacity(float opacity) {
    if (!ctx_) return;
    if (opacity >= 1.0f) return;

    Layer* l = new Layer();
    l->alpha = opacity;
    l->prev = ctx_;
    if (l->image.create(width_, height_, BL_FORMAT_PRGB32) != BL_SUCCESS ||
        l->ctx.begin(l->image) != BL_SUCCESS) {
        delete l;
        return;
    }
    // Полотно шару порожнє; перетворення переносимо, щоб координати збіглися.
    l->ctx.set_transform(ctx_->user_transform());
    layers_.push_back(l);
    ctx_ = &l->ctx;
}

void ContainerBL::pop_opacity() {
    if (layers_.empty()) return;
    Layer* l = layers_.back();
    layers_.pop_back();
    l->ctx.end();
    ctx_ = l->prev;
    if (ctx_) {
        // Кладемо ГРУПУ цілком і вже з альфою — саме в цьому й сенс шару:
        // там, де діти перекриваються, не стає темніше.
        ctx_->save();
        ctx_->reset_transform();          // полотно шару вже в координатах цілі
        ctx_->set_global_alpha(l->alpha);
        ctx_->blit_image(BLPointI(0, 0), l->image);
        ctx_->restore();
    }
    delete l;
}

bool ContainerBL::push_transform(const litehtml::position& border_box,
                                 const std::string& value,
                                 litehtml::pixel_t font_size) {
    if (!ctx_ || value.empty()) return false;
    float m[6];
    if (!parse_transform(value, font_size, border_box, m)) return false;

    transforms_.push_back(ctx_->user_transform());
    // Порядок полів у BLMatrix2D той самий, що й у нашому масиві.
    ctx_->apply_transform(BLMatrix2D(m[0], m[1], m[2], m[3], m[4], m[5]));
    return true;
}

void ContainerBL::pop_transform() {
    if (!ctx_ || transforms_.empty()) return;
    ctx_->set_transform(transforms_.back());
    transforms_.pop_back();
}

void ContainerBL::draw_box_shadow(litehtml::uint_ptr hdc,
                                  const litehtml::position& border_box,
                                  const litehtml::border_radiuses& radius,
                                  const std::string& value,
                                  litehtml::pixel_t font_size) {
    if (!ctx_ || value.empty()) return;

    // Кілька тіней через кому — малюємо від останньої до першої: у CSS перша
    // лежить НАД рештою.
    const std::vector<std::string> parts = split_shadow_list(value);
    for (size_t k = parts.size(); k-- > 0; ) {
        BoxShadow sh;
        if (!parse_box_shadow(parts[k], font_size, &sh)) continue;
        if (sh.inset) continue;          // внутрішні тіні не малюємо (див. README)

        const float r = max_radius(radius) + sh.spread;
        // Розмиття — кількома розширеними контурами з малою альфою; те саме
        // наближення, що й під Windows, щоб вигляд збігався.
        const int steps = sh.blur > 0.5f ? 5 : 1;
        for (int i = steps; i >= 1; --i) {
            const float grow = sh.spread + sh.blur * ((float)i / (float)steps) * 0.5f;
            Color c = sh.color;
            c.a = sh.color.a * (steps == 1 ? 1.0f : 0.9f / (float)steps);
            const BLRect rc(border_box.x + sh.dx - grow, border_box.y + sh.dy - grow,
                            border_box.width + grow * 2, border_box.height + grow * 2);
            fill_box(rc, r > 0.5f ? r + grow : 0.0f, c);
        }
    }
}

// --- решта -----------------------------------------------------------------

void ContainerBL::transform_text(litehtml::string& text, litehtml::text_transform tt) {
    // Лише латиниця й кирилиця: саме вони трапляються в чаті, а тягнути сюди
    // повну таблицю Unicode заради text-transform ні до чого.
    if (tt == litehtml::text_transform_none) return;
    std::string out;
    bool word_start = true;
    for (size_t i = 0; i < text.size(); ) {
        int len = 0;
        unsigned cp = utf8_cp(text, i, &len);
        const bool up = tt == litehtml::text_transform_uppercase ||
                        (tt == litehtml::text_transform_capitalize && word_start);
        const bool low = tt == litehtml::text_transform_lowercase;
        if (up) {
            if (cp >= 'a' && cp <= 'z') cp -= 32;
            else if (cp >= 0x430 && cp <= 0x44F) cp -= 32;
            else if (cp >= 0x450 && cp <= 0x45F) cp -= 80;
        } else if (low) {
            if (cp >= 'A' && cp <= 'Z') cp += 32;
            else if (cp >= 0x410 && cp <= 0x42F) cp += 32;
            else if (cp >= 0x400 && cp <= 0x40F) cp += 80;
        }
        word_start = cp == ' ' || cp == '\t' || cp == '\n';
        // Назад у UTF-8.
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        i += len;
    }
    text = out;
}

void ContainerBL::import_css(litehtml::string& text, const litehtml::string& url,
                             litehtml::string& baseurl) {
    // Сторінка чату навмисно самодостатня: @import у мережу не ходить.
    text.clear();
}

void ContainerBL::get_viewport(litehtml::position& viewport) const {
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = width_;
    viewport.height = height_;
}

litehtml::element::ptr ContainerBL::create_element(
        const char* tag_name, const litehtml::string_map& attributes,
        const std::shared_ptr<litehtml::document>& doc) {
    return nullptr;
}

void ContainerBL::get_media_features(litehtml::media_features& media) const {
    media.type = litehtml::media_type_screen;
    media.width = width_;
    media.height = height_;
    media.device_width = width_;
    media.device_height = height_;
    media.color = 8;
    media.monochrome = 0;
    media.color_index = 256;
    media.resolution = 96;
}

void ContainerBL::get_language(litehtml::string& language, litehtml::string& culture) const {
    language = "uk";
    culture = "";
}

void ContainerBL::split_text(const char* text,
                             const std::function<void(const char*)>& on_word,
                             const std::function<void(const char*)>& on_space) {
    // Правила переносу спільні для всіх систем — див. cssbits.cpp.
    split_text_words(text, on_word, on_space);
}

}  // namespace hominka
