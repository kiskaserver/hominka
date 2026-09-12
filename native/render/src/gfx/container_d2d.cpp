#include "gfx/container_d2d.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace hominka {

namespace {

inline D2D1_COLOR_F to_d2d(const litehtml::web_color& c) {
    return D2D1::ColorF(c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f, c.alpha / 255.0f);
}

inline D2D1_RECT_F to_rect(const litehtml::position& p) {
    return D2D1::RectF(p.x, p.y, p.x + p.width, p.y + p.height);
}


// Наш колір → колір Direct2D. Розбір лишився спільним (cssbits), а тип
// тут потрібен свій.
inline D2D1_COLOR_F to_d2d(const Color& c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}

std::wstring utf8_to_w(const char* s, int len = -1) {
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, &w[0], n);
    if (len == -1 && !w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

}  // namespace

// ---------------------------------------------------------------------------

ContainerD2D::ContainerD2D(IDWriteFactory* dw, ImageCache* images)
    : dw_(dw), images_(images) {}

ContainerD2D::~ContainerD2D() {
    for (Font* f : fonts_) {
        if (f->format) f->format->Release();
        delete f;
    }
    for (auto& kv : bitmaps_) if (kv.second) kv.second->Release();
    if (brush_) brush_->Release();
    if (layer_) layer_->Release();
}

void ContainerD2D::begin(ID2D1RenderTarget* rt, int width, int height) {
    if (rt != bitmaps_owner_) {
        // Текстури прив'язані до конкретної цілі малювання; змінилася ціль —
        // старі більше не наші.
        for (auto& kv : bitmaps_) if (kv.second) kv.second->Release();
        bitmaps_.clear();
        if (brush_) { brush_->Release(); brush_ = nullptr; }
        if (layer_) { layer_->Release(); layer_ = nullptr; }
        bitmaps_owner_ = rt;
    }
    rt_ = rt;
    width_ = width;
    height_ = height;
    clip_depth_ = 0;
    layer_depth_ = 0;
    transforms_.clear();
}

void ContainerD2D::end() {
    // Догортаємо все, що лишилося відкритим. Так буває лише при збої посеред
    // малювання, але ціль після нього має бути придатною для наступного кадру.
    while (layer_depth_ > 0) { rt_->PopLayer(); --layer_depth_; }
    if (!transforms_.empty()) {
        rt_->SetTransform(transforms_.front());
        transforms_.clear();
    }
    while (clip_depth_ > 0) { rt_->PopAxisAlignedClip(); --clip_depth_; }
    rt_ = nullptr;
}

std::vector<std::string> ContainerD2D::take_missing() {
    std::vector<std::string> out;
    out.swap(missing_);
    return out;
}

ID2D1SolidColorBrush* ContainerD2D::brush(const D2D1_COLOR_F& c) {
    if (!rt_) return nullptr;
    if (!brush_) {
        if (FAILED(rt_->CreateSolidColorBrush(c, &brush_))) return nullptr;
        return brush_;
    }
    brush_->SetColor(c);
    brush_->SetOpacity(1.0f);
    return brush_;
}

// --- шрифти ----------------------------------------------------------------

litehtml::uint_ptr ContainerD2D::create_font(const litehtml::font_description& descr,
                                             const litehtml::document* doc,
                                             litehtml::font_metrics* fm) {
    dtrace("create_font: family=[%s] size=%.2f weight=%d", descr.family.c_str(),
           (double)descr.size, descr.weight);
    Font* f = new Font();
    // Тінь цього елемента: значення приїхало закодованим у text-emphasis-style
    // (litehtml успадковує його так само, як успадковувався б text-shadow).
    // Нічого не приїхало — лишається загальна, зі стилю сторінки.
    f->shadow = shadow_;
    TextShadow parsed;
    if (shadow_from_channel(descr.emphasis_style, &parsed)) f->shadow = parsed;
    f->size = descr.size > 0 ? (float)descr.size : base_font_px_;
    f->decoration = descr.decoration_line;

    // Сімейство: беремо перше ім'я зі списку — DirectWrite сам підставить
    // запасний шрифт для символів, яких у ньому немає (кирилиця, емодзі, CJK),
    // тож перебирати список вручну не потрібно.
    std::string family = descr.family;
    const size_t comma = family.find(',');
    if (comma != std::string::npos) family = family.substr(0, comma);
    while (!family.empty() && (family.front() == ' ' || family.front() == '\'' ||
                               family.front() == '"')) family.erase(family.begin());
    while (!family.empty() && (family.back() == ' ' || family.back() == '\'' ||
                               family.back() == '"')) family.pop_back();
    // Узагальнені імена CSS у DirectWrite не існують — підставляємо системний.
    if (family.empty() || family == "system-ui" || family == "sans-serif" ||
        family == "inherit" || family == "initial")
        family = get_default_font_name();

    dtrace("  сімейство після розбору: [%s]", family.c_str());
    std::wstring wfam = utf8_to_w(family.c_str());
    const DWRITE_FONT_WEIGHT weight = (DWRITE_FONT_WEIGHT)(descr.weight > 0 ? descr.weight : 400);
    const DWRITE_FONT_STYLE style = descr.style == litehtml::font_style_italic
                                        ? DWRITE_FONT_STYLE_ITALIC
                                        : DWRITE_FONT_STYLE_NORMAL;

    HRESULT hr = dw_->CreateTextFormat(wfam.c_str(), nullptr, weight, style,
                                       DWRITE_FONT_STRETCH_NORMAL, f->size, L"",
                                       &f->format);
    if (FAILED(hr) || !f->format) {
        std::wstring fallback = utf8_to_w(get_default_font_name());
        dw_->CreateTextFormat(fallback.c_str(), nullptr, weight, style,
                              DWRITE_FONT_STRETCH_NORMAL, f->size, L"", &f->format);
    }
    if (f->format) {
        // Без переносу: рядок повідомлення ріже на слова сам litehtml, і якщо
        // DirectWrite перенесе ще й усередині «слова», розкладка розійдеться
        // з тим, що litehtml нарахував.
        f->format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    dtrace("  CreateTextFormat -> %p", (void*)f->format);
    // Метрики беремо зі справжнього накреслення, а не на око: від них залежить
    // базова лінія, а отже — чи стоять емоут і текст на одному рівні.
    f->ascent = f->size * 0.8f;
    f->descent = f->size * 0.2f;
    f->x_height = f->size * 0.5f;
    f->ch_width = f->size * 0.5f;
    IDWriteFontCollection* coll = nullptr;
    if (f->format && SUCCEEDED(dw_->GetSystemFontCollection(&coll)) && coll) {
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(coll->FindFamilyName(wfam.c_str(), &idx, &exists)) && exists) {
            IDWriteFontFamily* fam = nullptr;
            if (SUCCEEDED(coll->GetFontFamily(idx, &fam)) && fam) {
                IDWriteFont* font = nullptr;
                if (SUCCEEDED(fam->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL,
                                                        style, &font)) && font) {
                    DWRITE_FONT_METRICS m{};
                    font->GetMetrics(&m);
                    if (m.designUnitsPerEm) {
                        const float k = f->size / (float)m.designUnitsPerEm;
                        f->ascent = m.ascent * k;
                        f->descent = m.descent * k;
                        f->line_gap = m.lineGap * k;
                        f->x_height = m.xHeight * k;
                    }
                    font->Release();
                }
                fam->Release();
            }
        }
        coll->Release();
    }
    dtrace("  метрики: asc=%.2f desc=%.2f gap=%.2f", f->ascent, f->descent, f->line_gap);
    // Цілі пікселі: базова лінія тоді лягає рівно на піксель, і текст виходить
    // чіткішим — DirectWrite не розмазує гліфи по пів пікселя.
    //
    // (Спроба таким чином зійтися з браузером по висоті рядка НЕ вдалася:
    // litehtml однаково дає приблизно на піксель більше на повідомлення, і
    // стрічка виходить на ~2.6% вища. Помітно лише поруч із еталоном.)
    f->ascent = (float)(int)(f->ascent + 0.5f);
    f->descent = (float)(int)(f->descent + 0.5f);

    // Ширина «0» — те, що CSS зве одиницею ch.
    IDWriteTextLayout* probe = layout_for(L"0", f);
    if (probe) {
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(probe->GetMetrics(&tm))) f->ch_width = tm.widthIncludingTrailingWhitespace;
        probe->Release();
    }

    // Базову лінію фіксуємо в самому форматі DirectWrite.
    //
    // Навіщо. DrawTextLayout ставить ВЕРХ рядка, а базова лінія всередині
    // залежить від того, який шрифт DirectWrite фактично підставив під символ.
    // Для емодзі це не Segoe UI, а Segoe UI Emoji — з іншими метриками, і
    // емоут «стрибав» відносно тексту поруч. Задавши інтервал і базову лінію
    // явно, ми отримуємо ОДНЕ й те саме зміщення для будь-якого накреслення.
    if (f->format)
        f->format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                                  f->ascent + f->descent, f->ascent);

    if (fm) {
        fm->font_size = f->size;
        fm->ascent = f->ascent;
        fm->descent = f->descent;
        // Висота — рівно ascent+descent, без line_gap: litehtml кладе базову
        // лінію на «низ мінус base_line()», а base_line() у нього — це descent.
        // Додавши сюди міжрядковий проміжок, ми зсунули б базову лінію рівно на
        // нього, причому по-різному для різних кеглів — саме через це плашки
        // (.b — 0.55em, .money — 0.75em) стояли не на одному рівні з текстом.
        fm->height = f->ascent + f->descent;
        fm->x_height = f->x_height;
        fm->ch_width = f->ch_width;
        fm->draw_spaces = f->decoration != 0;
        fm->sub_shift = f->descent;
        fm->super_shift = f->ascent / 3.0f;
    }

    dtrace("  готово: ch=%.2f, шрифтів у списку %d", f->ch_width, (int)fonts_.size() + 1);
    fonts_.push_back(f);
    return (litehtml::uint_ptr)f;
}

void ContainerD2D::delete_font(litehtml::uint_ptr hFont) {
    Font* f = (Font*)hFont;
    if (!f) return;
    for (size_t i = 0; i < fonts_.size(); ++i) {
        if (fonts_[i] == f) { fonts_.erase(fonts_.begin() + i); break; }
    }
    if (f->format) f->format->Release();
    delete f;
}

IDWriteTextLayout* ContainerD2D::layout_for(const std::wstring& w, const Font* f) {
    if (!f || !f->format) return nullptr;
    IDWriteTextLayout* layout = nullptr;
    // Ширина «нескінченна»: рядок міряємо як він є, переноси робить litehtml.
    if (FAILED(dw_->CreateTextLayout(w.c_str(), (UINT32)w.size(), f->format,
                                     100000.0f, 100000.0f, &layout)))
        return nullptr;
    if (f->decoration & litehtml::text_decoration_line_underline)
        layout->SetUnderline(TRUE, {0, (UINT32)w.size()});
    if (f->decoration & litehtml::text_decoration_line_line_through)
        layout->SetStrikethrough(TRUE, {0, (UINT32)w.size()});
    return layout;
}

litehtml::pixel_t ContainerD2D::text_width(const char* text, litehtml::uint_ptr hFont) {
    const Font* f = (const Font*)hFont;
    if (!f || !text || !*text) return 0;
    std::wstring w = utf8_to_w(text);
    IDWriteTextLayout* layout = layout_for(w, f);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS tm{};
    const HRESULT hr = layout->GetMetrics(&tm);
    layout->Release();
    // Саме widthIncludingTrailingWhitespace: пробіл у кінці слова — теж ширина,
    // і без нього слова в рядку злипаються.
    return SUCCEEDED(hr) ? tm.widthIncludingTrailingWhitespace : 0;
}

void ContainerD2D::draw_run(const std::wstring& w, const Font* f, const D2D1_COLOR_F& c,
                            float x, float y_baseline) {
    IDWriteTextLayout* layout = layout_for(w, f);
    if (!layout) return;
    ID2D1SolidColorBrush* b = brush(c);
    if (b) {
        // DrawTextLayout ставить ВЕРХ рядка, а litehtml дає базову лінію.
        rt_->DrawTextLayout(D2D1::Point2F(x, y_baseline - f->ascent), layout, b,
                            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
    layout->Release();
}

void ContainerD2D::draw_text(litehtml::uint_ptr hdc, const char* text,
                             litehtml::uint_ptr hFont, litehtml::web_color color,
                             const litehtml::position& pos) {
    if (!rt_ || !text || !*text) return;
    const Font* f = (const Font*)hFont;
    if (!f) return;
    const std::wstring w = utf8_to_w(text);
    if (w.empty()) return;

    // Базова лінія за домовленістю litehtml: він розставляє все у рядку по
    // «низ мінус base_line()», а base_line() повертає descent (line_box.cpp).
    // Центрувати текст у прямокутнику, як тут було раніше, не можна: при
    // різних кеглях у рядку (нік, плашка, дрібний системний текст) кожен
    // з'їжджав на свою частку — підписи й виходили не на одному рівні.
    const float baseline = pos.y + pos.height - f->descent;

    const TextShadow& shadow_ref = f->shadow;
    if (shadow_ref.on && shadow_ref.color.a > 0.0f) {
        // Розмиття робимо кількома зсувами по колу замість справжнього гауса:
        // тінь тут — тонка обводка під текстом заради читабельності поверх
        // гри, і на 2–3 пікселях різниці не видно, зате не треба заводити
        // окрему ціль малювання й ефект на кожен рядок.
        const float r = shadow_ref.blur * 0.5f;
        D2D1_COLOR_F sc = to_d2d(shadow_ref.color);
        if (r < 0.5f) {
            draw_run(w, f, sc, pos.x + shadow_ref.dx, baseline + shadow_ref.dy);
        } else {
            const float taps[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                      {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
            sc.a = shadow_ref.color.a * 0.45f;
            for (const auto& t : taps)
                draw_run(w, f, sc, pos.x + shadow_ref.dx + t[0] * r,
                         baseline + shadow_ref.dy + t[1] * r);
            sc.a = shadow_ref.color.a * 0.8f;
            draw_run(w, f, sc, pos.x + shadow_ref.dx, baseline + shadow_ref.dy);
        }
    }
    draw_run(w, f, to_d2d(color), pos.x, baseline);
}

litehtml::pixel_t ContainerD2D::pt_to_px(float pt) const { return pt * 96.0f / 72.0f; }
litehtml::pixel_t ContainerD2D::get_default_font_size() const { return base_font_px_; }
const char* ContainerD2D::get_default_font_name() const { return "Segoe UI"; }

void ContainerD2D::draw_list_marker(litehtml::uint_ptr hdc,
                                    const litehtml::list_marker& marker) {
    // Списків у чаті немає; малюємо простий кружечок, щоб чужий CSS із
    // list-style не давав порожнечу.
    if (!rt_) return;
    ID2D1SolidColorBrush* b = brush(to_d2d(marker.color));
    if (!b) return;
    const float r = marker.pos.height / 3.0f;
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(marker.pos.x + marker.pos.width / 2,
                                                 marker.pos.y + marker.pos.height / 2), r, r), b);
}

// --- картинки ---------------------------------------------------------------

void ContainerD2D::load_image(const char* src, const char* baseurl, bool redraw_on_ready) {
    if (!src || !*src) return;
    dtrace("load_image: %d байтів адреси", (int)strlen(src));
    const std::string url(src);
    if (images_->get(url)) { dtrace("  вже є"); return; }               // уже є (або «data:» щойно розібрали)
    if (images_->known(url)) return;             // уже пробували й не вийшло
    for (const auto& m : missing_) if (m == url) return;
    missing_.push_back(url);
}

void ContainerD2D::get_image_size(const char* src, const char* baseurl, litehtml::size& sz) {
    sz.width = 0;
    sz.height = 0;
    if (!src || !*src) return;
    const Image* img = images_->get(src);
    if (!img) return;
    // Природний, а не растровий: інакше SVG-значок, растеризований із запасом,
    // прикинеться величезним і рознесе розкладку рядка.
    sz.width = (litehtml::pixel_t)img->nat_width;
    sz.height = (litehtml::pixel_t)img->nat_height;
}

ID2D1Bitmap* ContainerD2D::bitmap_for(const std::string& url, const Image* img) {
    auto it = bitmaps_.find(url);
    if (it != bitmaps_.end()) return it->second;
    if (!rt_ || !img || !img->ok()) return nullptr;

    const ImageFrame& f = img->frames[0];
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap* bmp = nullptr;
    if (FAILED(rt_->CreateBitmap(D2D1::SizeU((UINT32)f.width, (UINT32)f.height),
                                 f.bgra.data(), (UINT32)(f.width * 4), &props, &bmp)))
        return nullptr;
    bitmaps_[url] = bmp;
    return bmp;
}

// Кеш картинок викинув адресу — викидаємо й текстуру. Інакше вона лишалася б
// висіти до кінця кадру життя вікна й показувала б старе.
void ContainerD2D::forget_image(const std::string& url) {
    auto it = bitmaps_.find(url);
    if (it == bitmaps_.end()) return;
    if (it->second) it->second->Release();
    bitmaps_.erase(it);
}

void ContainerD2D::blit(const std::string& url, const litehtml::background_layer& layer,
                        const Image* img) {
    ID2D1Bitmap* bmp = bitmap_for(url, img);
    if (!bmp) return;

    const D2D1_RECT_F clip = to_rect(layer.clip_box);
    rt_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

    const D2D1_RECT_F dst = to_rect(layer.origin_box);
    const float w = layer.origin_box.width, h = layer.origin_box.height;
    if (w > 0 && h > 0) {
        const bool rx = layer.repeat == litehtml::background_repeat_repeat ||
                        layer.repeat == litehtml::background_repeat_repeat_x;
        const bool ry = layer.repeat == litehtml::background_repeat_repeat ||
                        layer.repeat == litehtml::background_repeat_repeat_y;
        if (!rx && !ry) {
            rt_->DrawBitmap(bmp, dst, 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // Повторення трапляється лише в чужому CSS (background-image на
            // .m). Малюємо плитками в межах відсікання — простіше й надійніше
            // за брашеві перетворення.
            const float x0 = rx ? clip.left - std::fmod(clip.left - dst.left, w) - w : dst.left;
            const float y0 = ry ? clip.top - std::fmod(clip.top - dst.top, h) - h : dst.top;
            for (float y = y0; y < (ry ? clip.bottom : dst.top + 0.5f); y += h) {
                for (float x = x0; x < (rx ? clip.right : dst.left + 0.5f); x += w) {
                    rt_->DrawBitmap(bmp, D2D1::RectF(x, y, x + w, y + h), 1.0f,
                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    if (!rx) break;
                }
                if (!ry) break;
            }
        }
    }
    rt_->PopAxisAlignedClip();
}

void ContainerD2D::draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const std::string& url, const std::string& base_url) {
    if (!rt_) return;
    const Image* img = images_->get(url);
    if (!img) { load_image(url.c_str(), base_url.c_str(), true); return; }

    // Анімований емоут у картинку рядка не запікаємо: рядок малюється один
    // раз і назавжди, а емоут має жити далі. Запам'ятовуємо місце — кадри
    // покладе поверх стрічка (Feed::draw), щокадру й лише тут.
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
    blit(url, layer, img);
}

std::vector<Sprite> ContainerD2D::take_sprites() {
    std::vector<Sprite> out;
    out.swap(sprites_);
    return out;
}

void ContainerD2D::draw_solid_fill(litehtml::uint_ptr hdc,
                                   const litehtml::background_layer& layer,
                                   const litehtml::web_color& color) {
    if (!rt_ || color.alpha == 0) return;
    ID2D1SolidColorBrush* b = brush(to_d2d(color));
    if (!b) return;

    const D2D1_RECT_F box = to_rect(layer.border_box);
    const float r = max_radius(layer.border_radius);
    if (r > 0.5f && same_radius(layer.border_radius)) {
        rt_->FillRoundedRectangle(D2D1::RoundedRect(box, r, r), b);
        return;
    }
    if (r > 0.5f) {
        // Кути різні — заливаємо по відсіканню найбільшим радіусом: краще
        // трохи не той кут, ніж прямокутник поверх скругленої рамки.
        rt_->PushAxisAlignedClip(to_rect(layer.clip_box), D2D1_ANTIALIAS_MODE_ALIASED);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(box, r, r), b);
        rt_->PopAxisAlignedClip();
        return;
    }
    rt_->FillRectangle(box, b);
}

void ContainerD2D::draw_linear_gradient(litehtml::uint_ptr hdc,
                                        const litehtml::background_layer& layer,
                                        const litehtml::background_layer::linear_gradient& g) {
    if (!rt_ || g.color_points.empty()) return;
    std::vector<D2D1_GRADIENT_STOP> stops;
    stops.reserve(g.color_points.size());
    for (const auto& cp : g.color_points)
        stops.push_back({cp.offset, to_d2d(cp.color)});

    ID2D1GradientStopCollection* coll = nullptr;
    if (FAILED(rt_->CreateGradientStopCollection(stops.data(), (UINT32)stops.size(), &coll)))
        return;
    ID2D1LinearGradientBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(g.start.x, g.start.y),
                                                D2D1::Point2F(g.end.x, g.end.y)),
            coll, &b))) {
        rt_->PushAxisAlignedClip(to_rect(layer.clip_box), D2D1_ANTIALIAS_MODE_ALIASED);
        const float r = max_radius(layer.border_radius);
        if (r > 0.5f)
            rt_->FillRoundedRectangle(D2D1::RoundedRect(to_rect(layer.border_box), r, r), b);
        else
            rt_->FillRectangle(to_rect(layer.border_box), b);
        rt_->PopAxisAlignedClip();
        b->Release();
    }
    coll->Release();
}

void ContainerD2D::draw_radial_gradient(litehtml::uint_ptr hdc,
                                        const litehtml::background_layer& layer,
                                        const litehtml::background_layer::radial_gradient& g) {
    if (!rt_ || g.color_points.empty()) return;
    std::vector<D2D1_GRADIENT_STOP> stops;
    stops.reserve(g.color_points.size());
    for (const auto& cp : g.color_points)
        stops.push_back({cp.offset, to_d2d(cp.color)});

    ID2D1GradientStopCollection* coll = nullptr;
    if (FAILED(rt_->CreateGradientStopCollection(stops.data(), (UINT32)stops.size(), &coll)))
        return;
    ID2D1RadialGradientBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(g.position.x, g.position.y),
                                                D2D1::Point2F(0, 0), g.radius.x, g.radius.y),
            coll, &b))) {
        rt_->PushAxisAlignedClip(to_rect(layer.clip_box), D2D1_ANTIALIAS_MODE_ALIASED);
        rt_->FillRectangle(to_rect(layer.border_box), b);
        rt_->PopAxisAlignedClip();
        b->Release();
    }
    coll->Release();
}

void ContainerD2D::draw_conic_gradient(litehtml::uint_ptr hdc,
                                       const litehtml::background_layer& layer,
                                       const litehtml::background_layer::conic_gradient& g) {
    // Конічного градієнта в Direct2D 1.1 немає. Замість «нічого» беремо
    // перший колір заливкою: чужа тема з conic-gradient лишиться читабельною,
    // хоч і без переливу. Так само поводяться старі браузери.
    if (!rt_ || g.color_points.empty()) return;
    litehtml::web_color c = g.color_points.front().color;
    draw_solid_fill(hdc, layer, c);
}

void ContainerD2D::draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders,
                                const litehtml::position& draw_pos, bool root) {
    if (!rt_ || !borders.is_visible()) return;

    const float r = max_radius(borders.radius);
    const bool uniform = borders.left.width == borders.right.width &&
                         borders.top.width == borders.bottom.width &&
                         borders.left.width == borders.top.width &&
                         borders.left.style == borders.top.style &&
                         borders.left.style == borders.right.style &&
                         borders.left.style == borders.bottom.style &&
                         borders.left.color == borders.top.color &&
                         borders.left.color == borders.right.color &&
                         borders.left.color == borders.bottom.color;

    // Рівна рамка по колу — один виклик і правильні скруглення. Це найчастіший
    // випадок у темах («border: 2px solid …», «border-radius: 10px»).
    if (uniform && borders.left.width > 0 &&
        borders.left.style != litehtml::border_style_none &&
        borders.left.style != litehtml::border_style_hidden) {
        ID2D1SolidColorBrush* b = brush(to_d2d(borders.left.color));
        if (!b) return;
        const float w = borders.left.width;
        const D2D1_RECT_F rc = D2D1::RectF(draw_pos.x + w / 2, draw_pos.y + w / 2,
                                           draw_pos.right() - w / 2, draw_pos.bottom() - w / 2);
        if (r > 0.5f)
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), b, w);
        else
            rt_->DrawRectangle(rc, b, w);
        return;
    }

    // Різні сторони — малюємо смугами. Саме так виглядає «border-left: 3px
    // solid …», яким у темах позначають площадку.
    auto side = [&](const litehtml::border& bd, D2D1_RECT_F rc) {
        if (bd.width <= 0 || bd.style == litehtml::border_style_none ||
            bd.style == litehtml::border_style_hidden || bd.color.alpha == 0)
            return;
        ID2D1SolidColorBrush* b = brush(to_d2d(bd.color));
        if (b) rt_->FillRectangle(rc, b);
    };
    const float x0 = draw_pos.x, y0 = draw_pos.y;
    const float x1 = draw_pos.right(), y1 = draw_pos.bottom();
    side(borders.top,    D2D1::RectF(x0, y0, x1, y0 + borders.top.width));
    side(borders.bottom, D2D1::RectF(x0, y1 - borders.bottom.width, x1, y1));
    side(borders.left,   D2D1::RectF(x0, y0, x0 + borders.left.width, y1));
    side(borders.right,  D2D1::RectF(x1 - borders.right.width, y0, x1, y1));
}

// --- решта ------------------------------------------------------------------

void ContainerD2D::transform_text(litehtml::string& text, litehtml::text_transform tt) {
    if (text.empty()) return;
    std::wstring w = utf8_to_w(text.c_str());
    if (w.empty()) return;
    switch (tt) {
        case litehtml::text_transform_uppercase:
            CharUpperBuffW(&w[0], (DWORD)w.size());
            break;
        case litehtml::text_transform_lowercase:
            CharLowerBuffW(&w[0], (DWORD)w.size());
            break;
        case litehtml::text_transform_capitalize: {
            bool start = true;
            for (wchar_t& c : w) {
                if (start && iswalpha(c)) { CharUpperBuffW(&c, 1); start = false; }
                else if (iswspace(c)) start = true;
            }
            break;
        }
        default:
            return;
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    text = out;
}

void ContainerD2D::import_css(litehtml::string& text, const litehtml::string& url,
                              litehtml::string& baseurl) {
    // @import мовчки ігноруємо: сторінка чату має лишатися самодостатньою й не
    // залежати від мережі там, де вона не потрібна.
    text.clear();
}

void ContainerD2D::set_clip(const litehtml::position& pos,
                            const litehtml::border_radiuses& bdr_radius) {
    if (!rt_) return;
    rt_->PushAxisAlignedClip(to_rect(pos), D2D1_ANTIALIAS_MODE_ALIASED);
    ++clip_depth_;
}

void ContainerD2D::del_clip() {
    if (!rt_ || clip_depth_ <= 0) return;
    rt_->PopAxisAlignedClip();
    --clip_depth_;
}

bool ContainerD2D::push_transform(const litehtml::position& border_box,
                                  const std::string& value,
                                  litehtml::pixel_t font_size) {
    if (!rt_ || value.empty()) return false;

    // Розбір значення й побудова матриці — спільні для всіх систем
    // (cssbits): порядок [a b c d e f] тут і в Blend2D той самий.
    float m[6];
    if (!parse_transform(value, font_size, border_box, m)) return false;

    // Множимо на те, що вже стоїть: перетворення вкладаються, і зовнішнє
    // має лишитися чинним для вкладеного.
    D2D1_MATRIX_3X2_F prev;
    rt_->GetTransform(&prev);
    transforms_.push_back(prev);
    const D2D1::Matrix3x2F mine(m[0], m[1], m[2], m[3], m[4], m[5]);
    const D2D1::Matrix3x2F outer(prev._11, prev._12, prev._21, prev._22,
                                 prev._31, prev._32);
    rt_->SetTransform(mine * outer);
    return true;
}

void ContainerD2D::pop_transform() {
    if (!rt_ || transforms_.empty()) return;
    rt_->SetTransform(transforms_.back());
    transforms_.pop_back();
}

void ContainerD2D::draw_box_shadow(litehtml::uint_ptr hdc,
                                   const litehtml::position& border_box,
                                   const litehtml::border_radiuses& radius,
                                   const std::string& value,
                                   litehtml::pixel_t font_size) {
    if (!rt_ || value.empty()) return;

    // Кілька тіней через кому — малюємо від останньої до першої: у CSS перша
    // лежить НАД рештою.
    const std::vector<std::string> parts = split_shadow_list(value);

    for (size_t k = parts.size(); k-- > 0; ) {
        BoxShadow sh;
        if (!parse_box_shadow(parts[k], font_size, &sh)) continue;
        // Внутрішні тіні (inset) поки не малюємо: у чаті вони не трапляються,
        // а зробити їх абияк — гірше, ніж не робити (див. README).
        if (sh.inset) continue;

        ID2D1SolidColorBrush* b = brush(to_d2d(sh.color));
        if (!b) continue;

        const float r = max_radius(radius) + sh.spread;
        // Розмиття — кількома розширеними контурами з малою альфою. Справжній
        // гаус вимагав би окремої цілі й ефекту на кожну тінь; для тіні під
        // рядком чату різниці не видно, а коштує це в рази менше.
        const int steps = sh.blur > 0.5f ? 5 : 1;
        for (int i = steps; i >= 1; --i) {
            const float grow = sh.spread + sh.blur * ((float)i / (float)steps) * 0.5f;
            D2D1_COLOR_F c = to_d2d(sh.color);
            c.a = sh.color.a * (steps == 1 ? 1.0f : 0.9f / (float)steps);
            b->SetColor(c);
            const D2D1_RECT_F rc = D2D1::RectF(
                border_box.x + sh.dx - grow, border_box.y + sh.dy - grow,
                border_box.right() + sh.dx + grow, border_box.bottom() + sh.dy + grow);
            const float rr = r > 0.5f ? r + grow : 0.0f;
            if (rr > 0.5f)
                rt_->FillRoundedRectangle(D2D1::RoundedRect(rc, rr, rr), b);
            else
                rt_->FillRectangle(rc, b);
        }
    }
}

void ContainerD2D::push_opacity(float opacity) {
    if (!rt_) return;
    // Один шар на всі виклики: Direct2D дозволяє вкладати той самий об'єкт, а
    // створювати новий на кожен напівпрозорий рядок — марна робота.
    if (!layer_ && FAILED(rt_->CreateLayer(nullptr, &layer_))) return;
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         D2D1::IdentityMatrix(), opacity),
                   layer_);
    ++layer_depth_;
}

void ContainerD2D::pop_opacity() {
    if (!rt_ || layer_depth_ <= 0) return;
    rt_->PopLayer();
    --layer_depth_;
}

void ContainerD2D::get_viewport(litehtml::position& viewport) const {
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = (litehtml::pixel_t)width_;
    viewport.height = (litehtml::pixel_t)height_;
}

litehtml::element::ptr ContainerD2D::create_element(
    const char* tag_name, const litehtml::string_map& attributes,
    const std::shared_ptr<litehtml::document>& doc) {
    return nullptr;      // своїх тегів у чаті немає
}

void ContainerD2D::get_media_features(litehtml::media_features& media) const {
    media.type = litehtml::media_type_screen;
    media.width = (litehtml::pixel_t)width_;
    media.height = (litehtml::pixel_t)height_;
    media.device_width = (litehtml::pixel_t)width_;
    media.device_height = (litehtml::pixel_t)height_;
    media.color = 8;
    media.monochrome = 0;
    media.color_index = 256;
    media.resolution = 96;
}

void ContainerD2D::get_language(litehtml::string& language, litehtml::string& culture) const {
    language = "uk";
    culture = "";
}

void ContainerD2D::split_text(const char* text,
                              const std::function<void(const char*)>& on_word,
                              const std::function<void(const char*)>& on_space) {
    // Правила переносу спільні для всіх систем — див. cssbits.cpp.
    split_text_words(text, on_word, on_space);
}

}  // namespace hominka
