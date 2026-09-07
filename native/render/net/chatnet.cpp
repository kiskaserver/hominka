#include "net/chatnet.h"

#include <chrono>
#include <cstring>
#include <set>

#include "net/src_kick.h"
#include "net/src_site.h"
#include "net/src_twitch.h"
#include "net/src_youtube.h"

namespace hominka {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Люди вписують канал як завгодно: «twitch.tv/nick», «@nick», «  Nick  ».
// Зводимо до того, що чекає площадка, — інакше «не знайшли канал» отримає той,
// хто просто скопіював адресу з браузера.
std::string clean_channel(std::string s, const char* host) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    s = s.substr(a, b - a);
    if (s.empty()) return s;

    const size_t h = s.find(host);
    if (h != std::string::npos) {
        s = s.substr(h + strlen(host));
        while (!s.empty() && s[0] == '/') s.erase(0, 1);
    }
    const size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    const size_t q = s.find_first_of("?#");
    if (q != std::string::npos) s = s.substr(0, q);
    return s;
}

}  // namespace

ChatNet::ChatNet() = default;
ChatNet::~ChatNet() { stop(); }

void ChatNet::apply(const Config& cfg) {
    delay_ms_ = cfg.chat_delay > 0 ? cfg.chat_delay * 1000 : 0;

    const std::string tw = clean_channel(cfg.twitch, "twitch.tv");
    const std::string kk = clean_channel(cfg.kick, "kick.com");
    // YouTube лишаємо як вписали: там і «@нік», і UC-id, і посилання на саму
    // трансляцію — розбирається це вже всередині джерела.
    std::string yt = cfg.youtube;
    {
        size_t a = 0, b = yt.size();
        while (a < b && (unsigned char)yt[a] <= ' ') ++a;
        while (b > a && (unsigned char)yt[b - 1] <= ' ') --b;
        yt = yt.substr(a, b - a);
    }

    if (tw != twitch_name_) {
        if (twitch_) { twitch_->stop(); twitch_.reset(); }
        twitch_name_ = tw;
        if (!tw.empty()) {
            twitch_.reset(new TwitchSource());
            twitch_->start(tw, [this](const ChatEvent& ev) { push(ev); });
        }
    }
    if (kk != kick_name_) {
        if (kick_) { kick_->stop(); kick_.reset(); }
        kick_name_ = kk;
        if (!kk.empty()) {
            kick_.reset(new KickSource());
            kick_->start(kk, [this](const ChatEvent& ev) { push(ev); });
        }
    }
    // Чат сайту йде останнім навмисно: йому треба знати, які площадки ми вже
    // читаємо самі. Сервер мостить чужі чати у свій, і без цього кожне таке
    // повідомлення з'являлося б у стрічці двічі.
    const std::string site = cfg.site_url;
    if (site != site_url_) {
        if (site_) { site_->stop(); site_.reset(); }
        site_url_ = site;
        if (!site.empty()) {
            std::set<std::string> skip;
            if (!tw.empty()) skip.insert("twitch");
            if (!kk.empty()) skip.insert("kick");
            if (!yt.empty()) skip.insert("youtube");
            site_.reset(new SiteSource());
            site_->start(site, skip, [this](const ChatEvent& ev) { push(ev); });
        }
    }

    if (yt != youtube_name_) {
        if (youtube_) { youtube_->stop(); youtube_.reset(); }
        youtube_name_ = yt;
        if (!yt.empty()) {
            youtube_.reset(new YouTubeSource());
            youtube_->start(yt, [this](const ChatEvent& ev) { push(ev); });
        }
    }
}

void ChatNet::push(const ChatEvent& ev) {
    std::lock_guard<std::mutex> lock(mx_);

    // «Прибрати» діє негайно й забирає з собою те, що ще не показане.
    if (ev.type != ChatEvent::Type::Message || delay_ms_ == 0) {
        if (ev.type != ChatEvent::Type::Message) drop_pending(ev);
        ready_.push_back(ev);
        return;
    }
    waiting_.push_back({now_ms() + delay_ms_, ev});
}

void ChatNet::drop_pending(const ChatEvent& ev) {
    if (waiting_.empty()) return;
    std::deque<Delayed> keep;
    for (auto& d : waiting_) {
        const bool hit = ev.type == ChatEvent::Type::Delete
                             ? (!ev.id.empty() && d.ev.msg.id == ev.id)
                             : (!ev.nick.empty() && d.ev.msg.nick == ev.nick);
        if (!hit) keep.push_back(std::move(d));
    }
    waiting_.swap(keep);
}

bool ChatNet::take(ChatEvent* ev) {
    std::lock_guard<std::mutex> lock(mx_);
    // Затримані випускаємо по одному за виклик: пачка, що висипалася разом,
    // не була б стрічкою — саме від цього затримка й рятує.
    if (!waiting_.empty() && waiting_.front().due_ms <= now_ms()) {
        *ev = std::move(waiting_.front().ev);
        waiting_.pop_front();
        return true;
    }
    if (ready_.empty()) return false;
    *ev = std::move(ready_.front());
    ready_.pop_front();
    return true;
}

std::string ChatNet::status() const {
    std::string out;
    auto add = [&out](const std::string& s) {
        if (s.empty()) return;
        if (!out.empty()) out += "   ";
        out += s;
    };
    if (twitch_)
        add("Twitch: " + (twitch_->connected() ? "читаємо"
                                               : (twitch_->error().empty() ? "під'єднуюся…"
                                                                           : twitch_->error())));
    if (kick_)
        add("Kick: " + (kick_->connected() ? "читаємо"
                                           : (kick_->error().empty() ? "під'єднуюся…"
                                                                     : kick_->error())));
    if (site_)
        add("Сайт: " + (site_->connected() ? "читаємо"
                                           : (site_->error().empty() ? "під'єднуюся…"
                                                                     : site_->error())));
    if (youtube_)
        add("YouTube: " + (youtube_->connected()
                               ? "читаємо"
                               : (youtube_->error().empty() ? "чекаю на ефір…"
                                                            : youtube_->error())));
    return out.empty() ? "Жодного каналу не вказано." : out;
}

void ChatNet::stop() {
    if (twitch_) { twitch_->stop(); twitch_.reset(); }
    if (kick_) { kick_->stop(); kick_.reset(); }
    if (youtube_) { youtube_->stop(); youtube_.reset(); }
    if (site_) { site_->stop(); site_.reset(); }
    site_url_.clear();
    twitch_name_.clear();
    kick_name_.clear();
    youtube_name_.clear();
}

}  // namespace hominka
