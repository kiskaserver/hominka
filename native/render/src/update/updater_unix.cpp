// Оновлення під Linux: та сама перевірка, інша підміна.
//
// Спільне з Windows (update/updater.cpp) — порядок, від якого не відступаємо:
// маніфест → ПІДПИС → завантаження → sha256 → і лише тоді щось міняється на
// диску. З мережі приїжджає код, який виконуватиметься на машині людини.
//
// Різне — сама підміна, і різниця тут на користь Linux. Запущений .exe Windows
// тримає замкненим, тому там працює окремий скрипт, який чекає нашого виходу.
// Тут програма — один файл AppImage, і замінити його можна просто на місці:
// ядро тримає вже відкритий inode, а нове ім'я отримає новий файл. Тож ані
// скрипта, ані очікування не потрібно — записали поруч, перейменували,
// перезапустилися.
#ifndef _WIN32

#include "update/updater.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <vector>

#include <mbedtls/sha256.h>
#include <nlohmann/json.hpp>

#include "net/net_http.h"

namespace hominka {

namespace {

using json = nlohmann::json;

std::string hex(const unsigned char* d, size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += kHex[d[i] >> 4];
        out += kHex[d[i] & 15];
    }
    return out;
}

// Де лежить сам AppImage. Змінну ставить його ж запускач; без неї ми не
// всередині AppImage (запустили розпакований бінар) — тоді оновлювати нічого.
std::string appimage_path() {
    const char* p = getenv("APPIMAGE");
    return p && *p ? p : std::string();
}

std::string manifest_url(const std::string& channel) {
    return "https://update.svitix.com/hominka/" + channel + ".json";
}

}  // namespace

Updater::~Updater() {
    if (worker_.joinable()) worker_.join();
}

Release Updater::release() const {
    std::lock_guard<std::mutex> lock(mx_);
    return release_;
}

std::string Updater::error() const {
    std::lock_guard<std::mutex> lock(mx_);
    return error_;
}

void Updater::check(const std::string& channel, const std::string& current,
                    const std::string& installed_channel) {
    if (state_ == State::Checking || state_ == State::Downloading) return;
    if (worker_.joinable()) worker_.join();
    state_ = State::Checking;
    worker_ = std::thread(&Updater::run_check, this, channel, current, installed_channel);
}

void Updater::run_check(std::string channel, std::string current, std::string installed) {
    const HttpResult r = http_get(manifest_url(channel), 20);
    if (!r.ok()) {
        std::lock_guard<std::mutex> lock(mx_);
        error_ = "не вдалося звернутися до сервера оновлень";
        state_ = State::Failed;
        return;
    }

    std::string err;
    const Release rel = parse_manifest(r.body, channel, &err);
    if (!rel.ok()) {
        std::lock_guard<std::mutex> lock(mx_);
        error_ = err.empty() ? "маніфест не розібрався" : err;
        state_ = State::Failed;
        return;
    }

    // Перехід між каналами — теж оновлення, хоч номер там може бути й меншим.
    const bool switched = !installed.empty() && installed != channel;
    if (!switched && !is_newer(rel.version, current)) {
        std::lock_guard<std::mutex> lock(mx_);
        release_ = rel;
        state_ = State::UpToDate;
        return;
    }
    std::lock_guard<std::mutex> lock(mx_);
    release_ = rel;
    state_ = State::Available;
}

void Updater::download() {
    if (state_ != State::Available) return;
    if (worker_.joinable()) worker_.join();
    Release rel;
    {
        std::lock_guard<std::mutex> lock(mx_);
        rel = release_;
    }
    state_ = State::Downloading;
    percent_ = 0;
    worker_ = std::thread(&Updater::run_download, this, rel);
}

void Updater::run_download(Release rel) {
    // Качаємо в пам'ять: AppImage — це десятки мегабайтів, не двісті, як був
    // архів із Python. Файл кладемо поруч із самим AppImage, а не в /tmp:
    // перейменувати можна лише в межах однієї файлової системи, а /tmp часто
    // окремий.
    const std::string self = appimage_path();
    if (self.empty()) {
        std::lock_guard<std::mutex> lock(mx_);
        error_ = "програма запущена не як AppImage — оновіться вручну";
        state_ = State::Failed;
        return;
    }

    const HttpResult r = http_get(rel.url, 600);
    if (!r.ok() || r.body.empty()) {
        std::lock_guard<std::mutex> lock(mx_);
        error_ = "не завантажилося";
        state_ = State::Failed;
        return;
    }
    percent_ = 90;

    unsigned char digest[32] = {0};
    mbedtls_sha256((const unsigned char*)r.body.data(), r.body.size(), digest, 0);
    if (hex(digest, sizeof digest) != rel.sha256) {
        std::lock_guard<std::mutex> lock(mx_);
        error_ = "сума не збіглася — файл пошкоджений або підмінений";
        state_ = State::Failed;
        return;
    }

    const std::string tmp = self + ".new";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) {
            std::lock_guard<std::mutex> lock(mx_);
            error_ = std::string("нема куди записати: ") + strerror(errno);
            state_ = State::Failed;
            return;
        }
        f.write(r.body.data(), (std::streamsize)r.body.size());
    }
    chmod(tmp.c_str(), 0755);

    {
        std::lock_guard<std::mutex> lock(mx_);
        zip_path_ = tmp;
    }
    percent_ = 100;
    state_ = State::Ready;
}

std::string Updater::install() {
    const std::string bad = try_install();
    if (!bad.empty()) {
        {
            std::lock_guard<std::mutex> lock(mx_);
            error_ = bad;
        }
        state_ = State::Failed;
    }
    return bad;
}

std::string Updater::try_install() {
    std::string tmp;
    {
        std::lock_guard<std::mutex> lock(mx_);
        tmp = zip_path_;
    }
    if (state_ != State::Ready || tmp.empty()) return "нема чого встановлювати";

    const std::string self = appimage_path();
    if (self.empty()) return "програма запущена не як AppImage";

    // rename поверх себе — атомарний: або старий файл, або новий, третього
    // стану немає. Наш процес далі працює зі свого, уже відкритого inode.
    if (rename(tmp.c_str(), self.c_str()) != 0)
        return std::string("не вдалося підмінити файл: ") + strerror(errno);

    // Перезапуск: execv замінює процес, тож нічого чекати не треба. Аргументи
    // не передаємо — програма й так запускається без ключів.
    char* argv[2] = {const_cast<char*>(self.c_str()), nullptr};
    execv(self.c_str(), argv);
    // Сюди потрапляємо, лише якщо execv не вдався. Файл уже новий, тож досить
    // сказати людині перезапустити вручну.
    return "оновлено, але перезапустити не вийшло — закрийте й відкрийте програму";
}

int Updater::cleanup_downloads(const std::string& keep) {
    (void)keep;
    // Під Windows тут прибиралися двохсотмегабайтні архіви з %TEMP%. Тут
    // прибирати нема чого: єдиний тимчасовий файл лежить поруч із AppImage і
    // зникає тим самим rename, що й ставить оновлення.
    return 0;
}

}  // namespace hominka

#endif  // !_WIN32
