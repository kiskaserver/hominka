// Малювання для litehtml через Blend2D — Linux-двійник container_d2d.
//
// Розподіл праці тут не такий, як під Windows, і це варто розуміти одразу.
// DirectWrite — це і пошук шрифту, і складання рядка, і растеризація гліфа, і
// кольорові емодзі, усе в одному. Поза Windows такої однієї речі немає, тож:
//
//   Blend2D  — заливки, скруглення, градієнти, рамки, картинки, відсікання,
//              перетворення. Тобто те саме, що Direct2D.
//   FreeType — гліфи, зокрема КОЛЬОРОВІ растрові емодзі (fontstore.h).
//   fontconfig — пошук файлу шрифту й запасного шрифту під символ.
//
// Що з цього виходить інакше, ніж на Windows, і це відомо:
//   * складання (shaping) немає — рядок іде символ за символом. Для латиниці,
//     кирилиці, емодзі й CJK цього досить; арабська й гінді будуть неправильні.
//   * кернінг не застосовується, тож ширина рядка може відрізнятися від
//     віконної на частку відсотка.
//
// Розбір CSS (тіні, кольори, перетворення, переноси) спільний — див. cssbits.
#pragma once

#include <blend2d.h>

#include <functional>
#include <string>
#include <vector>

#include "gfx/cssbits.h"
#include "gfx/fontstore.h"
#include "gfx/imgcache.h"
#include "litehtml.h"

namespace hominka {

class ContainerBL : public litehtml::document_container {
public:
    ContainerBL(FontStore* fonts, ImageCache* images);
    ~ContainerBL() override;

    // Куди малювати. Ставиться перед document::draw і на час малювання не
    // міняється. Розмір — область, у якій litehtml рахує відсотки й viewport.
    void begin(BLContext* ctx, int width, int height);
    void end();

    void set_text_shadow(const TextShadow& ts) { shadow_ = ts; }
    void set_base_font_size(float px) { base_font_px_ = px; }
    std::vector<std::string> take_missing();

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
        Face* face = nullptr;           // не володіємо, живе у FontStore
        std::string families;
        float size = 0;
        int weight = 400;
        bool italic = false;
        int decoration = 0;
        TextShadow shadow;
    };

    // Накреслення, у якому є цей символ: спершу основне, потім запасне.
    Face* face_for(const Font* f, unsigned cp);
    // Малює рядок одним кольором від точки вставки по базовій лінії.
    void draw_run(const std::string& text, const Font* f, const Color& c,
                  float x, float baseline);
    // Картинка в origin_box, обрізана по clip_box; повторення враховує.
    void blit(const litehtml::background_layer& layer, const Image* img);
    // Прямокутник із урахуванням скруглення — одним місцем для заливок і тіней.
    void fill_box(const BLRect& r, float radius, const Color& c);

    FontStore* fonts_ = nullptr;        // не володіємо
    ImageCache* images_ = nullptr;      // не володіємо
    BLContext* ctx_ = nullptr;          // не володіємо, дійсний між begin/end
    int width_ = 0, height_ = 0;
    int clip_depth_ = 0;
    float base_font_px_ = 20.0f;
    TextShadow shadow_;
    std::vector<std::string> missing_;
    bool record_sprites_ = false;
    std::vector<Sprite> sprites_;
    std::vector<Font*> fonts_owned_;

    // Прозорість ГРУПИ. Blend2D має лише глобальну альфу, а вона діє на кожну
    // операцію окремо — там, де діти перекриваються, вийшло б темніше. Тому
    // робимо як Direct2D із шаром: групу малюємо в окреме полотно й КЛАДЕМО
    // його цілком з потрібною альфою.
    struct Layer {
        BLImage image;
        BLContext ctx;
        BLContext* prev = nullptr;
        float alpha = 1.0f;
    };
    std::vector<Layer*> layers_;
    // Перетворення вкладаються, тож зберігаємо попереднє й повертаємо на pop.
    std::vector<BLMatrix2D> transforms_;
};

}  // namespace hominka
