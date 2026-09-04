"""Реєстрація Vulkan-шару чату в системі.

На відміну від інжектора (inject.py), Vulkan не можна «вкласти» після старту —
гра ініціалізує Vulkan одразу при запуску, і наш код має бути на місці ЩЕ до
того. Тому чат у Vulkan-грі показує не вклад-DLL, а *неявний шар* Vulkan-loader'а
(hominka-vklayer-{x64,x86}.dll): loader сам вантажить його в будь-яку Vulkan-гру,
якщо шар зареєстровано у реєстрі.

Реєстрація — це JSON-маніфест поруч із DLL плюс запис у
HKCU\\Software\\Khronos\\Vulkan\\ImplicitLayers (значення = шлях до JSON,
дані DWORD 0 = увімкнено). Робимо це, лише поки ввімкнено «чат у грі», і
знімаємо, коли вимкнено чи при виході: щоб шар не вантажився в чужі Vulkan-
застосунки, коли він нам не потрібен. Сам шар усе одно нічого не малює, поки
продюсер не поставить у спільній памʼяті enabled=1 — але тримати його
зареєстрованим постійно ні до чого.

64- і 32-бітну гілки реєстру пишемо окремо (KEY_WOW64_{64,32}KEY): 64-бітна гра
бачить x64-маніфест, 32-бітна — x86, і кожна вантажить DLL своєї розрядності.
"""

import json
import os
import sys

from .inject import native_dir

IS_WINDOWS = sys.platform == "win32"

# Ключ Vulkan-loader'а для неявних шарів (див. Vulkan-Loader, LoaderLayerInterface).
_REG_SUBKEY = r"Software\Khronos\Vulkan\ImplicitLayers"

# Пара (DLL, JSON) для кожної розрядності. Ключі — прапорці winreg для гілки
# реєстру тієї ж розрядності, що й гра.
_ARCHES = ("x64", "x86")


def _manifest_json(arch: str) -> str:
    """Текст JSON-маніфесту шару. library_path — відносний до самого JSON, тож
    JSON має лежати поруч із DLL."""
    return json.dumps({
        "file_format_version": "1.2.0",
        "layer": {
            "name": "VK_LAYER_hominka_overlay",
            "type": "GLOBAL",
            # Через json.dumps зворотні слеші екрануються самі — не буде
            # зіпсованого «.\\h…», як при ручному написанні.
            "library_path": ".\\hominka-vklayer-%s.dll" % arch,
            "api_version": "1.3.280",
            "implementation_version": "1",
            "description": "Hominka in-game chat overlay",
            "functions": {
                "vkNegotiateLoaderLayerInterfaceVersion": "HominkaVkNegotiate"
            },
            "disable_environment": {
                "DISABLE_HOMINKA_VK_LAYER": "1"
            }
        }
    }, indent=4)


def _paths(arch: str):
    """(шлях до DLL, шлях до JSON) для розрядності."""
    d = native_dir()
    return (os.path.join(d, "hominka-vklayer-%s.dll" % arch),
            os.path.join(d, "hominka-vklayer-%s.json" % arch))


def available() -> bool:
    """Чи є хоч одна DLL шару поруч — інакше й реєструвати нема чого."""
    if not IS_WINDOWS:
        return False
    return any(os.path.isfile(_paths(a)[0]) for a in _ARCHES)


def _wow_flag(arch: str) -> int:
    import winreg
    # 64-бітна гра читає нативну гілку HKCU, 32-бітна — WOW6432Node.
    return winreg.KEY_WOW64_64KEY if arch == "x64" else winreg.KEY_WOW64_32KEY


def register() -> bool:
    """Кладе JSON-маніфести поруч із DLL і вмикає шар у реєстрі. Тихо повертає
    False, якщо не Windows чи файлів нема — виклик безпечний за будь-яких умов."""
    if not available():
        return False
    import winreg
    ok = False
    for arch in _ARCHES:
        dll, jpath = _paths(arch)
        if not os.path.isfile(dll):
            continue
        try:
            with open(jpath, "w", encoding="utf-8") as f:
                f.write(_manifest_json(arch))
        except OSError:
            continue
        try:
            key = winreg.CreateKeyEx(
                winreg.HKEY_CURRENT_USER, _REG_SUBKEY, 0,
                winreg.KEY_SET_VALUE | _wow_flag(arch))
            try:
                # Ім'я значення = шлях до JSON, дані DWORD 0 = шар увімкнено.
                winreg.SetValueEx(key, jpath, 0, winreg.REG_DWORD, 0)
                ok = True
            finally:
                winreg.CloseKey(key)
        except OSError:
            continue
    return ok


def unregister() -> None:
    """Знімає шар з реєстру (JSON поруч із DLL лишаємо — він нешкідливий і
    знадобиться при наступному вмиканні). Безпечно кликати, навіть якщо нічого
    не було зареєстровано."""
    if not IS_WINDOWS:
        return
    import winreg
    for arch in _ARCHES:
        _, jpath = _paths(arch)
        try:
            key = winreg.OpenKeyEx(
                winreg.HKEY_CURRENT_USER, _REG_SUBKEY, 0,
                winreg.KEY_SET_VALUE | _wow_flag(arch))
        except OSError:
            continue
        try:
            winreg.DeleteValue(key, jpath)
        except OSError:
            pass          # не було нашого значення — то й нема що знімати
        finally:
            winreg.CloseKey(key)
