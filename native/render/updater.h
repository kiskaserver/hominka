// Оновлення програми: перевірити, завантажити, підмінити.
//
// Порядок навмисний і не міняється: спершу питаємо маніфест каналу, ПЕРЕВІРЯЄМО
// ПІДПИС (release.h) і лише тоді качаємо архів; після завантаження звіряємо
// sha256 і лише тоді розпаковуємо. З мережі приїжджає код, який виконуватиметься
// на машині людини, — жоден із цих кроків не можна пропустити «щоб швидше».
//
// Мережа крутиться у власних потоках: вікно чату не має підвисати через те, що
// сервер оновлень задумався.
//
// Windows-only, і це поки що чесно: під Linux програма їде AppImage'ом, у якого
// своя історія оновлення, і вигадувати її наперед, не маючи де перевірити, —
// гірше, ніж не мати зовсім.
#pragma once

#ifdef _WIN32

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "release.h"

namespace hominka {

class Updater {
public:
    enum class State {
        Idle,          // ще не питали
        Checking,
        UpToDate,
        Available,     // є новіша версія
        Downloading,
        Ready,         // архів завантажено й перевірено
        Failed,
    };

    ~Updater();

    // current — наша версія; installed_channel — канал, з якого нас поставили.
    // Перехід зі свіжої бети на стабільну — це теж оновлення, хоч номер там і
    // менший, тому канал теж має значення.
    void check(const std::string& channel, const std::string& current,
               const std::string& installed_channel);
    void download();
    // Розпаковує поруч і запускає підмінника. Повертає причину відмови або
    // порожній рядок; після успіху програму треба закрити.
    std::string install();

    State state() const { return state_; }
    Release release() const;
    std::string error() const;
    // Скільки завантажено, у відсотках (0..100).
    int percent() const { return percent_; }

    // Прибирає завантажені архіви з тимчасової теки. Кожен — двісті мегабайтів,
    // а Windows свій %TEMP% сама не чистить.
    static int cleanup_downloads(const std::string& keep = "");

private:
    void run_check(std::string channel, std::string current, std::string installed);
    void run_download(Release rel);

    std::atomic<State> state_{State::Idle};
    std::atomic<int> percent_{0};
    mutable std::mutex mx_;
    Release release_;
    std::string error_;
    std::string zip_path_;
    std::thread worker_;
};

}  // namespace hominka

#endif  // _WIN32
