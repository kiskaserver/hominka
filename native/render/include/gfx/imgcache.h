// Картинки чату: емоути, значки площадок, іконки плашок.
//
// Звідки вони беруться. Мережі тут немає навмисно — качає Python
// (hominka/thirdparty.py, hominka/badges.py), який уже вміє в 7TV/BTTV/FFZ,
// кеші й TTL. Сюди приходять готові БАЙТИ разом з URL, під яким їх запам'ятати.
// Виняток — «data:»-адреси: вони самодостатні, їх розбираємо на місці (значки
// площадок і плашки Kick вбудовані прямо в розмітку).
//
// Чому три декодери:
//   WIC      — PNG, JPEG, GIF, BMP. Є в кожній Windows, тягнути нічого не треба.
//   stb_image— те саме поза Windows, де WIC немає. Один заголовок, жодної
//              бібліотеки; анімований GIF він до того ж віддає ВЖЕ СКЛАДЕНИМ
//              покадрово, тобто робить те, що під Windows довелося писати
//              руками (латки кадрів, зсуви, спосіб затирання).
//   libwebp  — 7TV і BTTV віддають емоути у WebP, а ні WIC, ні stb його не знають.
//   nanosvg  — значки площадок і плашки Kick — це SVG; його теж ніхто з них не знає.
//
// Формат на виході один: premultiplied BGRA, рядок = width*4. Саме такий чекає
// Direct2D (D2D1_ALPHA_MODE_PREMULTIPLIED) — інакше напівпрозорі краї емоутів
// світилися б чорним обідком.
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>       // IWICImagingFactory: оголошення потрібне тут, бо
                            // попереднє оголошення всередині namespace hominka
                            // створило б ІНШИЙ тип, не COM-інтерфейс
#endif
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hominka {

// Один кадр картинки. Для нерухомої — єдиний; для анімованої (GIF/WebP) їх
// кілька, і delay_ms каже, скільки тримати цей.
struct ImageFrame {
    int width = 0;
    int height = 0;
    int delay_ms = 0;
    std::vector<uint8_t> bgra;      // premultiplied, stride = width*4
};

struct Image {
    // Природний розмір — той, який картинка «має» з погляду CSS. Для PNG/WebP
    // це її пікселі, а для SVG — розмір із самого документа (width/height).
    //
    // Чому окремо від розміру растру: SVG ми навмисно растеризуємо більшим за
    // природний розмір (щоб не мазало при великому кеглі), але CSS мусить
    // бачити саме природний. Інакше «.em { height:1.5em; width:auto }» бере
    // ширину з растру — і емоут роздувається на пів рядка.
    int nat_width = 0;
    int nat_height = 0;
    int width = 0;                  // розмір растру (усі кадри однакові)
    int height = 0;
    std::vector<ImageFrame> frames;
    // Картинку розібрали, але намалювати її як слід ми не можемо: SVG із
    // <text>. Шрифтового рушія в nanosvg немає, і напис просто зникає, лишаючи
    // порожню плитку. Краще чесно сказати «не вмію» — тоді замість картинки
    // стане текстова плашка.
    //
    // Наші власні значки цього більше не потребують (усі вони — самі лише
    // <path>, див. hominka/make_badge_og.py), але страхування лишається: SVG
    // може приїхати й ззовні.
    bool unsupported = false;

    // Скільки триває повний оберт анімації і скільки памʼяті займають розібрані
    // кадри. Обидва рахуємо один раз при розборі: вибір кадру трапляється
    // щокадру, а ділення там ні до чого.
    int total_ms = 0;
    size_t bytes = 0;
    // Коли картинку востаннє питали. За цим вирішуємо, кого викидати першим
    // (див. ImageCache::gc_animated).
    int64_t used = 0;

    bool ok() const { return width > 0 && height > 0 && !frames.empty() && !unsupported; }
    bool animated() const { return frames.size() > 1 && total_ms > 0; }

    // Номер кадру на момент t (мс від довільної точки). Анімація йде по колу.
    int frame_at(int64_t t_ms) const;
    // Скільки мілісекунд лишилося до наступної зміни кадру. Саме заради цього
    // числа все й затіяно: кадр стрічки перемальовується не «кожні 16 мс», а
    // рівно тоді, коли емоут справді змінює кадр.
    int ms_to_next(int64_t t_ms) const;
};

// Скільки анімованих емоутів тримаємо розібраними і скільки памʼяті їм
// дозволено. Нерухомі під ці межі не підпадають: значок 28×28 — це 3 КБ, і
// тисяча таких не помітна. А от анімований емоут 128×128 на 30 кадрів — це
// 1.9 МБ, і саме з них набігає та сама «сотня мегабайтів».
//
// 64 штуки — з добрим запасом: різних анімованих емоутів у полі зору буває
// одиниці, бо в стрічці повторюється те саме.
static const size_t ANIM_MAX_COUNT = 64;
static const size_t ANIM_MAX_BYTES = 64u * 1024 * 1024;
// І окремо — стеля на ОДНУ картинку. Емоут-монстр (велике полотно на багато
// кадрів) здатен сам важити більше за всю межу, і тоді жодне вигнання не
// допоможе: викидати його не можна, доки він на екрані. Такий лишаємо
// нерухомим — статичний емоут краще, ніж 60 МБ памʼяті на один значок.
static const size_t ANIM_MAX_IMAGE_BYTES = 8u * 1024 * 1024;

// Сховище «URL → картинка». Живе стільки ж, скільки процес: емоути того самого
// каналу повторюються в кожному другому повідомленні, і декодувати їх щоразу —
// марна робота.
class ImageCache {
public:
    ImageCache();
    ~ImageCache();

    // Покласти байти, які прислав Python. Декодуємо одразу: краще витратити
    // мілісекунду тут, ніж під час розкладки повідомлення.
    bool put(const std::string& url, const uint8_t* data, size_t len);

    // Знайти. Повертає nullptr, якщо картинки ще немає — це нормальний стан:
    // емоут міг не встигнути докачатися. Виклик із «data:»-адресою декодує її
    // на місці й запам'ятовує.
    const Image* get(const std::string& url);

    // Чи знаємо ми вже про цю адресу (хай навіть декодування провалилося).
    // Потрібно, щоб не питати в Python те саме двічі.
    bool known(const std::string& url) const;

    void clear();
    size_t size() const { return items_.size(); }

    // Викинути анімовані картинки, які зараз ніде не потрібні, доки не влізуть
    // у межі. Найдавніше питані йдуть першими.
    //
    // in_use — адреси, на які ще посилаються рядки стрічки. Їх не викидаємо
    // НІКОЛИ, хоч би скільки їх було: емоут, що зник просто під час показу,
    // гірший за перевищену на пару мегабайтів межу. Рядків усього 80, тож
    // множина ця й так обмежена.
    // Повертає, скільки викинуло: за цим числом видно в журналі, що межа
    // справді працює, а не просто оголошена в заголовку.
    size_t gc_animated(const std::set<std::string>& in_use,
                       size_t max_count = ANIM_MAX_COUNT,
                       size_t max_bytes = ANIM_MAX_BYTES);

    // Кого викинули. Потрібно тому, хто тримає похідні ресурси (текстури
    // кадрів на відеокарті): інакше в нього лишилися б посилання на вже
    // неіснуючу картинку.
    using EvictHook = std::function<void(const std::string& url)>;
    void set_evict_hook(EvictHook h) { evict_ = std::move(h); }

    size_t animated_count() const;
    size_t animated_bytes() const;

private:
    // Розбирає «data:[<тип>][;base64],<дані>».
    static bool decode_data_url(const std::string& url, std::vector<uint8_t>* out,
                                std::string* mime);

    bool decode(const uint8_t* data, size_t len, Image* out);
#ifdef _WIN32
    bool decode_wic(const uint8_t* data, size_t len, Image* out);
#else
    bool decode_stb(const uint8_t* data, size_t len, Image* out);
#endif
    bool decode_webp(const uint8_t* data, size_t len, Image* out);
    bool decode_svg(const uint8_t* data, size_t len, Image* out);

    // Дорахувати те, що залежить від кадрів: тривалість оберту й вагу.
    static void finish(Image* img);

#ifdef _WIN32
    IWICImagingFactory* wic_ = nullptr;
#endif
    std::map<std::string, Image> items_;
    EvictHook evict_;
    int64_t tick_ = 0;              // лічильник звернень, він же «час» для LRU
};

}  // namespace hominka
