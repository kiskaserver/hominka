// Шрифти поза Windows: пошук файлу (fontconfig) і растеризація гліфів (FreeType).
//
// Навіщо цей файл узагалі існує. Під Windows усе це робить DirectWrite сам:
// назвав родину — отримав накреслення, трапився ієрогліф чи емодзі — він тихо
// підставив інший шрифт, і намалював кольорову емодзі теж сам. Поза Windows
// такої однієї речі немає, тож збираємо з двох:
//
//   fontconfig — «дай файл шрифту для родини Segoe UI, жирного, курсивом» і,
//                що важливіше, «дай будь-який шрифт, у якому є ОЦЕЙ символ».
//                Друге — це і є підбір запасного шрифту.
//   FreeType   — «намалюй гліф» — і для звичайних контурів, і для КОЛЬОРОВИХ
//                РАСТРОВИХ емодзі (CBDT), яких у чаті багато. Саме заради
//                другого тут FreeType, а не власний растеризатор Blend2D:
//                кольорових шрифтів той не вміє.
//
// Чого тут свідомо немає: складання (shaping) і двонапрямленого письма. Чат —
// це латиниця, кирилиця, емодзі й іноді CJK; для них достатньо йти символ за
// символом і додавати ширину. Арабська й гінді виглядатимуть неправильно —
// це відомо й записано, а не забуто.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hominka {

// Один растеризований гліф.
struct Glyph {
    int width = 0, height = 0;      // розмір растру
    int left = 0, top = 0;          // зсув від точки вставки (top — вгору)
    float advance = 0;              // на скільки посунутися далі
    bool color = false;             // true — растр уже BGRA (кольорова емодзі)
    std::vector<uint8_t> bits;      // color ? BGRA premultiplied : альфа A8
};

// Накреслення потрібного кегля: файл + розмір.
class Face {
public:
    ~Face();
    bool open(FT_Library ft, const std::string& path, int index, float px);

    // Чи є в цьому шрифті такий символ. За цим вирішуємо, чи шукати запасний.
    bool has(unsigned cp) const;
    // Растеризований гліф (кешується: у чаті ті самі літери йдуть тисячами).
    const Glyph* glyph(unsigned cp);

    float ascent() const { return ascent_; }
    float descent() const { return descent_; }
    float line_gap() const { return line_gap_; }
    float x_height() const { return x_height_; }
    float ch_width() const { return ch_width_; }
    float size() const { return size_; }

private:
    FT_Face face_ = nullptr;
    float size_ = 0;
    float ascent_ = 0, descent_ = 0, line_gap_ = 0, x_height_ = 0, ch_width_ = 0;
    // Кольорові растрові шрифти мають фіксовані розміри: якщо потрібного немає,
    // беремо найближчий і масштабуємо гліф самі.
    float bitmap_scale_ = 1.0f;
    std::map<unsigned, Glyph> cache_;
};

// Усі шрифти програми. Один на процес: і FreeType, і fontconfig дорого
// піднімати, а файли шрифтів у чаті повторюються.
class FontStore {
public:
    FontStore();
    ~FontStore();
    bool ok() const { return ft_ != nullptr; }

    // Накреслення для родини («Segoe UI», «sans-serif»), ваги й курсиву.
    // Родини перебираємо в тому ж порядку, що написано в CSS.
    Face* face(const std::string& families, int weight, bool italic, float px);

    // Запасне накреслення, у якому є цей символ. nullptr — не знайшлося ніде.
    Face* fallback(unsigned cp, int weight, bool italic, float px);

private:
    struct Key {
        std::string path;
        int index;
        int px10;                   // кегль з точністю до 0.1 px
        bool operator<(const Key& o) const {
            if (path != o.path) return path < o.path;
            if (index != o.index) return index < o.index;
            return px10 < o.px10;
        }
    };

    Face* open(const std::string& path, int index, float px);
    // «Segoe UI» на Linux немає — питаємо fontconfig, і той дає щось розумне.
    // exact каже, чи це САМЕ та родина, яку просили: fontconfig не вміє
    // відповідати «немає», він завжди дає найкраще з наявного.
    bool find_file(const std::string& family, int weight, bool italic,
                   std::string* path, int* index, bool* exact = nullptr);
    bool find_cover(unsigned cp, int weight, bool italic,
                    std::string* path, int* index);

    FT_Library ft_ = nullptr;
    std::map<Key, Face*> faces_;
    // «родина+вага+курсив» → файл. Запит до fontconfig не безкоштовний, а
    // питаємо ми те саме на кожен рядок чату.
    std::map<std::string, std::pair<std::string, int>> files_;
    // Символ → шрифт, у якому він знайшовся. Ключ — не сам символ, а його
    // блок: сусідні символи майже завжди живуть в одному шрифті.
    std::map<unsigned, std::pair<std::string, int>> covers_;
};

}  // namespace hominka
