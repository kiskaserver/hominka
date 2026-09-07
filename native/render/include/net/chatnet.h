// Усі площадки разом: що читаємо, звідки й із якою затримкою.
//
// Джерела не знають одне про одного і кожне живе у власному потоці. Тут вони
// зводяться до однієї черги, яку розбирає той, хто малює, — у своєму темпі й у
// своєму потоці. Без цієї межі малювання відбувалося б із чужих потоків, і
// перший же емоут, що приїхав під час розкладки, зіпсував би кадр.
//
// Тут же живе затримка чату. Вона не про мережу, а про читабельність: коли
// пишуть швидше, ніж людина читає, стрічка перестає бути стрічкою. Затримані
// повідомлення чекають своєї секунди, а от «прибрати» діє одразу — і заразом
// викидає з черги те, що ще навіть не показали. Показати повідомлення й
// негайно його зняти гірше, ніж не показати зовсім.
#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "net/chatsrc.h"

namespace hominka {

class TwitchSource;
class KickSource;
class YouTubeSource;
class SiteSource;

class ChatNet {
public:
    ChatNet();
    ~ChatNet();

    // Приводить набір джерел у відповідність до налаштувань. Викликається і на
    // старті, і після кожної зміни в панелі; канал, що не змінився, не
    // перепідключається.
    void apply(const Config& cfg);

    // Забрати подію, чий час настав. false — поки нічого.
    bool take(ChatEvent* ev);

    void stop();

    // Що показати людині: до чого під'єдналися, а де не вийшло. Рядком — для
    // журналу й командного рядка.
    std::string status() const;

    // Те саме, але по джерелах: панелі потрібен не рядок, а стан кожного
    // окремо — інакше «щось не так» доводиться вичитувати з речення.
    struct SourceInfo {
        const char* name = "";     // «Twitch», «Kick», «YouTube», «Сайт»
        bool configured = false;   // канал вписано
        bool connected = false;
        std::string note;          // причина, якщо не під'єдналися
    };
    std::vector<SourceInfo> sources() const;

private:
    void push(const ChatEvent& ev);
    void drop_pending(const ChatEvent& ev);

    struct Delayed {
        int64_t due_ms;
        ChatEvent ev;
    };

    std::unique_ptr<TwitchSource> twitch_;
    std::unique_ptr<KickSource> kick_;
    std::unique_ptr<YouTubeSource> youtube_;
    std::unique_ptr<SiteSource> site_;

    std::string twitch_name_, kick_name_, youtube_name_, site_url_;
    int delay_ms_ = 0;

    mutable std::mutex mx_;
    std::deque<ChatEvent> ready_;
    std::deque<Delayed> waiting_;
};

}  // namespace hominka
