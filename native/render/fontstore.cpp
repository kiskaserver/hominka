#include "fontstore.h"

#include <fontconfig/fontconfig.h>

#include <cmath>
#include <cstring>

#include "cssbits.h"

namespace hominka {

namespace {

// Вага CSS (100..900) у шкалу fontconfig.
int fc_weight(int css) {
    if (css <= 100) return FC_WEIGHT_THIN;
    if (css <= 200) return FC_WEIGHT_EXTRALIGHT;
    if (css <= 300) return FC_WEIGHT_LIGHT;
    if (css <= 400) return FC_WEIGHT_REGULAR;
    if (css <= 500) return FC_WEIGHT_MEDIUM;
    if (css <= 600) return FC_WEIGHT_DEMIBOLD;
    if (css <= 700) return FC_WEIGHT_BOLD;
    if (css <= 800) return FC_WEIGHT_EXTRABOLD;
    return FC_WEIGHT_BLACK;
}

// Розбирає список родин із CSS: «'Segoe UI', system-ui, sans-serif».
std::vector<std::string> family_list(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool quote = false;
    char q = 0;
    for (char c : s) {
        if (quote) {
            if (c == q) quote = false;
            else cur += c;
            continue;
        }
        if (c == '"' || c == '\'') { quote = true; q = c; continue; }
        if (c == ',') { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    out.push_back(cur);
    for (std::string& f : out) {
        while (!f.empty() && isspace((unsigned char)f.front())) f.erase(f.begin());
        while (!f.empty() && isspace((unsigned char)f.back())) f.pop_back();
    }
    return out;
}

}  // namespace

// --- Face -----------------------------------------------------------------

Face::~Face() {
    if (face_) FT_Done_Face(face_);
}

bool Face::open(FT_Library ft, const std::string& path, int index, float px) {
    if (FT_New_Face(ft, path.c_str(), index, &face_) != 0) return false;
    size_ = px;

    // Кольорові емодзі — це растри фіксованих розмірів; масштабованих розмірів
    // у такому шрифті немає взагалі, і FT_Set_Pixel_Sizes на ньому не працює.
    // Тому беремо найближчий наявний розмір і домовляємося масштабувати гліф
    // самі (bitmap_scale_).
    if (!FT_IS_SCALABLE(face_) && face_->num_fixed_sizes > 0) {
        int best = 0;
        int bestd = 1 << 30;
        for (int i = 0; i < face_->num_fixed_sizes; ++i) {
            const int h = face_->available_sizes[i].height;
            const int d = h > (int)px ? h - (int)px : (int)px - h;
            if (d < bestd) { bestd = d; best = i; }
        }
        if (FT_Select_Size(face_, best) != 0) {
            FT_Done_Face(face_);
            face_ = nullptr;
            return false;
        }
        const float have = (float)face_->available_sizes[best].height;
        bitmap_scale_ = have > 0 ? px / have : 1.0f;
        // Метрики беремо вже масштабовані — інакше рядок з емодзі роз'їхався б.
        ascent_ = px * 0.8f;
        descent_ = px * 0.2f;
        line_gap_ = 0;
        x_height_ = px * 0.5f;
        ch_width_ = px;
        return true;
    }

    if (FT_Set_Pixel_Sizes(face_, 0, (FT_UInt)(px + 0.5f)) != 0) {
        FT_Done_Face(face_);
        face_ = nullptr;
        return false;
    }
    ascent_ = face_->size->metrics.ascender / 64.0f;
    descent_ = -face_->size->metrics.descender / 64.0f;
    const float line = face_->size->metrics.height / 64.0f;
    line_gap_ = line - ascent_ - descent_;
    if (line_gap_ < 0) line_gap_ = 0;

    // x-height і ширина «0» — литехtml просить їх для одиниць ex і ch.
    x_height_ = px * 0.5f;
    if (FT_Load_Char(face_, 'x', FT_LOAD_DEFAULT) == 0)
        x_height_ = face_->glyph->metrics.height / 64.0f;
    ch_width_ = px * 0.5f;
    if (FT_Load_Char(face_, '0', FT_LOAD_DEFAULT) == 0)
        ch_width_ = face_->glyph->advance.x / 64.0f;
    return true;
}

bool Face::has(unsigned cp) const {
    return face_ && FT_Get_Char_Index(face_, cp) != 0;
}

const Glyph* Face::glyph(unsigned cp) {
    auto it = cache_.find(cp);
    if (it != cache_.end()) return &it->second;
    if (!face_) return nullptr;

    // FT_LOAD_COLOR — і є та причина, заради якої тут FreeType: без нього
    // кольорова емодзі приїхала б порожньою альфою.
    if (FT_Load_Char(face_, cp, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0) return nullptr;
    FT_GlyphSlot g = face_->glyph;

    Glyph out;
    out.advance = g->advance.x / 64.0f * bitmap_scale_;
    out.left = (int)lrintf(g->bitmap_left * bitmap_scale_);
    out.top = (int)lrintf(g->bitmap_top * bitmap_scale_);

    const FT_Bitmap& b = g->bitmap;
    if (b.width == 0 || b.rows == 0) {
        cache_[cp] = out;                 // пробіл: ширина є, растру немає
        return &cache_[cp];
    }

    if (b.pixel_mode == FT_PIXEL_MODE_BGRA) {
        out.color = true;
        // FreeType віддає BGRA вже помножену на альфу — саме те, що треба.
        const int sw = (int)b.width, sh = (int)b.rows;
        const int dw = bitmap_scale_ == 1.0f ? sw : (int)lrintf(sw * bitmap_scale_);
        const int dh = bitmap_scale_ == 1.0f ? sh : (int)lrintf(sh * bitmap_scale_);
        out.width = dw > 0 ? dw : 1;
        out.height = dh > 0 ? dh : 1;
        out.bits.assign((size_t)out.width * out.height * 4, 0);
        // Найближчий сусід: емодзі й так растрова, а різниця між нею та
        // згладженою на 20 пікселях не варта окремого масштабувальника.
        for (int y = 0; y < out.height; ++y) {
            const int sy = out.height == sh ? y : y * sh / out.height;
            const uint8_t* s = b.buffer + (size_t)sy * b.pitch;
            uint8_t* d = out.bits.data() + (size_t)y * out.width * 4;
            for (int x = 0; x < out.width; ++x) {
                const int sx = out.width == sw ? x : x * sw / out.width;
                memcpy(d + (size_t)x * 4, s + (size_t)sx * 4, 4);
            }
        }
    } else if (b.pixel_mode == FT_PIXEL_MODE_GRAY) {
        out.width = (int)b.width;
        out.height = (int)b.rows;
        out.bits.assign((size_t)out.width * out.height, 0);
        for (int y = 0; y < out.height; ++y)
            memcpy(out.bits.data() + (size_t)y * out.width,
                   b.buffer + (size_t)y * b.pitch, (size_t)out.width);
    } else if (b.pixel_mode == FT_PIXEL_MODE_MONO) {
        // Однобітний растр (старі шрифти) — розгортаємо в альфу.
        out.width = (int)b.width;
        out.height = (int)b.rows;
        out.bits.assign((size_t)out.width * out.height, 0);
        for (int y = 0; y < out.height; ++y) {
            const uint8_t* s = b.buffer + (size_t)y * b.pitch;
            uint8_t* d = out.bits.data() + (size_t)y * out.width;
            for (int x = 0; x < out.width; ++x)
                d[x] = (s[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
        }
    } else {
        return nullptr;
    }

    cache_[cp] = std::move(out);
    return &cache_[cp];
}

// --- FontStore ------------------------------------------------------------

FontStore::FontStore() {
    if (FT_Init_FreeType(&ft_) != 0) ft_ = nullptr;
    FcInit();
}

FontStore::~FontStore() {
    for (auto& kv : faces_) delete kv.second;
    if (ft_) FT_Done_FreeType(ft_);
}

bool FontStore::find_file(const std::string& family, int weight, bool italic,
                          std::string* path, int* index, bool* exact) {
    FcPattern* pat = FcPatternCreate();
    if (!pat) return false;
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)family.c_str());
    FcPatternAddInteger(pat, FC_WEIGHT, fc_weight(weight));
    FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult res = FcResultNoMatch;
    FcPattern* got = FcFontMatch(nullptr, pat, &res);
    FcPatternDestroy(pat);
    if (!got) return false;

    FcChar8* file = nullptr;
    int idx = 0;
    bool ok = false;
    if (FcPatternGetString(got, FC_FILE, 0, &file) == FcResultMatch && file) {
        FcPatternGetInteger(got, FC_INDEX, 0, &idx);
        *path = (const char*)file;
        *index = idx;
        ok = true;
    }
    // Чи це САМЕ та родина, яку просили. FcFontMatch не вміє «не знайшлося»:
    // він завжди віддає найкраще, що є, і «Segoe UI» тихо стає DejaVu Sans.
    // Через це рядки виходили на 19% ширші, ніж на Windows, і переносилися
    // раніше. Тому питаємо ще й що саме нам дали.
    if (exact) {
        *exact = false;
        FcChar8* got_fam = nullptr;
        for (int i = 0; FcPatternGetString(got, FC_FAMILY, i, &got_fam) == FcResultMatch; ++i) {
            if (!got_fam) continue;
            std::string a = (const char*)got_fam, b = family;
            for (char& ch : a) ch = (char)tolower((unsigned char)ch);
            for (char& ch : b) ch = (char)tolower((unsigned char)ch);
            if (a == b) { *exact = true; break; }
        }
    }
    FcPatternDestroy(got);
    return ok;
}

bool FontStore::find_cover(unsigned cp, int weight, bool italic,
                           std::string* path, int* index) {
    FcCharSet* cs = FcCharSetCreate();
    if (!cs) return false;
    FcCharSetAddChar(cs, cp);

    FcPattern* pat = FcPatternCreate();
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcPatternAddInteger(pat, FC_WEIGHT, fc_weight(weight));
    FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    // Кольорові шрифти не відкидаємо: саме вони й потрібні для емодзі.
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult res = FcResultNoMatch;
    FcFontSet* set = FcFontSort(nullptr, pat, FcTrue, nullptr, &res);
    bool ok = false;
    if (set) {
        // Два заходи, і порядок тут важливий. fontconfig упорядковує за своєю
        // мірою схожості, і для смайла він цілком може поставити першим
        // ЧОРНО-БІЛИЙ символьний шрифт — цей символ є і в ньому. Тому спершу
        // шукаємо серед КОЛЬОРОВИХ (ознака — FC_COLOR), і лише якщо жоден не
        // підійшов, беремо будь-який. Кольорових шрифтів поза емодзі майже не
        // буває, тож ієрогліфам це не шкодить.
        for (int pass = 0; pass < 2 && !ok; ++pass) {
            for (int i = 0; i < set->nfont && !ok; ++i) {
                FcCharSet* have = nullptr;
                if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &have) != FcResultMatch)
                    continue;
                if (!FcCharSetHasChar(have, cp)) continue;
                if (pass == 0) {
                    FcBool color = FcFalse;
                    if (FcPatternGetBool(set->fonts[i], FC_COLOR, 0, &color) != FcResultMatch ||
                        !color)
                        continue;
                }
                FcChar8* file = nullptr;
                int idx = 0;
                if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) == FcResultMatch && file) {
                    FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &idx);
                    *path = (const char*)file;
                    *index = idx;
                    ok = true;
                }
            }
        }
        FcFontSetDestroy(set);
    }
    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);
    return ok;
}

Face* FontStore::open(const std::string& path, int index, float px) {
    Key k{path, index, (int)lrintf(px * 10.0f)};
    auto it = faces_.find(k);
    if (it != faces_.end()) return it->second;

    Face* f = new Face();
    if (!f->open(ft_, path, index, px)) {
        delete f;
        faces_[k] = nullptr;             // щоб не пробувати те саме щоразу
        return nullptr;
    }
    faces_[k] = f;
    return f;
}

// Чим заміняти шрифти, яких поза Windows немає.
//
// Порядок не випадковий, він ЗАМІРЯНИЙ на тих самих 29 зразках: полотно
// виходить 1520 px на Noto Sans, 1547 на Liberation і 1682 на DejaVu — проти
// 1412 на Windows. Тобто типовий для Debian DejaVu дає +19% висоти просто
// тому, що ширший: рядки переносяться раніше. Noto лишає +7.6%, і це вже
// різниця самих накреслень, а не помилка підбору.
static const char* kSubstitutes[] = {"Noto Sans", "Liberation Sans", "DejaVu Sans"};

Face* FontStore::face(const std::string& families, int weight, bool italic, float px) {
    if (!ft_) return nullptr;

    // Перший захід — лише ТОЧНІ збіги: якщо в системі справді є названий у CSS
    // шрифт, беремо його й нічого не вигадуємо.
    std::string first_path;
    int first_index = 0;
    bool have_any = false;
    for (const std::string& fam : family_list(families)) {
        if (fam.empty()) continue;
        const std::string key = fam + "|" + std::to_string(weight) + (italic ? "|i" : "|r");
        auto it = files_.find(key);
        if (it == files_.end()) {
            std::string path;
            int index = 0;
            bool exact = false;
            if (!find_file(fam, weight, italic, &path, &index, &exact)) {
                files_[key] = {"", 0};
                continue;
            }
            // Неточний збіг запам'ятовуємо окремо: він знадобиться лише як
            // останній порятунок.
            if (!have_any) { first_path = path; first_index = index; have_any = true; }
            if (!exact) { files_[key] = {"", 0}; continue; }
            it = files_.insert({key, {path, index}}).first;
        }
        if (it->second.first.empty()) continue;
        Face* f = open(it->second.first, it->second.second, px);
        if (f) return f;
    }

    // Точного немає — беремо заміну зі списку вище.
    for (const char* sub : kSubstitutes) {
        std::string path;
        int index = 0;
        bool exact = false;
        if (find_file(sub, weight, italic, &path, &index, &exact) && exact) {
            Face* f = open(path, index, px);
            if (f) return f;
        }
    }

    // І нарешті — те, що fontconfig вважав найкращим для першої родини.
    if (have_any) {
        Face* f = open(first_path, first_index, px);
        if (f) return f;
    }
    std::string path;
    int index = 0;
    if (find_file("sans-serif", weight, italic, &path, &index, nullptr))
        return open(path, index, px);
    return nullptr;
}

Face* FontStore::fallback(unsigned cp, int weight, bool italic, float px) {
    if (!ft_) return nullptr;
    // Ключ — початок блоку по 128 символів: сусідні символи майже завжди
    // живуть в одному шрифті, а запит до fontconfig не безкоштовний.
    const unsigned block = cp & ~0x7Fu;
    auto it = covers_.find(block);
    if (it == covers_.end()) {
        std::string path;
        int index = 0;
        if (!find_cover(cp, weight, italic, &path, &index)) {
            covers_[block] = {"", 0};
            return nullptr;
        }
        it = covers_.insert({block, {path, index}}).first;
    }
    if (it->second.first.empty()) return nullptr;
    Face* f = open(it->second.first, it->second.second, px);
    // Шрифт із блоку міг не мати саме цього символу — тоді питаємо точно.
    if (f && !f->has(cp)) {
        std::string path;
        int index = 0;
        if (find_cover(cp, weight, italic, &path, &index)) return open(path, index, px);
    }
    return f;
}

}  // namespace hominka
