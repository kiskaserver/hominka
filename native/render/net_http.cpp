#include "net_http.h"

#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>

#include <cstdio>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif


namespace hominka {

namespace {

// Той самий, що шле браузер. Kick без нього відповідає 403: він відсіює
// запити, які «не схожі на людину». Це не обхід захисту, а звичайна ввічливість
// — саме так робить і сторінка самого Kick.
const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36";

// Під Windows сокети треба підняти явно (WSAStartup), інакше будь-який запит
// падає з «Cannot connect». Робимо це тут, а не покладаємося на джерела чату:
// значки й емоути тягнуться й тоді, коли жодного сокета ще не відкривали, і
// саме на цьому YouTube мовчки лишався без сторінки.
void ensure_net() {
    static std::once_flag once;
    std::call_once(once, [] { ix::initNetSystem(); });
}

}  // namespace

// Кому довіряти. Повертає або ШЛЯХ до файлу, або самі сертифікати текстом —
// IXWebSocket приймає обидва (рядок, що починається з «-----BEGIN», він
// вважає вмістом, а не шляхом).
//
// mbedTLS, на відміну від системних бібліотек, не знає жодного сховища: йому
// треба дати корені явно. Без них рукостискання не відбувається взагалі, а
// помилка виглядає як «не вдалося під'єднатися» — тобто вказує кудись не туди.
// На це вже пішла година.
//
// Перевірку НЕ вимикаємо. Спокуса є (чат публічний, що там красти), але через
// це з'єднання чужий у мережі зможе підмінити вміст чату, а показуємо ми його
// на весь екран.
std::string ca_bundle_path() {
#ifdef _WIN32
    // Беремо сховище самої Windows, а не власний cacert.pem поруч із програмою.
    // Так ми, по-перше, не тягнемо файл, який через рік протухне й почне тихо
    // ламати з'єднання, а по-друге — поважаємо корені, які додав користувач
    // або його організація.
    static std::string pem;
    if (!pem.empty()) return pem;

    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return "SYSTEM";
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr) {
        DWORD chars = 0;
        if (!CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded,
                                  CRYPT_STRING_BASE64HEADER, nullptr, &chars))
            continue;
        std::string one(chars, '\0');
        if (CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded,
                                 CRYPT_STRING_BASE64HEADER, &one[0], &chars)) {
            one.resize(chars);
            pem += one;
        }
    }
    CertCloseStore(store, 0);
    // Порожньо — краще чесно віддати «SYSTEM» і дати бібліотеці сказати своє,
    // ніж мовчки лишитися без перевірки.
    return pem.empty() ? std::string("SYSTEM") : pem;
#else
    static const char* kCandidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",   // Debian, Ubuntu, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",     // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",               // openSUSE
        "/etc/ssl/cert.pem",                    // Alpine, macOS
    };
    for (const char* p : kCandidates) {
        FILE* f = fopen(p, "rb");
        if (f) { fclose(f); return p; }
    }
    return "SYSTEM";
#endif
}

HttpResult http_get(const std::string& url, int timeout_sec) {
    return http_get(url, {}, timeout_sec);
}

HttpResult http_get(const std::string& url,
                    const std::map<std::string, std::string>& headers, int timeout_sec) {
    HttpResult out;
    ensure_net();

    ix::HttpClient client(/*async=*/false);
    ix::SocketTLSOptions tls;
    tls.caFile = ca_bundle_path();
    client.setTLSOptions(tls);

    auto args = client.createRequest(url, ix::HttpClient::kGet);
    args->connectTimeout = timeout_sec;
    args->transferTimeout = timeout_sec;
    args->followRedirects = true;
    args->maxRedirects = 5;
    args->extraHeaders["User-Agent"] = kUserAgent;
    args->extraHeaders["Accept"] = "application/json";
    for (const auto& kv : headers) args->extraHeaders[kv.first] = kv.second;

    auto resp = client.get(url, args);
    if (!resp) {
        out.error = "немає відповіді";
        return out;
    }
    out.status = resp->statusCode;
    out.body = resp->body;
    if (out.status == 0) out.error = resp->errorMsg;
    return out;
}

HttpResult http_post(const std::string& url, const std::string& body,
                     const std::map<std::string, std::string>& headers, int timeout_sec) {
    HttpResult out;
    ensure_net();

    ix::HttpClient client(/*async=*/false);
    ix::SocketTLSOptions tls;
    tls.caFile = ca_bundle_path();
    client.setTLSOptions(tls);

    auto args = client.createRequest(url, ix::HttpClient::kPost);
    args->connectTimeout = timeout_sec;
    args->transferTimeout = timeout_sec;
    args->extraHeaders["User-Agent"] = kUserAgent;
    args->extraHeaders["Content-Type"] = "application/json";
    for (const auto& kv : headers) args->extraHeaders[kv.first] = kv.second;

    auto resp = client.post(url, body, args);
    if (!resp) {
        out.error = "немає відповіді";
        return out;
    }
    out.status = resp->statusCode;
    out.body = resp->body;
    if (out.status == 0) out.error = resp->errorMsg;
    return out;
}

std::string http_get_json(const std::string& url, int timeout_sec) {
    const HttpResult r = http_get(url, timeout_sec);
    if (!r.ok() || r.body.empty()) return "{}";
    return r.body;
}

}  // namespace hominka
