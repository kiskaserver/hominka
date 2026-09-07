#include "update/release.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include "monocypher-ed25519.h"
}

namespace hominka {

namespace {

using json = nlohmann::json;

// Базова адреса оновлень. Змінюється разом із доменом, тому окремою сталою.
const char* kUpdateBase = "https://update.svitix.com/hominka/";

// Публічні ключі, чиїм підписам віримо (base64, Ed25519). Приватна половина —
// у того, хто випускає, і на сервері її немає ніколи.
//
// Список, а не один ключ: так можна ввести новий ключ випуском, який знає
// обидва, і лише потім прибрати старий.
const char* kReleaseKeys[] = {
    "Hj9RXkr1ACooavzVQ3qbqhZ+FaqgsnGgoRM5ItMYcPo=",
};

// Для якої системи шукати файл у маніфесті. Випуск один, файлів у ньому може
// бути кілька: Windows і Linux оновлюються з того самого каналу, але качають
// різні архіви.
#ifdef _WIN32
const char* kPlatform = "win64";
#else
const char* kPlatform = "linux64";
#endif

bool unb64(const std::string& text, std::vector<unsigned char>* out) {
    static const char* kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val = 0, bits = 0;
    out->clear();
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const char* p = strchr(kAlpha, c);
        if (!p || c == 0) return false;
        val = (val << 6) | (int)(p - kAlpha);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back((unsigned char)((val >> bits) & 0xFF));
        }
    }
    return true;
}

std::string str_of(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return "";
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// Файли випуску: нові маніфести мають «files», старі — лише «file» (там завжди
// був Windows). Читаємо обидва, інакше вже встановлені програми перестали б
// оновлюватися.
std::vector<json> files_of(const json& manifest) {
    std::vector<json> out;
    auto files = manifest.find("files");
    if (files != manifest.end() && files->is_array()) {
        for (const auto& f : *files) if (f.is_object()) out.push_back(f);
    }
    if (out.empty()) {
        auto one = manifest.find("file");
        if (one != manifest.end() && one->is_object()) out.push_back(*one);
    }
    return out;
}

}  // namespace

std::vector<int> parse_version(const std::string& s) {
    std::vector<int> out;
    const std::string head = s.substr(0, s.find('+'));
    size_t i = 0;
    while (i < head.size() && out.size() < 3) {
        if (head[i] < '0' || head[i] > '9') { ++i; continue; }
        int n = 0;
        while (i < head.size() && head[i] >= '0' && head[i] <= '9') {
            n = n * 10 + (head[i] - '0');
            ++i;
        }
        out.push_back(n);
    }
    while (out.size() < 3) out.push_back(0);
    return out;
}

bool is_newer(const std::string& candidate, const std::string& current) {
    return parse_version(candidate) > parse_version(current);
}

std::string channel_label(const std::string& channel) {
    if (channel == "stable") return "Стабільна";
    if (channel == "beta") return "Бета";
    if (channel == "dev") return "Тестова";
    return channel;
}

std::string kind_label(const std::string& kind) {
    if (kind == "major") return "велике оновлення";
    if (kind == "minor") return "нові можливості";
    if (kind == "patch") return "виправлення";
    if (kind == "hotfix") return "термінове виправлення";
    return kind.empty() ? "оновлення" : kind;
}

std::string release_payload(const json& manifest) {
    std::vector<json> files = files_of(manifest);
    // Порядок файлів — за назвою системи, і саме тому, що в JSON порядок
    // ключів нічого не гарантує: підписувач і перевіряльник мусять скласти
    // однаковий рядок незалежно від того, як маніфест зберегли.
    std::sort(files.begin(), files.end(), [](const json& a, const json& b) {
        return str_of(a, "platform") < str_of(b, "platform");
    });

    std::string product = str_of(manifest, "product");
    if (product.empty()) product = "hominka";

    std::string out = product;
    out += "\n" + str_of(manifest, "channel");
    out += "\n" + str_of(manifest, "version");
    for (const json& f : files) {
        out += "\n" + str_of(f, "platform");
        out += "\n" + str_of(f, "url");
        const std::string size = str_of(f, "size");
        out += "\n" + (size.empty() ? std::string("0") : size);
        out += "\n" + lower(str_of(f, "sha256"));
    }
    return out;
}

std::string verify_signature(const json& manifest) {
    const std::string sig64 = str_of(manifest, "signature");
    if (sig64.empty()) return "випуск без підпису — не встановлюємо";

    std::vector<unsigned char> sig;
    if (!unb64(sig64, &sig) || sig.size() != 64) return "підпис випуску пошкоджено";

    const std::string payload = release_payload(manifest);
    for (const char* key64 : kReleaseKeys) {
        std::vector<unsigned char> key;
        if (!unb64(key64, &key) || key.size() != 32) continue;
        if (crypto_ed25519_check(sig.data(), key.data(),
                                 (const unsigned char*)payload.data(), payload.size()) == 0)
            return "";
    }
    return "підпис випуску не сходиться — файл або маніфест підмінено";
}

Release parse_manifest(const std::string& raw, const std::string& channel,
                       std::string* error) {
    Release rel;
    json data;
    try {
        data = json::parse(raw);
    } catch (const std::exception& e) {
        *error = std::string("маніфест не розібрався: ") + e.what();
        return rel;
    }
    if (!data.is_object()) { *error = "маніфест не є об'єктом"; return rel; }

    json file;
    for (const json& f : files_of(data)) {
        std::string p = str_of(f, "platform");
        if (p.empty()) p = "win64";        // у старих маніфестах був лише Windows
        if (p == kPlatform) { file = f; break; }
    }
    if (file.is_null()) {
        *error = std::string("для цієї системи (") + kPlatform + ") випуску немає";
        return rel;
    }

    rel.channel = str_of(data, "channel");
    if (rel.channel.empty()) rel.channel = channel;
    rel.version = str_of(data, "version");
    rel.kind = str_of(data, "kind");
    rel.notes = str_of(data, "notes");
    rel.warning = str_of(data, "warning");
    rel.released_at = str_of(data, "releasedAt");
    auto m = data.find("mandatory");
    rel.mandatory = m != data.end() && m->is_boolean() && m->get<bool>();
    rel.url = str_of(file, "url");
    rel.sha256 = lower(str_of(file, "sha256"));
    rel.size = file.contains("size") && file["size"].is_number()
                   ? file["size"].get<long long>()
                   : 0;

    if (!rel.ok()) { *error = "маніфест без версії або без файлу"; return rel; }

    // Підпис перевіряємо ДО того, як хтось устигне щось завантажити: відмовити
    // треба на етапі «що нам пропонують», а не «що ми вже скачали».
    const std::string bad = verify_signature(data);
    if (!bad.empty()) { *error = bad; return Release(); }

    // Качати будемо тільки з нашого домену: маніфест теж приїхав із мережі, і
    // посилання «кудись іще» — привід зупинитися, а не качати.
    if (rel.url.compare(0, strlen(kUpdateBase), kUpdateBase) != 0) {
        *error = std::string("посилання на файл поза ") + kUpdateBase;
        return Release();
    }
    error->clear();
    return rel;
}

}  // namespace hominka
