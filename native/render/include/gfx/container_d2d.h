// Малювання для litehtml через Direct2D + DirectWrite.
//
// litehtml сам нічого не малює: він рахує розкладку й каже «намалюй цей текст
// таким шрифтом отут», «залий цей прямокутник», «постав цю картинку». Усе це
// й робить цей клас.
//
// Чому Direct2D, а не своя растеризація: текст. Чат — це кирилиця, латиниця,
// емодзі й іноді CJK упереміш, з переносами й підбором шрифту під символ.
// DirectWrite усе це вже вміє (і кольорові емодзі теж), він є в кожній
// Windows, і малює прямо в ту саму текстуру D3D11, яку показує
// DirectComposition — тобто без жодного зайвого копіювання.
//
// Одне свідоме доповнення до litehtml: text-shadow. У базових стилях чату він
// є (рядок читається поверх будь-якої гри саме завдяки йому), а litehtml
// такої властивості не знає. Тому її розбираємо самі (parse_text_shadow) і
// застосовуємо тут, у draw_text.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "litehtml.h"
#include "gfx/cssbits.h"
#include "gfx/imgcache.h"

namespace hominka {

// Тіні, кольори, перетворення й переноси слів розбирає cssbits — спільно для
// Windows і Linux. Тут лишається саме малювання.

class ContainerD2D : public litehtml::document_container {
public:
    ContainerD2D(IDWriteFactory* dw, ImageCache* images);
    ~ContainerD2D() override;

    // Куди малювати. Ставиться перед document::draw і на час малювання не
    // міняється. Розмір — область, у якій litehtml рахує відсотки й viewport.
    void begin(ID2D1RenderTarget* rt, int width, int height);
    void end();

    // Тінь, якою користуємось, коли стиль її не задав (чужа тема могла не
    // писати text-shadow узагалі). Зазвичай це тінь із правила body.
    void set_text_shadow(const TextShadow& ts) { shadow_ = ts; }
    // Кегль базового шрифту (font-size у body). Змінюється повзунком «A+/A−»
    // — це той самий zoom, що раніше йшов у QWebEngineView.setZoomFactor.
    void set_base_font_size(float px) { base_font_px_ = px; }
    // Адреси, яких бракує: litehtml попросив картинку, а її ще немає. Програма
    // забирає цей список і просить Python докачати.
    std::vector<std::string> take_missing();

    // Режим запису спрайтів. Увімкнений — анімований емоут НЕ малюється, а
    // потрапляє в список; на його місці в картинці рядка лишається дірка.
    //
    // Чому саме так, а не «запекти перший кадр і малювати наступні поверх»:
    // емоути напівпрозорі, і крізь наступний кадр просвічував би перший.
    // Дірка ж дає чесне накладання на будь-яке тло.
    //
    // Вимкнений режим (самоперевірка) малює перший кадр як звичайну картинку —
    // так PNG виходить однаковим від запуску до запуску.
    void set_record_sprites(bool on) { record_sprites_ = on; }
    std::vector<Sprite> take_sprites();

    // --- litehtml::document_container -----------------------------------
    litehtml::uint_ptr create_font(const litehtml::font_description& descr,
                                   const litehtml::document* doc,
                                   litehtml::font_metrics* fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont,
                   litehtml::web_color color, const litehtml::position& pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override;
    litehtml::pixel_t get_default_font_size() const override;
    const char* get_default_font_name() const override;
    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override;
    void load_image(const char* src, const char* baseurl, bool redraw_on_ready) override;

    // Забути текстуру за адресою — кличе стрічка, коли кеш картинок викинув
    // або замінив саму картинку.
    void forget_image(const std::string& url);
    void get_image_size(const char* src, const char* baseurl, litehtml::size& sz) override;
    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                    const std::string& url, const std::string& base_url) override;
    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                         const litehtml::web_color& color) override;
    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::linear_gradient& g) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::radial_gradient& g) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                             const litehtml::background_layer::conic_gradient& g) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders,
                      const litehtml::position& draw_pos, bool root) override;
    void set_caption(const char* caption) override {}
    void set_base_url(const char* base_url) override {}
    void link(const std::shared_ptr<litehtml::document>& doc,
              const litehtml::element::ptr& el) override {}
    void on_anchor_click(const char* url, const litehtml::element::ptr& el) override {}
    void on_mouse_event(const litehtml::element::ptr& el, litehtml::mouse_event event) override {}
    void set_cursor(const char* cursor) override {}
    void transform_text(litehtml::string& text, litehtml::text_transform tt) override;
    void import_css(litehtml::string& text, const litehtml::string& url,
                    litehtml::string& baseurl) override;
    void set_clip(const litehtml::position& pos,
                  const litehtml::border_radiuses& bdr_radius) override;
    void del_clip() override;
    void push_opacity(float opacity) override;
    void pop_opacity() override;
    bool push_transform(const litehtml::position& border_box, const std::string& value,
                        litehtml::pixel_t font_size) override;
    void pop_transform() override;
    void draw_box_shadow(litehtml::uint_ptr hdc, const litehtml::position& border_box,
                         const litehtml::border_radiuses& radius,
                         const std::string& value, litehtml::pixel_t font_size) override;
    void get_viewport(litehtml::position& viewport) const override;
    litehtml::element::ptr create_element(const char* tag_name,
                                          const litehtml::string_map& attributes,
                                          const std::shared_ptr<litehtml::document>& doc) override;
    void get_media_features(litehtml::media_features& media) const override;
    void get_language(litehtml::string& language, litehtml::string& culture) const override;
    void split_text(const char* text, const std::function<void(const char*)>& on_word,
                    const std::function<void(const char*)>& on_space) override;

private:
    struct Font {
        IDWriteTextFormat* format = nullptr;
        float size = 0;
        float ascent = 0, descent = 0, line_gap = 0, x_height = 0, ch_width = 0;
        int decoration = 0;                 // підкреслення/закреслення
        // Тінь саме цього елемента — приїхала разом зі стилем шрифту через
        // text-emphasis-style (див. inject_shadow_channel). Через це «.b {
        // text-shadow: none }» і справді вимикає тінь на плашці.
        TextShadow shadow;
    };

    // Обгортка «намалюй текст» — одним місцем і для тіні, і для самого тексту.
    void draw_run(const std::wstring& w, const Font* f, const D2D1_COLOR_F& c,
                  float x, float y_baseline);
    IDWriteTextLayout* layout_for(const std::wstring& w, const Font* f);
    ID2D1SolidColorBrush* brush(const D2D1_COLOR_F& c);
    // Малює картинку в origin_box, обрізаючи по clip_box; повторення враховує.
    void blit(const std::string& url, const litehtml::background_layer& layer,
              const Image* img);
    ID2D1Bitmap* bitmap_for(const std::string& url, const Image* img);

    IDWriteFactory* dw_ = nullptr;          // не володіємо
    ImageCache* images_ = nullptr;          // не володіємо
    ID2D1RenderTarget* rt_ = nullptr;       // не володіємо, дійсний між begin/end
    ID2D1SolidColorBrush* brush_ = nullptr;
    int width_ = 0, height_ = 0;
    int clip_depth_ = 0;
    // Шар прозорості. Direct2D вимагає, щоб PushLayer і PopLayer збігалися за
    // кількістю; тримаємо лічильник і догортаємо його в end(), щоб недомальований
    // кадр не лишив ціль у зіпсованому стані.
    int layer_depth_ = 0;
    ID2D1Layer* layer_ = nullptr;
    // Перетворення вкладаються, тож зберігаємо попереднє й повертаємо його на
    // pop — інакше вихід із вкладеного скинув би й зовнішнє.
    std::vector<D2D1_MATRIX_3X2_F> transforms_;
    float base_font_px_ = 20.0f;
    TextShadow shadow_;
    std::vector<std::string> missing_;
    bool record_sprites_ = false;
    std::vector<Sprite> sprites_;
    std::vector<Font*> fonts_;
    // Кеш «картинка → текстура D2D». Прив'язаний до render target, тому
    // скидається в begin(), якщо target змінився.
    // Ключ — АДРЕСА картинки, а не вказівник на неї.
    //
    // Вказівник тут був пасткою: картинки живуть у std::map всередині кешу, і
    // коли запис звідти викидають (скінчилася межа памʼяті) або замінюють
    // новими байтами, наступна картинка цілком може лягти за тією ж адресою —
    // і рядок дістав би чужу текстуру. Адреса ж унікальна й переживає будь-яке
    // перекладання в кеші.
    std::map<std::string, ID2D1Bitmap*> bitmaps_;
    ID2D1RenderTarget* bitmaps_owner_ = nullptr;
};

}  // namespace hominka
