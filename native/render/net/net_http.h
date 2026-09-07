// Прості запити HTTP: значки, емоути, довідка про канал.
//
// Чому не WebSocket і чому взагалі окремо. Чат приходить потоком (це
// WebSocket), а от «дай список емоутів цього каналу» — звичайний разовий
// запит. Обидва вміє та сама IXWebSocket, але користуються ними по-різному, і
// змішувати в одному місці нема сенсу.
//
// Мережа тут СИНХРОННА, і це навмисно: усі виклики роблять окремі потоки
// (набори емоутів тягнуться при під'єднанні до каналу, значки — раз на запуск).
// Асинхронність усередині додала б стан, який нікому не потрібен.
#pragma once

#include <map>
#include <string>

namespace hominka {

// Кому довіряти при TLS. Потрібне і запитам, і WebSocket, тож живе тут —
// в одному місці на весь мережевий шар.
std::string ca_bundle_path();

struct HttpResult {
    int status = 0;              // 0 — не дійшло взагалі
    std::string body;
    std::string error;           // причина, якщо status == 0
    bool ok() const { return status >= 200 && status < 300; }
};

// GET із таймаутом у секундах. Заголовок User-Agent ставиться завжди: без
// нього Kick і частина CDN відповідають 403 — вони фільтрують за ним.
HttpResult http_get(const std::string& url, int timeout_sec = 15);

// Те саме, але з власними заголовками. Потрібне YouTube: без «Accept-Language»
// і згоди на куки він віддає сторінку іншою мовою й з іншою розміткою.
HttpResult http_get(const std::string& url,
                    const std::map<std::string, std::string>& headers,
                    int timeout_sec = 15);

// Те саме, але одразу розібране як JSON. Порожній об'єкт, якщо не вийшло, —
// виклик має право не перевіряти помилку окремо, бо реакція однакова:
// лишитися без емоутів, а не впасти.
std::string http_get_json(const std::string& url, int timeout_sec = 15);

// POST із власними заголовками. Потрібен рівно одному місцю — значкам Twitch,
// які тепер віддає лише GraphQL, — але саме тому й тут, а не всередині нього.
HttpResult http_post(const std::string& url, const std::string& body,
                     const std::map<std::string, std::string>& headers,
                     int timeout_sec = 15);

}  // namespace hominka
