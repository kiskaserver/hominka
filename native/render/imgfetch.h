// Качання картинок чату: емоути й значки.
//
// Раніше байти надсилав Python (hominka/imagefetch.py), і кеш просто клав
// готове. Тепер джерела чату наші, тож і картинки наші.
//
// Розділення праці навмисне: качають кілька фонових потоків, а РОЗБИРАЄ
// (декодує) головний — там же, де малює. Декодер пише в спільний кеш, з якого
// щокадру читає розкладка, і робити його потокобезпечним заради кількох
// емоутів на секунду означало б платити блокуванням у найгарячішому місці.
//
// Кількість потоків мала свідомо: чотири вистачає, щоб пачка нових емоутів
// приїхала за одну-дві секунди, і замало, щоб хтось порахував це за напад на
// свій CDN.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hominka {

class ImageFetch {
public:
    ~ImageFetch();

    void start(int threads = 4);
    void stop();

    // «Ця адреса потрібна». Повтори й «data:» відсіюються тут же: перші —
    // марна робота, другі самодостатні й мережі не потребують.
    void want(const std::string& url);

    // Забрати одну готову картинку. false — поки нічого не приїхало.
    bool take(std::string* url, std::vector<uint8_t>* data);

    size_t pending() const;

private:
    void worker();

    std::vector<std::thread> threads_;
    mutable std::mutex mx_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::set<std::string> asked_;          // щоб не питати те саме двічі
    std::deque<std::pair<std::string, std::vector<uint8_t>>> done_;
    bool stopping_ = false;
};

}  // namespace hominka
