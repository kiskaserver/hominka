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
# Заставка: перенос рядка прогресу і СПРАВЖНЯ смуга.
#
# PyInstaller уміє показати лише картинку та один рядок тексту. Смуга на
# картинці була намальованою — тобто брехнею: вона не рухалася й нічого не
# означала. Тут вона стає справжньою, і коштує це двох вставок у шаблон Tcl:
#
#   1. `-width` для тексту: bootloader пише туди повні шляхи файлів, і довгий
#      шлях їхав за край заставки й далі за екран. В API Splash цієї опції
#      немає, тому дописуємо. Числа літеральні: шаблон потім проходить через
#      %-форматування, і зайвий %d його зламав би.
#   2. прямокутник поверх картинки + лічильник у canvas_text_update. Цю
#      процедуру Tk кличе на КОЖНУ зміну тексту, а bootloader змінює його на
#      кожен розпакований файл — отже, це і є наш крок прогресу. Скільки всього
#      файлів, ми знаємо тут, під час збірки (див. _steps нижче).
#
# Повідомлення від самої програми йдуть із префіксом «NN|» — це прямий наказ
# «постав смугу на NN %» (див. hominka/splash.py). Так останні кроки запуску,
# яких bootloader уже не бачить, теж видно.
from PyInstaller.building import splash_templates as _splash_tpl  # noqa: E402

_steps = max(1, len(a.binaries) + len(a.datas))

if '-width' not in _splash_tpl.splash_canvas_text:
    _splash_tpl.splash_canvas_text = _splash_tpl.splash_canvas_text.replace(
        '-anchor sw', '-anchor sw \\\n        -width 612')

if 'pyi_progress' not in _splash_tpl.splash_canvas_setup:
    _splash_tpl.splash_canvas_setup += """
# Смуга прогресу: доріжка і заповнення поверх картинки.
set pyi_progress_done 0
set pyi_progress_shown 0
set pyi_progress_total %d
set pyi_progress_x0 54
set pyi_progress_x1 666
.root.canvas create rectangle $pyi_progress_x0 232 $pyi_progress_x1 239 \\
    -fill #241d2e -outline "" -tag pyi_progress_track
.root.canvas create rectangle $pyi_progress_x0 232 $pyi_progress_x0 239 \\
    -fill #a855f7 -outline "" -tag pyi_progress
""" % _steps

if 'pyi_progress' not in _splash_tpl.image_script:
    _splash_tpl.image_script = _splash_tpl.image_script.replace(
        """    upvar $_var var
    $canvas itemconfigure $tag -text $var""",
        """    upvar $_var var
    global pyi_progress_done pyi_progress_total pyi_progress_x0 pyi_progress_x1
    global pyi_progress_shown

    # Повідомлення від програми: «NN|текст» — поставити смугу рівно на NN %.
    set shown $var
    set bar -1
    if {[regexp {^([0-9]+)\\|(.*)$} $var - pct rest]} {
        set bar $pct
        set shown $rest
    } else {
        # Рядок від bootloader-а: ще один розпакований файл.
        #
        # Розпакування — це перші 85 % смуги. Решту віддано запуску Qt, якого
        # bootloader уже не бачить: інакше смуга впиралася б у край за пару
        # секунд до вікна і виглядала б як зависання.
        incr pyi_progress_done
        set bar [expr {int(85.0 * $pyi_progress_done / $pyi_progress_total)}]
        if {$bar > 85} { set bar 85 }
        # Довгий шлях файлу нікому нічого не каже — показуємо лише імʼя.
        set shown [file tail $shown]
    }
    # Смуга не їде назад: підрахунок кроків приблизний, і стрибок ліворуч
    # читається як помилка, навіть коли це просто уточнення.
    if {$bar >= 0 && $bar < $pyi_progress_shown} { set bar $pyi_progress_shown }
    set pyi_progress_shown $bar
    $canvas itemconfigure $tag -text $shown
    if {$bar >= 0} {
        set w [expr {$pyi_progress_x0 + ($pyi_progress_x1 - $pyi_progress_x0) * $bar / 100.0}]
        $canvas coords pyi_progress $pyi_progress_x0 232 $w 239
    }""")

splash = Splash(
    'splash.png',
    binaries=a.binaries,
    datas=a.datas,
    text_pos=(54, 322),
    text_size=10,
    text_color='#9a9490',
    text_default='Готуюсь до запуску…',
    max_img_size=(760, 400),
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
