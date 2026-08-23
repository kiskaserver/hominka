# -*- mode: python ; coding: utf-8 -*-
#
# Збірка ОДНИМ файлом: усе, що раніше лежало поруч у теці _internal, тепер
# усередині Hominka.exe.
#
# Ціна відома і свідома: під час запуску bootloader розпаковує ~300 МБ у
# тимчасову теку, тож вікно з'являється не миттєво (близько 8 с проти 1-2 с
# у збірці текою). Тому тут же splash — заставка, яку видно одразу, інакше
# запуск виглядає як «клікнув і нічого не сталося».
#
# Мовні файли Chromium ріжемо: їх понад сотня, а Qt однаково візьме англійську,
# якщо потрібної немає. Це найдешевші мегабайти, які можна не розпаковувати
# при кожному старті.

import sys

# Той самий spec збирає і Windows, і Linux: різняться вони лише дрібницями —
# іконка та ресурс версії існують тільки у Windows-збірці.
IS_WIN = sys.platform == "win32"

KEEP_LOCALES = ("en-US", "uk", "ru")


def keep(entry) -> bool:
    dest = entry[0].replace("\\", "/")
    if "qtwebengine_locales/" in dest:
        return any(dest.endswith("/%s.pak" % loc) for loc in KEEP_LOCALES)
    if "PySide6/translations/" in dest and dest.endswith(".qm"):
        return any(("_%s." % loc.split("-")[0]) in dest for loc in KEEP_LOCALES)
    return True


a = Analysis(
    ['chat_overlay.py'],
    pathex=[],
    binaries=[],
    datas=[('hominka.ico', '.'), ('hominka.png', '.'), ('splash.png', '.')],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    # Qt3D/Quick3D/Charts тощо тягне за собою хук PySide6 «про всяк випадок»;
    # ми з них не використовуємо нічого, а розпаковувати їх доводиться щоразу.
    excludes=[
        'tkinter', 'PySide6.QtQuick3D', 'PySide6.Qt3DCore', 'PySide6.Qt3DRender',
        'PySide6.QtCharts', 'PySide6.QtDataVisualization', 'PySide6.QtMultimedia',
        'PySide6.QtDesigner', 'PySide6.QtTest', 'PySide6.QtSql', 'PySide6.QtBluetooth',
    ],
    noarchive=False,
    optimize=0,
)
a.datas = [d for d in a.datas if keep(d)]

pyz = PYZ(a.pure)

# Заставка з рядком прогресу.
#
# text_pos — це не прикраса: саме він вмикає текст, у який bootloader пише, що
# зараз розпаковує. Без нього людина дивиться на нерухому картинку і не знає,
# чи взагалі щось відбувається. Перед появою вікна текст замінюємо своїм
# (див. close_splash / splash_text у chat_overlay).
splash = Splash(
    'splash.png',
    binaries=a.binaries,
    datas=a.datas,
    text_pos=(36, 192),
    text_size=9,
    text_color='#9a9490',
    text_default='Готуюсь до запуску…',
    minify_script=True,
    always_on_top=True,
)

exe = EXE(
    pyz,
    a.scripts,
    splash,
    splash.binaries,
    a.binaries,
    a.datas,
    [],
    name='Hominka',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    version='version_info.txt' if IS_WIN else None,
    icon=['hominka.ico'] if IS_WIN else ['hominka.png'],
)
