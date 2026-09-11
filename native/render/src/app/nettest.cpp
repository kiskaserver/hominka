#include "app/nettest.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

#include "net/src_kick.h"
#include "net/src_site.h"
#include "net/src_twitch.h"
#include "net/src_youtube.h"

namespace hominka {

namespace {

void print(const ChatEvent& ev, std::atomic<int>* count) {
    switch (ev.type) {
    case ChatEvent::Type::Message: {
        ++*count;
        const ChatMessage& m = ev.msg;
        if (m.kind == "system") {
            printf("  * [%s] %s\n", m.event.empty() ? "-" : m.event.c_str(), m.text.c_str());
        } else {
            printf("  %-20s %s", m.name.empty() ? m.nick.c_str() : m.name.c_str(),
                   m.text.c_str());
            if (!m.emotes.empty()) printf("   [емоутів: %d]", (int)m.emotes.size());
            if (!m.badge_icons.empty()) printf("   [значків: %d]", (int)m.badge_icons.size());
            else if (!m.badges.empty()) printf("   [плашок: %d]", (int)m.badges.size());
            if (!m.amount.empty()) printf("   [%s]", m.amount.c_str());
            if (!m.event.empty()) printf("   [%s]", m.event.c_str());
            printf("\n");
        }
        break;
    }
    case ChatEvent::Type::Delete:
        printf("  (прибрано повідомлення %s)\n", ev.id.c_str());
        break;
    case ChatEvent::Type::Purge:
        printf("  (прибрано все від %s)\n", ev.nick.c_str());
        break;
    case ChatEvent::Type::Clear:
        printf("  (чат почищено)\n");
        break;
    }
    fflush(stdout);
}

// Крутиться seconds секунд, доки джерело шле події. Спільне для всіх площадок:
// різниця лише в тому, кого запускати.
int watch(const std::function<bool(ChatSink)>& start, const std::function<void()>& stop,
          const std::function<std::string()>& error, int seconds,
          std::atomic<int>* count) {
    if (!start([count](const ChatEvent& ev) { print(ev, count); })) {
        fprintf(stderr, "не під'єдналося: %s\n", error().c_str());
        return 1;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop();

    if (!error().empty()) fprintf(stderr, "помилка з'єднання: %s\n", error().c_str());
    printf("прочитано повідомлень: %d\n", count->load());
    return 0;
}

}  // namespace

int nettest(const std::string& platform, const std::string& channel, int seconds) {
    printf("під'єднуюся до %s/%s на %d с…\n", platform.c_str(), channel.c_str(), seconds);
    fflush(stdout);
    std::atomic<int> count{0};

    if (platform == "kick") {
        KickSource src;
        const int rc = watch(
            [&](ChatSink s) {
                if (!src.start(channel, std::move(s))) return false;
                printf("канал %s, кімната знайдена\n", src.channel_id().c_str());
                fflush(stdout);
                return true;
            },
            [&] { src.stop(); }, [&] { return src.error(); }, seconds, &count);
        return rc;
    }

    if (platform == "twitch") {
        TwitchSource src;
        const int rc = watch([&](ChatSink s) { return src.start(channel, std::move(s)); },
                             [&] { src.stop(); }, [&] { return src.error(); }, seconds, &count);
        if (rc == 0 && !src.room_id().empty()) printf("канал %s\n", src.room_id().c_str());
        return rc;
    }

    if (platform == "site") {
        SiteSource src;
        printf("сокет: %s\n", site_ws_url(channel).c_str());
        fflush(stdout);
        return watch([&](ChatSink s) { return src.start(channel, {}, std::move(s)); },
                     [&] { src.stop(); }, [&] { return src.error(); }, seconds, &count);
    }

    if (platform == "youtube") {
        YouTubeSource src;
        return watch([&](ChatSink s) { return src.start(channel, std::move(s)); },
                     [&] { src.stop(); }, [&] { return src.error(); }, seconds, &count);
    }

    fprintf(stderr, "невідома площадка: %s (треба twitch, kick, youtube або site)\n",
            platform.c_str());
    return 1;
}

}  // namespace hominka
