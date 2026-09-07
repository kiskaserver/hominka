// Маніфест випуску: розбір, вибір файлу під систему, перевірка підпису.
//
// Навіщо підпис. Досі цілісність оновлення трималася на двох речах: HTTPS до
// update.svitix.com і sha256 у маніфесті, що приїхав ЗВІДТИ Ж. Тобто на одному
// джерелі: хто дістанеться цього домену, той підмінить і файл, і його
// контрольну суму, і програма слухняно поставить чужий код на чужу машину.
// Підпис розриває це коло: приватний ключ лежить не на сервері, а в того, хто
// випускає.
//
// Підписуємо не маніфест цілком, а СУТЬ випуску (release_payload): продукт,
// канал, версію і по кожному файлу — систему, адресу, розмір і sha256. Історія
// та дата в підпис не входять: вони переписуються при кожному наступному
// випуску каналу, і підпис ламався б ні через що.
//
// Формат того, що підписується, мусить збігатися з hominka/signing.py ДО
// БАЙТА: маніфести на сервері підписані вже давно, і найменша розбіжність
// означала б, що жодне наявне оновлення більше не встановлюється.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace hominka {

struct Release {
    std::string channel;
    std::string version;
    std::string kind;          // major | minor | patch | hotfix
    std::string notes;
    std::string warning;
    std::string released_at;
    bool mandatory = false;
    std::string url;
    std::string sha256;
    long long size = 0;

    bool ok() const { return !version.empty() && !url.empty(); }
};

// «1.10.2» → (1, 10, 2). Нецифрові хвости («1.2.0-beta») відкидаємо: канал і
// так відомий, а порівнювати треба саме числа.
std::vector<int> parse_version(const std::string& s);
bool is_newer(const std::string& candidate, const std::string& current);

// Як показувати канал і вид оновлення.
std::string channel_label(const std::string& channel);
std::string kind_label(const std::string& kind);

// Те, що підписано. Рівно ті самі байти, що будує signing.release_payload.
std::string release_payload(const nlohmann::json& manifest);

// Пропускає лише те, що підписано нашим ключем. Повертає порожній рядок, якщо
// все гаразд, або причину відмови.
//
// Відмовляємо жорстко: сумнівне оновлення краще не поставити зовсім, ніж
// поставити «про всяк випадок». Причина видна в налаштуваннях, тож мовчазного
// провалу не буде.
std::string verify_signature(const nlohmann::json& manifest);

// Розбирає маніфест каналу. error порожній — усе гаразд.
Release parse_manifest(const std::string& raw, const std::string& channel,
                       std::string* error);

}  // namespace hominka
