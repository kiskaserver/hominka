#include "core/feed.h"

#include <cmath>
#include <cstring>

#include "core/page_assets.h"

namespace hominka {

namespace {

// Значення властивості з правила для заданого селектора.
//
// Розкладку стовпця ми складаємо нативно (див. коментар угорі), тож кілька
// правил доводиться прочитати самим: відступи й проміжок стрічки, напрямок
// стовпця, і чи не просили вимкнути появу рядка. Це не парсер CSS — рівно ці
// кілька випадків.
//
// Береться ОСТАННЄ правило з таким селектором: як і в браузері, виграє те, що
// нижче, а користувацький CSS іде після базового.
bool find_rule_value(const std::string& css, const char* selector, const char* prop,
                     std::string* out) {
    const size_t sel_len = strlen(selector);
    size_t pos = 0, found = std::string::npos;
    while ((pos = css.find(selector, pos)) != std::string::npos) {
        // Саме селектор, а не шматок довшого імені («.m» усередині «.money»).
        const size_t after = pos + sel_len;
        const bool ok_after = after >= css.size() ||
                              (!isalnum((unsigned char)css[after]) && css[after] != '-' &&
                               css[after] != '_');
        if (ok_after) found = pos;
        pos = after;
    }
    if (found == std::string::npos) return false;
    const size_t open = css.find('{', found);
    if (open == std::string::npos) return false;
    size_t close = css.find('}', open);
    if (close == std::string::npos) close = css.size();
    const std::string body = css.substr(open + 1, close - open - 1);

    const size_t plen = strlen(prop);
    size_t p = 0;
    while ((p = body.find(prop, p)) != std::string::npos) {
        const bool word = p == 0 || (!isalnum((unsigned char)body[p - 1]) &&
                                     body[p - 1] != '-' && body[p - 1] != '_');
        const size_t colon = body.find(':', p);
        if (word && colon != std::string::npos &&
            (colon == p + plen || body.find_first_not_of(" 	", p + plen) == colon)) {
            size_t end = body.find_first_of(";", colon);
            if (end == std::string::npos) end = body.size();
            std::string v = body.substr(colon + 1, end - colon - 1);
            while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
            while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
            *out = v;
            return true;
        }
        p += plen;
    }
    return false;
}

bool find_list_length(const std::string& css, const char* prop, int* out) {
    std::string v;
    if (!find_rule_value(css, "#list", prop, &v)) return false;
    *out = (int)(atof(v.c_str()) + 0.5);
    return true;
}

// Плавність появи — та сама, що в «animation: .18s ease-out» на сторінці.
float ease_out(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }

}  // namespace

Feed::Feed(GfxFonts* fonts, ImageCache* images)
    : fonts_(fonts), images_(images), container_(fonts, images) {
    layout_ = clean_layout({});
    shadow_ = parse_text_shadow(BASE_CSS);
}

Feed::~Feed() {
    for (auto& it : items_) release_bitmap(&it);
    for (auto& kv : anim_) gfx_release(kv.second);
}

void Feed::release_bitmap(Item* it) {
    // Растр належить полотну, тож окремо його не звільняємо — лише забуваємо.
    it->bitmap = nullptr;
    if (it->surface) { gfx_release(it->surface); it->surface = nullptr; }
    // Дірки під анімовані емоути знайдуться наново під час растеризації.
    it->sprites.clear();
}

void Feed::drop_bitmaps() {
    for (auto& it : items_) release_bitmap(&it);
    // Кадрові текстури належать тій самій цілі, що й картинки рядків.
    for (auto& kv : anim_) gfx_release(kv.second);
    anim_.clear();
    next_anim_ms_ = 0;
}

void Feed::drop_layout() {
    for (auto& it : items_) {
        release_bitmap(&it);
        it.doc.reset();
        it.content = 0;
        // Розкладка змінилася — хто куди потрапить, вирішиться наново.
        it.offscreen = false;
    }
}

void Feed::set_css(const std::string& css) {
    if (css_ == css) return;
    css_ = css;
    const std::string all = std::string(BASE_CSS) + "\n" + css_;
    // Тінь може бути перевизначена своїм CSS; виграє те, що нижче.
    shadow_ = parse_text_shadow(all);
    int v = 0;
    inset_ = find_list_length(all, "inset", &v) ? v : LIST_INSET_DEFAULT;
    gap_ = find_list_length(all, "gap", &v) ? v : LIST_GAP_DEFAULT;

    // «#list { flex-direction: column-reverse }» — новіші рядки згори, а не
    // знизу. Напрямок стовпця складаємо ми, тож і читаємо його самі.
    std::string val;
    reversed_ = find_rule_value(all, "#list", "flex-direction", &val) &&
                val.find("column-reverse") != std::string::npos;

    // «.m { animation: none }» — вимкнути появу рядка. Анімацію малює програма,
    // а не CSS, але сам ЗАПИС лишився тим самим: людина пише те, що писала в
    // браузері, і воно так само діє. Інакше довелося б вигадувати свою
    // властивість і пояснювати, чому старий рецепт більше не працює.
    enter_anim_ = !(find_rule_value(all, ".m", "animation", &val) &&
                    val.find("none") != std::string::npos);
    drop_layout();
}

void Feed::set_layout(const std::vector<std::string>& layout) {
    const std::vector<std::string> clean = clean_layout(layout);
    if (clean == layout_) return;
    layout_ = clean;
    drop_layout();
}

void Feed::set_zoom(float zoom) {
    const float z = zoom < 0.3f ? 0.3f : (zoom > 4.0f ? 4.0f : zoom);
    if (std::fabs(z - zoom_) < 0.001f) return;
    zoom_ = z;
    drop_layout();
}

void Feed::set_width(int width) {
    if (width <= 0 || width == width_) return;
    width_ = width;
    drop_layout();
}

void Feed::add(const ChatMessage& m, int64_t now_ms) {
    Item it;
    it.msg = m;
    it.added_ms = now_ms;
    items_.push_back(std::move(it));
    while (items_.size() > FEED_MAX_MESSAGES) {
        release_bitmap(&items_.front());
        items_.pop_front();
    }
}

void Feed::remove_id(const std::string& id) {
    if (id.empty()) return;
    for (size_t i = 0; i < items_.size(); ) {
        if (items_[i].msg.id == id) {
            release_bitmap(&items_[i]);
            items_.erase(items_.begin() + i);
        } else {
            ++i;
        }
    }
}

void Feed::purge_nick(const std::string& nick) {
    if (nick.empty()) return;
    for (size_t i = 0; i < items_.size(); ) {
        if (items_[i].msg.nick == nick) {
            release_bitmap(&items_[i]);
            items_.erase(items_.begin() + i);
        } else {
            ++i;
        }
    }
}

void Feed::clear() {
    for (auto& it : items_) release_bitmap(&it);
    items_.clear();
}

void Feed::on_image_arrived(const std::string& url) {
    if (url.empty()) return;
    // Перекладаємо наново лише ті рядки, які цю картинку справді чекали:
    // емоут у тексті або значок автора. Решта вже правильна.
    for (auto& it : items_) {
        bool wanted = false;
        for (const auto& e : it.msg.emotes) if (e.url == url) { wanted = true; break; }
        if (!wanted)
            for (const auto& b : it.msg.badge_icons) if (b.url == url) { wanted = true; break; }
        if (!wanted) continue;
        release_bitmap(&it);
        it.doc.reset();
        it.content = 0;
    }
}

std::vector<std::string> Feed::take_missing() {
    std::vector<std::string> out;
    out.swap(missing_);
    return out;
}

void Feed::enter_state(const Item& it, int64_t now_ms, float* alpha, float* dy) const {
    const int64_t age = now_ms - it.added_ms;
    if (!enter_anim_ || age >= FEED_ENTER_MS || age < 0) { *alpha = 1.0f; *dy = 0.0f; return; }
    const float t = ease_out((float)age / (float)FEED_ENTER_MS);
    *alpha = t;
    // Той самий зсув, що в «transform: translateY(6px)» на сторінці.
    *dy = 6.0f * (1.0f - t);
}

bool Feed::dirty(int64_t now_ms) const {
    for (const auto& it : items_) {
        // Те, що вище краю вікна, до кадру не входить — і не привід його
        // перемальовувати.
        if (it.offscreen) continue;
        if (!it.ready()) return true;                            // ще не малювали
        if (enter_anim_ && now_ms - it.added_ms < FEED_ENTER_MS) return true;  // зʼявляється
    }
    // Емоут перемкнув кадр. Момент цей порахував попередній draw(), тому тут
    // лишається одне порівняння — і жодного обходу картинок.
    if (next_anim_ms_ && now_ms >= next_anim_ms_) return true;
    return false;
}

void Feed::set_animate(bool on) {
    if (animate_ == on) return;
    animate_ = on;
    // Спрайти визначаються під час растеризації, тож картинки рядків треба
    // зробити наново: інакше дірки лишилися б дірками.
    drop_bitmaps();
}

std::set<std::string> Feed::animated_in_use() const {
    std::set<std::string> out;
    for (const auto& it : items_) {
        // Рядок за краєм вікна анімувати нема кому — його емоути не тримаємо.
        // Саме це й робить множину малою: видно десяток рядків, а не всі 80.
        if (it.offscreen) continue;
        // Беремо адреси з САМОГО ПОВІДОМЛЕННЯ, а не зі спрайтів: спрайти
        // зʼявляються лише під час растеризації, і щойно доданий рядок їх ще
        // не має. Інакше емоут, який щойно приїхав, викинули б рівно перед
        // тим, як його вперше намалювати.
        for (const auto& e : it.msg.emotes) out.insert(e.url);
        for (const auto& b : it.msg.badge_icons) out.insert(b.url);
    }
    return out;
}

void Feed::forget_image(const std::string& url) {
    for (auto it = anim_.begin(); it != anim_.end(); ) {
        if (it->first.url == url) {
            gfx_release(it->second);
            it = anim_.erase(it);
        } else {
            ++it;
        }
    }
}

void Feed::trim_anim() {
    const std::set<std::string> keep = animated_in_use();
    for (auto it = anim_.begin(); it != anim_.end(); ) {
        if (keep.find(it->first.url) == keep.end()) {
            gfx_release(it->second);
            it = anim_.erase(it);
        } else {
            ++it;
        }
    }
}

GfxRaster* Feed::anim_bitmap(const std::string& url, int frame, GfxTarget* rt) {
    AnimKey key{url, frame};
    auto it = anim_.find(key);
    if (it != anim_.end()) return it->second;

    const Image* img = images_->get(url);
    if (!img || frame < 0 || (size_t)frame >= img->frames.size()) return nullptr;
    const ImageFrame& f = img->frames[(size_t)frame];

    GfxRaster* bmp = gfx_raster_from_bgra(rt, f.bgra.data(), f.width, f.height);
    if (!bmp) return nullptr;
    anim_[key] = bmp;
    return bmp;
}

bool Feed::ensure_layout(Item* it) {
    if (it->laid()) return true;
    const int inner = width_ - inset_ * 2;
    if (inner <= 0) return false;

    // Розмітку складаємо, знаючи, які картинки вже є: значка без іконки не
    // буває — буде текстова плашка, емоута без картинки — буде його код.
    const ImageReady ready = [this](const std::string& url) {
        return images_->get(url) != nullptr;
    };
    const std::string html = message_document(it->msg, layout_, css_, ready);

    container_.set_text_shadow(shadow_);
    container_.set_base_font_size(20.0f * zoom_);
    container_.begin(nullptr, inner, 1);            // лише розкладка, без цілі

    litehtml::document::ptr doc =
        litehtml::document::createFromString(html.c_str(), &container_);
    if (!doc) return false;
    doc->render((litehtml::pixel_t)inner);
    container_.end();

    for (auto& u : container_.take_missing()) {
        bool have = false;
        for (const auto& m : missing_) if (m == u) { have = true; break; }
        if (!have) missing_.push_back(u);
    }

    const int content = (int)doc->height();
    if (content <= 0) return false;

    // Запас під тінь: угору — на радіус розмиття мінус зсув, униз — плюс зсув.
    // Тінь у чаті спрямована вниз, тож знизу її більше.
    const int blur = shadow_.on ? (int)(shadow_.blur + 1) : 0;
    it->doc = doc;
    it->content = content;
    it->pad_top = shadow_.on ? (int)(blur - shadow_.dy > 0 ? blur - shadow_.dy : 0) : 0;
    it->pad_bottom = shadow_.on ? (int)(blur + (shadow_.dy > 0 ? shadow_.dy : 0)) : 0;
    return true;
}

bool Feed::ensure_bitmap(Item* it, GfxTarget* rt) {
    if (it->ready()) return true;
    if (!rt || !ensure_layout(it)) return false;

    const int inner = width_ - inset_ * 2;
    // Картинка рядка живе на тому ж пристрої, що й ціль: далі її досить
    // покласти одним DrawBitmap, без жодного дотику до CPU.
    GfxSurface* surface = gfx_offscreen(rt, inner, it->height());
    if (!surface) return false;

    container_.set_text_shadow(shadow_);
    container_.set_base_font_size(20.0f * zoom_);
    container_.set_record_sprites(animate_);
    GfxTarget* into = gfx_begin(surface);
    if (!into) { gfx_release(surface); return false; }
    container_.begin(into, inner, it->height());
    it->doc->draw((litehtml::uint_ptr)0, 0, (litehtml::pixel_t)it->pad_top, nullptr);
    container_.end();
    it->sprites = container_.take_sprites();
    container_.set_record_sprites(false);
    if (!gfx_end(surface)) { gfx_release(surface); return false; }

    GfxRaster* bmp = gfx_raster(surface);
    if (!bmp) { gfx_release(surface); return false; }
    it->surface = surface;
    it->bitmap = bmp;
    return true;
}

int Feed::content_height() {
    int total = inset_ * 2;
    int drawn = 0;
    for (auto& it : items_) {
        if (!ensure_layout(&it)) continue;
        total += it.content + gap_;
        ++drawn;
    }
    if (drawn) total -= gap_;
    return total;
}

bool Feed::draw(GfxTarget* rt, int view_w, int view_h, int64_t now_ms) {
    if (!rt) return false;
    if (rt != owner_) {
        // Ціль змінилася (переріс свопчейн) — картинки належали їй, а от
        // розкладка не залежить від цілі й лишається.
        drop_bitmaps();
        owner_ = rt;
    }

    // Знизу вгору: останнє повідомлення найнижче. Це те саме, що
    // «flex-direction: column; justify-content: flex-end» на сторінці, але без
    // перерахунку всієї стрічки на кожне нове повідомлення.
    //
    // «column-reverse» перевертає: новіші згори, стрічка росте вниз.
    float y = reversed_ ? (float)inset_ : (float)(view_h - inset_);
    size_t first_drawn = items_.size();
    int64_t next = 0;                    // найближча зміна кадру анімації
    for (size_t i = items_.size(); i-- > 0; ) {
        Item& it = items_[i];
        if (!ensure_bitmap(&it, rt)) continue;
        first_drawn = i;
        it.offscreen = false;

        float alpha = 1.0f, shift = 0.0f;
        enter_state(it, now_ms, &alpha, &shift);
        // Рядок виїжджає з того боку, з якого приходить.
        if (reversed_) shift = -shift;

        const float content_top = reversed_ ? y : y - (float)it.content;
        const float img_top = content_top - (float)it.pad_top + shift;
        // Вийшли за край вікна — далі не видно.
        if (reversed_ ? img_top > (float)view_h : img_top + (float)it.height() < 0) break;

        gfx_blit(rt, it.bitmap, (float)inset_, img_top,
                 (float)(inset_ + width_ - inset_ * 2), img_top + (float)it.height(),
                 alpha);

        // Анімовані емоути — поверх готового рядка, у дірки, лишені під час
        // растеризації. Робота тут пропорційна КІЛЬКОСТІ ЕМОУТІВ У КАДРІ, а не
        // розміру стрічки: сам рядок уже намальований і не чіпається.
        for (const auto& s : it.sprites) {
            const Image* img = images_->get(s.url);
            if (!img || !img->animated()) continue;
            const int fi = img->frame_at(now_ms);
            GfxRaster* fb = anim_bitmap(s.url, fi, rt);
            if (!fb) continue;
            gfx_blit(rt, fb, (float)inset_ + s.x, img_top + s.y,
                     (float)inset_ + s.x + s.w, img_top + s.y + s.h, alpha);

            // Коли цьому емоуту пора перемкнути кадр. Найближчий із усіх
            // видимих і буде моментом, коли стрічку варто перемалювати.
            const int64_t at = now_ms + img->ms_to_next(now_ms);
            if (!next && at > now_ms) next = at;
            else if (at > now_ms && at < next) next = at;
        }

        y = reversed_ ? content_top + (float)it.content + (float)gap_
                      : content_top - (float)gap_;
        if (reversed_ ? y > (float)view_h : y < 0) break;
    }
    // Усе, що лишилося вище намальованого, у кадр не входить.
    for (size_t i = 0; i < first_drawn; ++i) items_[i].offscreen = true;
    // Жодного видимого анімованого емоута — нема чого й чекати: стрічка знову
    // засинає намертво, як до всієї цієї анімації.
    next_anim_ms_ = next;
    return true;
}

}  // namespace hominka
