// Реєстрація Vulkan-шару чату в системі. Порт hominka/vklayer.py.
//
// Чому окремим файлом, а не разом із рештою віконного: тут єдине місце, де ми
// пишемо в реєстр щось, що бачить КОЖНА Vulkan-програма на машині. Такий код
// краще тримати на видноті, а не в кінці великого файлу.
#include "platform/gamewin.h"

#ifdef _WIN32

#include <windows.h>

#include <cstdio>

#include <nlohmann/json.hpp>

namespace hominka {

namespace {

// Ключ Vulkan-loader'а для неявних шарів (див. Vulkan-Loader,
// LoaderLayerInterface).
const char* kVkKey = "Software\\Khronos\\Vulkan\\ImplicitLayers";

// 64-бітна гра читає нативну гілку HKCU, 32-бітна — WOW6432Node. Пишемо в
// обидві: кожна гра вантажить DLL своєї розрядності.
REGSAM wow_flag(const char* arch) {
    return std::string(arch) == "x64" ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
}

std::string json_path(const std::string& dir, const char* arch) {
    return dir + "\\hominka-vklayer-" + arch + ".json";
}

// Текст маніфесту. Складаємо його бібліотекою JSON, а не руками: library_path
// містить зворотні похилі риски, і саме на їх екрануванні тут найлегше
// помилитися — вийшов би маніфест, який loader мовчки пропустить.
//
// library_path відносний до самого JSON, тож JSON має лежати ПОРУЧ із DLL.
std::string manifest(const char* arch) {
    nlohmann::json layer;
    layer["name"] = "VK_LAYER_hominka_overlay";
    layer["type"] = "GLOBAL";
    layer["library_path"] = std::string(".\\hominka-vklayer-") + arch + ".dll";
    layer["api_version"] = "1.3.280";
    layer["implementation_version"] = "1";
    layer["description"] = "Hominka in-game chat overlay";
    layer["functions"]["vkNegotiateLoaderLayerInterfaceVersion"] = "HominkaVkNegotiate";
    layer["disable_environment"]["DISABLE_HOMINKA_VK_LAYER"] = "1";

    nlohmann::json doc;
    doc["file_format_version"] = "1.2.0";
    doc["layer"] = layer;
    return doc.dump(4);
}

}  // namespace

bool vklayer_register() {
    const std::string dir = native_dir();
    bool ok = false;
    for (const char* arch : {"x64", "x86"}) {
        const std::string dll = dir + "\\hominka-vklayer-" + arch + ".dll";
        if (GetFileAttributesA(dll.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

        const std::string json = json_path(dir, arch);
        const std::string text = manifest(arch);
        FILE* f = fopen(json.c_str(), "wb");
        if (!f) continue;
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);

        HKEY key = nullptr;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, kVkKey, 0, nullptr, 0,
                            KEY_SET_VALUE | wow_flag(arch), nullptr, &key,
                            nullptr) != ERROR_SUCCESS)
            continue;
        // Ім'я значення = шлях до JSON, дані DWORD 0 = шар увімкнено.
        const DWORD zero = 0;
        if (RegSetValueExA(key, json.c_str(), 0, REG_DWORD, (const BYTE*)&zero,
                           sizeof zero) == ERROR_SUCCESS)
            ok = true;
        RegCloseKey(key);
    }
    return ok;
}

void vklayer_unregister() {
    const std::string dir = native_dir();
    for (const char* arch : {"x64", "x86"}) {
        HKEY key = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, kVkKey, 0, KEY_SET_VALUE | wow_flag(arch),
                          &key) != ERROR_SUCCESS)
            continue;
        // Сам JSON поруч із DLL лишаємо: він нешкідливий і знадобиться при
        // наступному вмиканні.
        RegDeleteValueA(key, json_path(dir, arch).c_str());
        RegCloseKey(key);
    }
}

}  // namespace hominka

#endif  // _WIN32
