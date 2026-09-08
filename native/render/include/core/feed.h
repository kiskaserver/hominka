// Стрічка чату: список повідомлень, кеш намальованих рядків, складання кадру.
//
// Головна ідея. Рядок, який уже намальовано, більше не міняється — ні від
// нового повідомлення, ні від перемальовування. Тож кожне повідомлення
// розкладається й растеризується РІВНО ОДИН РАЗ, у власну картинку, а кадр
// стрічки — це вже просто накладання готових картинок одна над одною знизу
// вгору. У браузері так було не можна (документ один на всіх), і саме тому
// там кожне нове повідомлення коштувало перерахунку всієї сторінки.
//
// Наслідки, заради яких усе й робилося:
//   * прийшло повідомлення — робота пропорційна ОДНОМУ рядку, а не стрічці;
//   * нічого не змінилося — кадр не перемальовується взагалі, і відеокарта
//     стоїть без діла;
//   * поява рядка малюється нативно (зсув і прозорість), тож @keyframes, якого
//     litehtml не знає, не потрібен.
//
// Два кроки на рядок, і вони роздільні НАВМИСНО:
//   1) розкладка (litehtml) — не потребує цілі малювання, дає висоту рядка;
//   2) растеризація — потребує цілі, бо картинка живе на тому ж пристрої.
// Через це висоту стрічки можна дізнатися ДО того, як зʼявиться поверхня
// потрібного розміру (саме це й робить самоперевірка), а зміна цілі
// (переростання свопчейна) викидає лише картинки, лишаючи розкладку.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/chat_doc.h"
#include "core/feedgfx.h"
#include "gfx/container.h"
#include "gfx/imgcache.h"
#include "litehtml.h"

namespace hominka {

// Стільки рядків тримаємо. Те саме число, що й на сторінці (MAX = 80): вище
// краю вікна їх однаково не видно, а памʼять і час вони їдять.
static const size_t FEED_MAX_MESSAGES = 80;

// Скільки триває поява рядка. 180 мс — рівно як в анімації на сторінці.
static const int64_t FEED_ENTER_MS = 180;

class Feed {
public:
    Feed(GfxFonts* fonts, ImageCache* images);
    ~Feed();

    // --- налаштування (кожне скидає те, що вже пораховано) ---
    void set_css(const std::string& css);
    void set_layout(const std::vector<std::string>& layout);
    void set_zoom(float zoom);
    void set_width(int width);

    int width() const { return width_; }
    int inset() const { return inset_; }

    // Прозорість усієї стрічки: 1 — як намальовано, 0.5 — напівпрозоро.
    //
    // Саме цього чекають від «прозорості вікна»: щоб крізь чат було видно те,
    // що під ним. Доки це значення множилося лише в підкладку, у режимі без
    // підкладки повзунок не робив рівно нічого.
    void set_alpha(float a) { alpha_ = a < 0.05f ? 0.05f : (a > 1.0f ? 1.0f : a); }

    // Скільки пікселів згори лишити вільними під смужку керування. Без цього
    // смужка накривала найстаріший видимий рядок — вона ж напівпрозора і
    // з'являється поверх уже намальованого.
    void set_top_pad(int px) { top_pad_ = px < 0 ? 0 : px; }

    // --- вміст ---
    void add(const ChatMessage& m, int64_t now_ms);
    void remove_id(const std::string& id);
    void purge_nick(const std::string& nick);
    void clear();
    size_t size() const { return items_.size(); }

    // Картинка приїхала — рядки, які її чекали, треба перекласти наново.
    void on_image_arrived(const std::string& url);

    // Адреси картинок, яких бракує: програма просить Python докачати саме їх.
    std::vector<std::string> take_missing();

    // Розкладає все, що ще не розкладено, і повертає висоту вмісту стрічки.
    // Цілі малювання не потребує.
    int content_height();

    // Чи змінилася б картинка стрічки, якби ми малювали зараз. Поки false —
    // Present робити НЕ треба (це і є «відеокарта стоїть без діла»).
    bool dirty(int64_t now_ms) const;

    // Анімовані емоути можна вимкнути зовсім — тоді від кожного лишається
    // перший кадр, і стрічка знову засинає намертво. Знадобиться і слабкій
    // машині, і самоперевірці, якій потрібен однаковий PNG щоразу.
    void set_animate(bool on);
    bool animate() const { return animate_; }

    // Адреси анімованих емоутів, які зараз тримають рядки стрічки. За цим
    // списком кеш картинок вирішує, кого можна викинути.
    std::set<std::string> animated_in_use() const;

    // Забути кадрові текстури викинутої картинки. Кличеться, коли кеш
    // картинок справді її позбувся, — інакше тут лишилися б посилання на
    // те, чого вже немає.
    void forget_image(const std::string& url);

    // Викинути кадрові текстури емоутів, яких зараз не видно.
    //
    // Це друга половина тієї самої межі. Розібрані кадри лежать у памʼяті, але
    // КОЖЕН намальований кадр робить іще й текстуру на відеокарті: сорок
    // емоутів по двадцять чотири кадри — це тисяча текстур. Анімується ж лише
    // те, що в кадрі, тож решту тримати нема сенсу.
    void trim_anim();

    // Малює стрічку в задану ціль. Рядки притиснуті до низу — те саме, що
    // «justify-content: flex-end» на сторінці.
    bool draw(GfxTarget* rt, int view_w, int view_h, int64_t now_ms);

private:
    struct Item {
        ChatMessage msg;
        int64_t added_ms = 0;
        // Крок 1: розкладка. Документ тримаємо, щоб не складати його вдруге
        // під час растеризації.
        litehtml::document::ptr doc;
        int content = 0;                 // висота самого рядка
        int pad_top = 0, pad_bottom = 0; // запас під тінь
        // Крок 2: картинка на пристрої цілі.
        GfxSurface* surface = nullptr;
        GfxRaster* bitmap = nullptr;
        // Рядок пішов вище краю вікна — його не малюють і малювати не будуть,
        // доки стрічка не зміниться. Без цієї позначки він назавжди лишався б
        // «ще не намальованим», а отже стрічка — назавжди «брудною», і кадр
        // перемальовувався б 60 разів на секунду ні для чого.
        bool offscreen = false;
        // Місця анімованих емоутів у цьому рядку. У картинці рядка на їх
        // місці дірка — кадри кладе draw() поверх, щоразу свій.
        std::vector<Sprite> sprites;
        int height() const { return content + pad_top + pad_bottom; }
        bool laid() const { return doc != nullptr && content > 0; }
        bool ready() const { return bitmap != nullptr; }
    };

    // Текстура одного кадру анімації. Живе на пристрої цілі — як і картинки
    // рядків, тож викидається разом з ними при зміні цілі.
    struct AnimKey {
        std::string url;
        int frame = 0;
        bool operator<(const AnimKey& o) const {
            return url == o.url ? frame < o.frame : url < o.url;
        }
    };

    // Кадр емоута як текстура. Робиться на вимогу: якщо емоут на 60 кадрів
    // видно лише мить, решта 59 текстур ніколи й не зʼявиться.
    GfxRaster* anim_bitmap(const std::string& url, int frame, GfxTarget* rt);

    void drop_layout();                  // стилі/кегль/ширина змінилися
    void drop_bitmaps();                 // змінилася ціль малювання
    static void release_bitmap(Item* it);
    bool ensure_layout(Item* it);
    bool ensure_bitmap(Item* it, GfxTarget* rt);
    void enter_state(const Item& it, int64_t now_ms, float* alpha, float* dy) const;

    GfxFonts* fonts_ = nullptr;               // не володіємо
    ImageCache* images_ = nullptr;            // не володіємо
    Container container_;
    TextShadow shadow_;

    std::deque<Item> items_;
    std::string css_;
    std::vector<std::string> layout_;
    float zoom_ = 1.0f;
    int width_ = 430;
    int inset_ = 8;
    int top_pad_ = 0;
    float alpha_ = 1.0f;
    int gap_ = 6;
    // «#list { flex-direction: column-reverse }» — новіші рядки згори.
    bool reversed_ = false;
    // «.m { animation: none }» — не анімувати появу рядка.
    bool enter_anim_ = true;
    std::vector<std::string> missing_;
    GfxTarget* owner_ = nullptr;

    bool animate_ = true;
    std::map<AnimKey, GfxRaster*> anim_;
    // Коли найближчий видимий емоут перемкне кадр. Поки цей момент не настав,
    // стрічка вважається незмінною — саме звідси беруться «нуль кадрів у
    // спокої» навіть тоді, коли в чаті крутиться десяток анімацій.
    mutable int64_t next_anim_ms_ = 0;
};

}  // namespace hominka
