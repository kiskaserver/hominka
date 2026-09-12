#include "gfx/imgcache.h"

#ifdef _WIN32
#include <wincodec.h>
#else
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
// Формати, яких у чаті не буває: кожен — це зайвий код у бінарі й зайва
// поверхня для чужих байтів.
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#endif
#include <algorithm>
#include <cstring>

#include "webp/decode.h"
#include "webp/demux.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

namespace hominka {

namespace {

// Множимо колір на альфу. Direct2D працює з premultiplied, а всі декодери
// віддають straight — без цього кроку напівпрозорі краї емоутів мали б
// темний обідок.
inline void premultiply(uint8_t* bgra, size_t px) {
    for (size_t i = 0; i < px; ++i) {
        uint8_t* p = bgra + i * 4;
        const unsigned a = p[3];
        if (a == 255) continue;
        if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
        p[0] = (uint8_t)((p[0] * a + 127) / 255);
        p[1] = (uint8_t)((p[1] * a + 127) / 255);
        p[2] = (uint8_t)((p[2] * a + 127) / 255);
    }
}

inline void rgba_to_bgra(uint8_t* buf, size_t px) {
    for (size_t i = 0; i < px; ++i) {
        uint8_t* p = buf + i * 4;
        const uint8_t t = p[0]; p[0] = p[2]; p[2] = t;
    }
}

int base64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

void base64_decode(const char* s, size_t n, std::vector<uint8_t>* out) {
    int acc = 0, bits = 0;
    for (size_t i = 0; i < n; ++i) {
        const int v = base64_val(s[i]);
        if (v < 0) continue;                       // пробіли, переноси, '='
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
}

// «%41» та «+» у percent-encoded даних (так приходить незакодований SVG).
void percent_decode(const char* s, size_t n, std::vector<uint8_t>* out) {
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '%' && i + 2 < n) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out->push_back((uint8_t)(hi * 16 + lo)); i += 2; continue; }
        }
        out->push_back((uint8_t)s[i]);
    }
}

bool starts_with(const std::string& s, const char* p) {
    return s.compare(0, strlen(p), p) == 0;
}

bool is_webp(const uint8_t* d, size_t n) {
    return n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4);
}

bool is_svg(const uint8_t* d, size_t n) {
    // SVG — це текст; шукаємо «<svg» у перших кількох сотнях байтів, бо перед
    // ним може бути <?xml …?> і коментарі.
    const size_t lim = n < 512 ? n : 512;
    for (size_t i = 0; i + 4 <= lim; ++i)
        if (!memcmp(d + i, "<svg", 4)) return true;
    return false;
}

}  // namespace

ImageCache::ImageCache() {
#ifdef _WIN32
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&wic_));
#endif
}

ImageCache::~ImageCache() {
#ifdef _WIN32
    if (wic_) wic_->Release();
#endif
}

int Image::frame_at(int64_t t_ms) const {
    if (frames.size() < 2 || total_ms <= 0) return 0;
    // Від'ємний залишок у C++ буває — зводимо в [0, total_ms).
    int64_t at = t_ms % total_ms;
    if (at < 0) at += total_ms;
    int64_t acc = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        acc += frames[i].delay_ms;
        if (at < acc) return (int)i;
    }
    return (int)frames.size() - 1;
}

int Image::ms_to_next(int64_t t_ms) const {
    if (frames.size() < 2 || total_ms <= 0) return 0;
    int64_t at = t_ms % total_ms;
    if (at < 0) at += total_ms;
    int64_t acc = 0;
    for (const ImageFrame& f : frames) {
        acc += f.delay_ms;
        if (at < acc) return (int)(acc - at);
    }
    return (int)(total_ms - at);
}

namespace {

// Зменшення premultiplied BGRA усередненням по прямокутнику.
//
// Усереднювати premultiplied можна просто так — саме заради цього він і
// premultiplied: колір у ньому вже помножений на прозорість, тож середнє
// чотирьох пікселів, з яких три прозорі, не дає сірої облямівки.
void downscale_bgra(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const int y0 = (int)((int64_t)y * sh / dh);
        int y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; ++x) {
            const int x0 = (int)((int64_t)x * sw / dw);
            int x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned acc[4] = {0, 0, 0, 0};
            unsigned n = 0;
            for (int sy = y0; sy < y1 && sy < sh; ++sy) {
                const uint8_t* row = src + ((size_t)sy * sw + x0) * 4;
                for (int sx = x0; sx < x1 && sx < sw; ++sx, row += 4) {
                    acc[0] += row[0]; acc[1] += row[1];
                    acc[2] += row[2]; acc[3] += row[3];
                    ++n;
                }
            }
            uint8_t* d = dst + ((size_t)y * dw + x) * 4;
            if (!n) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            for (int k = 0; k < 4; ++k) d[k] = (uint8_t)(acc[k] / n);
        }
    }
}

}  // namespace

// Зменшує кадри анімованої картинки до ANIM_MAX_SIDE. Природний розмір не
// чіпаємо: CSS має бачити картинку такою, якою вона є, а растр — це наша
// внутрішня справа (так само поводиться SVG, лише в інший бік).
void ImageCache::shrink(Image* img) const {
    if (img->frames.size() < 2) return;
    const int w = img->width, h = img->height;
    const int side = w > h ? w : h;
    if (w <= 0 || h <= 0 || side <= ANIM_MAX_SIDE) return;

    int nw = (int)((int64_t)w * ANIM_MAX_SIDE / side);
    int nh = (int)((int64_t)h * ANIM_MAX_SIDE / side);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    for (ImageFrame& f : img->frames) {
        if (f.width != w || f.height != h) continue;   // кадр іншого розміру не чіпаємо
        std::vector<uint8_t> small((size_t)nw * nh * 4);
        downscale_bgra(f.bgra.data(), w, h, small.data(), nw, nh);
        f.bgra.swap(small);
        f.width = nw;
        f.height = nh;
    }
    img->width = nw;
    img->height = nh;
}

void ImageCache::finish(Image* img) const {
    // Спершу зменшуємо, потім рахуємо вагу: інакше під стелю не пролізла б
    // жодна гіфка з чату, і всі вони стояли б нерухомо.
    shrink(img);
    img->total_ms = 0;
    img->bytes = 0;
    for (ImageFrame& f : img->frames) {
        img->bytes += f.bgra.size();
        // Нульова затримка трапляється і в GIF, і в кривому WebP. Крутити таку
        // анімацію означало б палити кадр щоразу; 10 мс — стеля в 100 к/с,
        // швидше за це око однаково не бачить.
        if (f.delay_ms < 10) f.delay_ms = 10;
        img->total_ms += f.delay_ms;
    }
    // Один кадр — це не анімація, скільки б там не стояло затримки.
    if (img->frames.size() < 2) img->total_ms = 0;

    // Завеликій анімації лишаємо самий перший кадр (див. ANIM_MAX_IMAGE_BYTES).
    // Те саме робить і режим «нерухомі», просто за іншим приводом.
    if (motion_ == Motion::Freeze || img->bytes > ANIM_MAX_IMAGE_BYTES) {
        if (img->frames.size() > 1) {
            img->frames.resize(1);
            img->bytes = img->frames[0].bgra.size();
            img->total_ms = 0;
        }
    }
}

void ImageCache::clear() { items_.clear(); }

bool ImageCache::known(const std::string& url) const {
    return items_.find(url) != items_.end();
}

size_t ImageCache::animated_count() const {
    size_t n = 0;
    for (const auto& kv : items_) if (kv.second.animated()) ++n;
    return n;
}

size_t ImageCache::animated_bytes() const {
    size_t n = 0;
    for (const auto& kv : items_) if (kv.second.animated()) n += kv.second.bytes;
    return n;
}

size_t ImageCache::gc_animated(const std::set<std::string>& in_use,
                               size_t max_count, size_t max_bytes) {
    // Кандидати на виліт: анімовані й нікому зараз не потрібні.
    std::vector<std::map<std::string, Image>::iterator> cand;
    size_t count = 0, bytes = 0;
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (!it->second.animated()) continue;
        ++count;
        bytes += it->second.bytes;
        if (in_use.find(it->first) == in_use.end()) cand.push_back(it);
    }
    if (count <= max_count && bytes <= max_bytes) return 0;

    // Найдавніше питані — першими.
    std::sort(cand.begin(), cand.end(),
              [](const std::map<std::string, Image>::iterator& a,
                 const std::map<std::string, Image>::iterator& b) {
                  return a->second.used < b->second.used;
              });
    size_t gone = 0;
    for (auto& it : cand) {
        if (count <= max_count && bytes <= max_bytes) break;
        --count;
        bytes -= it->second.bytes;
        if (evict_) evict_(it->first);
        items_.erase(it);
        ++gone;
    }
    return gone;
}

bool ImageCache::put(const std::string& url, const uint8_t* data, size_t len) {
    Image img;
    const bool ok = decode(data, len, &img);
    finish(&img);
    img.used = ++tick_;
    // Кладемо навіть невдалу: інакше кожен наступний кадр знову просив би в
    // Python те, що вже приходило й не розібралося.
    items_[url] = std::move(img);
    return ok;
}

// Приховані анімовані картинки поводяться так, ніби їх ще немає: розмітка
// сама підставить на їх місце код емоута — той самий шлях, що й для емоута,
// який не докачався. Нічого окремого вигадувати не довелося.
bool ImageCache::hidden(const Image& img) const {
    return motion_ == Motion::Hide && img.animated();
}

void ImageCache::set_motion(Motion m) {
    if (m == motion_) return;
    const bool was_frozen = motion_ == Motion::Freeze;
    motion_ = m;

    if (m == Motion::Freeze) {
        // Зупинити можна на місці: зайві кадри просто викидаємо.
        for (auto& kv : items_) freeze(kv.first, &kv.second);
        return;
    }
    // Повернути рух після «зупинити» можна лише перекачавши: самих байтів ми
    // не тримаємо. Кеш чистимо, картинки приїдуть знову.
    if (was_frozen) clear();
}

void ImageCache::freeze(const std::string& url, Image* img) {
    if (img->frames.size() < 2) return;
    // Стрічка тримає текстури кадрів — після обрізання їх більше немає.
    if (evict_) evict_(url);
    img->frames.resize(1);
    img->bytes = img->frames[0].bgra.size();
    img->total_ms = 0;
}

const Image* ImageCache::get(const std::string& url) {
    auto it = items_.find(url);
    if (it != items_.end()) {
        it->second.used = ++tick_;
        if (hidden(it->second)) return nullptr;
        return it->second.ok() ? &it->second : nullptr;
    }

    if (starts_with(url, "data:")) {
        std::vector<uint8_t> bytes;
        std::string mime;
        if (decode_data_url(url, &bytes, &mime) && !bytes.empty()) {
            put(url, bytes.data(), bytes.size());
            auto it2 = items_.find(url);
            if (it2 != items_.end() && it2->second.ok() && !hidden(it2->second))
                return &it2->second;
        }
        items_[url] = Image{};      // щоб не розбирати те саме щоразу
    }
    return nullptr;
}

bool ImageCache::decode_data_url(const std::string& url, std::vector<uint8_t>* out,
                                 std::string* mime) {
    const size_t comma = url.find(',');
    if (comma == std::string::npos) return false;
    const std::string head = url.substr(5, comma - 5);      // після «data:»
    const size_t semi = head.find(';');
    *mime = semi == std::string::npos ? head : head.substr(0, semi);

    const char* body = url.c_str() + comma + 1;
    const size_t body_len = url.size() - comma - 1;
    if (head.find("base64") != std::string::npos)
        base64_decode(body, body_len, out);
    else
        percent_decode(body, body_len, out);
    return true;
}

bool ImageCache::decode(const uint8_t* data, size_t len, Image* out) {
    if (!data || len < 8) return false;
    // Порядок за вартістю перевірки: підпис WebP і текст SVG дешеві, решту
    // віддаємо WIC.
    if (is_webp(data, len)) return decode_webp(data, len, out);
    if (is_svg(data, len))  return decode_svg(data, len, out);
#ifdef _WIN32
    return decode_wic(data, len, out);
#else
    return decode_stb(data, len, out);
#endif
}

#ifdef _WIN32

namespace {

// Прочитати число з метаданих кадру/файлу. WIC віддає їх PROPVARIANT-ами
// різних типів (UI1 для способу затирання, UI2 для координат), тож зводимо до
// одного.
bool meta_uint(IWICMetadataQueryReader* r, const wchar_t* name, unsigned* out) {
    if (!r) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(r->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1)      { *out = v.bVal;  ok = true; }
        else if (v.vt == VT_UI2) { *out = v.uiVal; ok = true; }
        else if (v.vt == VT_UI4) { *out = v.ulVal; ok = true; }
    }
    PropVariantClear(&v);
    return ok;
}

// Накласти кадр на полотно в точці (dx, dy), звичайним «source-over».
//
// Обидва боки premultiplied, тож формула проста: out = src + dst*(1-a). Якби
// пікселі були straight, тут довелося б ділити на альфу — саме тому ми й
// тримаємо все premultiplied від самого декодера.
void blend_over(uint8_t* dst, int dw, int dh, const uint8_t* src,
                int sw, int sh, int dx, int dy) {
    for (int y = 0; y < sh; ++y) {
        const int ty = dy + y;
        if (ty < 0 || ty >= dh) continue;
        const uint8_t* s = src + (size_t)y * sw * 4;
        uint8_t* d = dst + ((size_t)ty * dw + dx) * 4;
        for (int x = 0; x < sw; ++x, s += 4, d += 4) {
            const int tx = dx + x;
            if (tx < 0 || tx >= dw) continue;
            const unsigned a = s[3];
            if (a == 255) { memcpy(d, s, 4); continue; }
            if (a == 0) continue;
            const unsigned inv = 255 - a;
            d[0] = (uint8_t)(s[0] + (d[0] * inv + 127) / 255);
            d[1] = (uint8_t)(s[1] + (d[1] * inv + 127) / 255);
            d[2] = (uint8_t)(s[2] + (d[2] * inv + 127) / 255);
            d[3] = (uint8_t)(s[3] + (d[3] * inv + 127) / 255);
        }
    }
}

void clear_rect(uint8_t* buf, int w, int h, int rx, int ry, int rw, int rh) {
    for (int y = ry; y < ry + rh; ++y) {
        if (y < 0 || y >= h) continue;
        for (int x = rx; x < rx + rw; ++x) {
            if (x < 0 || x >= w) continue;
            memset(buf + ((size_t)y * w + x) * 4, 0, 4);
        }
    }
}

}  // namespace

// Дістати кадр у premultiplied BGRA разом з його власним розміром.
static bool wic_frame_pixels(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame,
                             std::vector<uint8_t>* out, int* w, int* h) {
    IWICFormatConverter* conv = nullptr;
    if (FAILED(wic->CreateFormatConverter(&conv))) return false;
    bool ok = false;
    if (SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT fw = 0, fh = 0;
        conv->GetSize(&fw, &fh);
        if (fw && fh) {
            out->resize((size_t)fw * fh * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, fw * 4, (UINT)out->size(), out->data()))) {
                *w = (int)fw;
                *h = (int)fh;
                ok = true;
            }
        }
    }
    conv->Release();
    return ok;
}

bool ImageCache::decode_wic(const uint8_t* data, size_t len, Image* out) {
    if (!wic_) return false;

    IWICStream* stream = nullptr;
    if (FAILED(wic_->CreateStream(&stream))) return false;
    // InitializeFromMemory не копіює — байти мають жити до кінця розбору;
    // вони й живуть, це аргумент виклику.
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    if (SUCCEEDED(stream->InitializeFromMemory(const_cast<uint8_t*>(data), (DWORD)len)) &&
        SUCCEEDED(wic_->CreateDecoderFromStream(stream, nullptr,
                                                WICDecodeMetadataCacheOnDemand, &dec))) {
        UINT count = 0;
        dec->GetFrameCount(&count);

        // Розмір полотна GIF беремо з опису файлу. Немає його — значить це не
        // GIF (PNG, JPEG), і кадр там один на весь розмір.
        //
        // Чому це важливо. WIC віддає кадри GIF ТАК, ЯК ВОНИ ЛЕЖАТЬ У ФАЙЛІ:
        // це не готові картинки, а латки — шматок зі своїм зсувом, який
        // накладається на попередній стан. Малювати їх по черзі як самостійні
        // кадри означало б смикану анімацію з кусками не на своїх місцях.
        // Тому збираємо кадри самі: полотно, накладення, правило затирання.
        unsigned cw = 0, ch = 0;
        IWICMetadataQueryReader* dmeta = nullptr;
        if (SUCCEEDED(dec->GetMetadataQueryReader(&dmeta)) && dmeta) {
            meta_uint(dmeta, L"/logscrdesc/Width", &cw);
            meta_uint(dmeta, L"/logscrdesc/Height", &ch);
            dmeta->Release();
        }
        const bool animated = count > 1 && cw > 0 && ch > 0;

        if (!animated) {
            IWICBitmapFrameDecode* frame = nullptr;
            std::vector<uint8_t> px;
            int fw = 0, fh = 0;
            if (SUCCEEDED(dec->GetFrame(0, &frame))) {
                if (wic_frame_pixels(wic_, frame, &px, &fw, &fh)) {
                    ImageFrame f;
                    f.width = fw;
                    f.height = fh;
                    f.delay_ms = 0;
                    f.bgra = std::move(px);
                    out->width = fw;
                    out->height = fh;
                    out->nat_width = fw;
                    out->nat_height = fh;
                    out->frames.push_back(std::move(f));
                    ok = true;
                }
                frame->Release();
            }
        } else {
            std::vector<uint8_t> canvas((size_t)cw * ch * 4, 0);
            std::vector<uint8_t> saved;              // під спосіб затирання 3
            // Що зробити ПЕРЕД наступним кадром — це властивість поточного,
            // тому несемо її через оберт циклу.
            unsigned prev_disposal = 0;
            int px_ = 0, py_ = 0, pw_ = 0, ph_ = 0;

            for (UINT i = 0; i < count; ++i) {
                IWICBitmapFrameDecode* frame = nullptr;
                if (FAILED(dec->GetFrame(i, &frame))) break;

                unsigned left = 0, top = 0, disposal = 0, delay = 0;
                IWICMetadataQueryReader* meta = nullptr;
                if (SUCCEEDED(frame->GetMetadataQueryReader(&meta)) && meta) {
                    meta_uint(meta, L"/imgdesc/Left", &left);
                    meta_uint(meta, L"/imgdesc/Top", &top);
                    meta_uint(meta, L"/grctlext/Disposal", &disposal);
                    meta_uint(meta, L"/grctlext/Delay", &delay);
                    meta->Release();
                }

                // Прибираємо за попереднім кадром.
                //   2 — вернути тло: витираємо його прямокутник;
                //   3 — вернути як було: відновлюємо збережене полотно;
                //   0/1 — лишити як є.
                if (prev_disposal == 2) {
                    clear_rect(canvas.data(), (int)cw, (int)ch, px_, py_, pw_, ph_);
                } else if (prev_disposal == 3 && !saved.empty()) {
                    canvas = saved;
                }

                std::vector<uint8_t> px;
                int fw = 0, fh = 0;
                if (!wic_frame_pixels(wic_, frame, &px, &fw, &fh)) { frame->Release(); break; }

                if (disposal == 3) saved = canvas;   // знадобиться перед наступним
                blend_over(canvas.data(), (int)cw, (int)ch, px.data(), fw, fh,
                           (int)left, (int)top);

                ImageFrame f;
                f.width = (int)cw;
                f.height = (int)ch;
                // Затримка в GIF — у сотих секунди. Нуль і одну соту браузери
                // здавна показують як 100 мс: інакше старі картинки крутилися б
                // із шаленою швидкістю.
                f.delay_ms = delay > 1 ? (int)delay * 10 : 100;
                f.bgra = canvas;
                out->frames.push_back(std::move(f));

                prev_disposal = disposal;
                px_ = (int)left; py_ = (int)top; pw_ = fw; ph_ = fh;
                frame->Release();
                ok = true;
            }
            if (ok) {
                out->width = (int)cw;
                out->height = (int)ch;
                out->nat_width = (int)cw;
                out->nat_height = (int)ch;
            }
        }
    }
    if (dec) dec->Release();
    stream->Release();
    return ok;
}

#else


// --- поза Windows: stb_image ---------------------------------------------
//
// Чому саме він, а не giflib+libpng+libjpeg: один заголовок замість трьох
// бібліотек, і — головне — анімований GIF він віддає ВЖЕ СКЛАДЕНИМ. Тобто вся
// та морока з латками кадрів, зсувами й способом затирання, яку під Windows
// довелося писати самим, тут уже зроблена.
bool ImageCache::decode_stb(const uint8_t* data, size_t len, Image* out) {
    // GIF пробуємо першим і окремим викликом: лише він уміє віддати всі кадри
    // разом. Для решти форматів ця функція не годиться.
    if (len > 3 && !memcmp(data, "GIF", 3)) {
        int* delays = nullptr;
        int w = 0, h = 0, frames = 0, comp = 0;
        stbi_uc* px = stbi_load_gif_from_memory(data, (int)len, &delays,
                                                &w, &h, &frames, &comp, 4);
        if (px) {
            const size_t one = (size_t)w * h * 4;
            for (int i = 0; i < frames; ++i) {
                ImageFrame f;
                f.width = w;
                f.height = h;
                // Затримка в GIF — у мілісекундах уже тут. Нуль і одну соту
                // браузери здавна показують як 100 мс: інакше старі картинки
                // крутилися б із шаленою швидкістю.
                const int d = delays ? delays[i] : 100;
                f.delay_ms = d > 10 ? d : 100;
                f.bgra.assign(px + one * i, px + one * (i + 1));
                rgba_to_bgra(f.bgra.data(), (size_t)w * h);
                premultiply(f.bgra.data(), (size_t)w * h);
                out->frames.push_back(std::move(f));
            }
            stbi_image_free(px);
            free(delays);
            if (!out->frames.empty()) {
                out->width = w;
                out->height = h;
                out->nat_width = w;
                out->nat_height = h;
                return true;
            }
        }
        return false;
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!px) return false;
    ImageFrame f;
    f.width = w;
    f.height = h;
    f.delay_ms = 0;
    f.bgra.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    rgba_to_bgra(f.bgra.data(), (size_t)w * h);
    premultiply(f.bgra.data(), (size_t)w * h);
    out->width = w;
    out->height = h;
    out->nat_width = w;
    out->nat_height = h;
    out->frames.push_back(std::move(f));
    return true;
}


#endif

bool ImageCache::decode_webp(const uint8_t* data, size_t len, Image* out) {
    WebPData wd{data, len};
    WebPDemuxer* dmx = WebPDemux(&wd);
    if (!dmx) {
        // Не анімований контейнер — пробуємо як звичайний WebP.
        int w = 0, h = 0;
        uint8_t* px = WebPDecodeBGRA(data, len, &w, &h);
        if (!px) return false;
        ImageFrame f;
        f.width = w; f.height = h; f.delay_ms = 0;
        f.bgra.assign(px, px + (size_t)w * h * 4);
        WebPFree(px);
        premultiply(f.bgra.data(), (size_t)w * h);
        out->width = w; out->height = h;
        out->nat_width = w; out->nat_height = h;
        out->frames.push_back(std::move(f));
        return true;
    }

    const uint32_t cw = WebPDemuxGetI(dmx, WEBP_FF_CANVAS_WIDTH);
    const uint32_t ch = WebPDemuxGetI(dmx, WEBP_FF_CANVAS_HEIGHT);
    bool ok = false;
    if (cw && ch) {
        // Анімацію збираємо на полотні: кадр WebP може бути шматком, який
        // накладається на попередній, а нам потрібні готові кадри.
        WebPAnimDecoderOptions opt;
        WebPAnimDecoderOptionsInit(&opt);
        opt.color_mode = MODE_BGRA;
        opt.use_threads = 0;
        WebPAnimDecoder* adec = WebPAnimDecoderNew(&wd, &opt);
        if (adec) {
            int prev_ts = 0;
            while (WebPAnimDecoderHasMoreFrames(adec)) {
                uint8_t* buf = nullptr;
                int ts = 0;
                if (!WebPAnimDecoderGetNext(adec, &buf, &ts)) break;
                ImageFrame f;
                f.width = (int)cw;
                f.height = (int)ch;
                f.bgra.assign(buf, buf + (size_t)cw * ch * 4);
                premultiply(f.bgra.data(), (size_t)cw * ch);
                f.delay_ms = ts - prev_ts;
                prev_ts = ts;
                out->frames.push_back(std::move(f));
            }
            WebPAnimDecoderDelete(adec);
            if (!out->frames.empty()) {
                out->width = (int)cw;
                out->height = (int)ch;
                out->nat_width = (int)cw;
                out->nat_height = (int)ch;
                ok = true;
            }
        }
    }
    WebPDemuxDelete(dmx);
    return ok;
}

bool ImageCache::decode_svg(const uint8_t* data, size_t len, Image* out) {
    // nanosvgParse псує вхідний буфер, тож даємо йому копію з нулем на кінці.
    std::string text((const char*)data, len);
    // nanosvg не має шрифтового рушія: <text> він просто пропускає, і значок
    // виходить порожньою плашкою. Чесно кажемо, що не вміємо — вище замість
    // картинки намалюють текстову плашку.
    if (text.find("<text") != std::string::npos) {
        out->unsupported = true;
        return false;
    }
    NSVGimage* svg = nsvgParse(&text[0], "px", 96.0f);
    if (!svg) return false;

    // Значки в чаті малі (16–24 px), але CSS може розтягнути їх на 1em при
    // великому кеглі. Растеризуємо із запасом, а зменшує вже Direct2D —
    // краще, ніж збільшувати замалий растр.
    const float kTarget = 64.0f;
    float w = svg->width, h = svg->height;
    if (w <= 0 || h <= 0) { nsvgDelete(svg); return false; }
    const float scale = kTarget / (w > h ? w : h);
    const int ow = (int)(w * scale + 0.5f), oh = (int)(h * scale + 0.5f);
    if (ow <= 0 || oh <= 0) { nsvgDelete(svg); return false; }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(svg); return false; }

    ImageFrame f;
    f.width = ow;
    f.height = oh;
    f.delay_ms = 0;
    f.bgra.assign((size_t)ow * oh * 4, 0);
    nsvgRasterize(rast, svg, 0, 0, scale, f.bgra.data(), ow, oh, ow * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    rgba_to_bgra(f.bgra.data(), (size_t)ow * oh);   // nanosvg віддає RGBA
    premultiply(f.bgra.data(), (size_t)ow * oh);
    out->width = ow;
    out->height = oh;
    // А ось природний — той, що написаний у самому SVG: саме його має бачити CSS.
    out->nat_width = (int)(w + 0.5f);
    out->nat_height = (int)(h + 0.5f);
    out->frames.push_back(std::move(f));
    return true;
}

}  // namespace hominka
