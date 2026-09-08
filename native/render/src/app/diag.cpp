#include "app/diag.h"

#include <cstdio>
#include <string>

#include "app/offscreen.h"
#include "core/version.h"
#include "ui/csslint.h"
#include "update/updater.h"

namespace hominka {

int css_check(const wchar_t* path) {
    const std::string text = read_file(path);
    if (text.empty()) {
        fprintf(stderr, "порожній або не прочитався файл\n");
        return 2;
    }
    for (const CssProblem& p : validate_css(text))
        printf("error %d %s\n", p.line, p.text.c_str());
    for (const CssProblem& p : lint_css(text))
        printf("warn %d %s\n", p.line, p.text.c_str());
    return 0;
}

// Перевірка оновлення з командного рядка. Саме тут видно, чи сходиться підпис
// зі СПРАВЖНІМ маніфестом на сервері: формат того, що підписується, мусить
// збігатися з Python до байта, і перевірити це можна лише проти живого випуску.
int update_check(const char* channel, const char* pretend, bool fetch, bool put) {
    Updater up;
    const char* current = pretend && *pretend ? pretend : HOMINKA_VERSION;
    up.check(channel, current, "");
    for (int i = 0; i < 300 && up.state() == Updater::State::Checking; ++i) Sleep(100);

    if (up.state() == Updater::State::UpToDate) {
        printf("оновлень немає (у нас %s)\n", current);
        return 0;
    }
    if (up.state() != Updater::State::Available) {
        fprintf(stderr, "не вийшло: %s\n", up.error().c_str());
        return 1;
    }

    const Release r = up.release();
    printf("є оновлення: %s %s (%s), %lld байт\n", channel_label(r.channel).c_str(),
           r.version.c_str(), kind_label(r.kind).c_str(), r.size);
    printf("  файл: %s\n", r.url.c_str());
    printf("  sha256: %s\n", r.sha256.c_str());
    printf("  підпис перевірено\n");
    if (!fetch) return 0;

    // Завантаження — окремим кроком і лише на прохання: це двісті мегабайтів.
    // Але саме тут перевіряється те, чого інакше не побачиш: чи справді ми
    // тягнемо потоком, чи сходиться сума й чи не бреше поступ.
    printf("качаю…\n");
    fflush(stdout);
    up.download();
    int last = -1;
    while (up.state() == Updater::State::Downloading) {
        const int p = up.percent();
        if (p / 10 != last / 10) {
            last = p;
            printf("  %d%%\n", p);
            fflush(stdout);
        }
        Sleep(200);
    }
    if (up.state() != Updater::State::Ready) {
        fprintf(stderr, "не завантажилося: %s\n", up.error().c_str());
        return 1;
    }
    printf("завантажено, сума збіглася\n");
    if (!put) {
        Updater::cleanup_downloads();
        return 0;
    }

    // І власне встановлення. Воно тут не заради зручності: підмінник —
    // єдина частина оновлення, якої не видно ні з коду, ні з журналу, доки
    // вона не спрацює. Саме він і був зламаний у 3.0.0–3.0.2, а помітили це
    // лише тому, що людина натиснула кнопку й нічого не сталося. Тепер те
    // саме робиться з командного рядка — і причину видно одразу.
    const std::string bad = up.install();
    if (!bad.empty()) {
        fprintf(stderr, "не встановилося: %s\n", bad.c_str());
        return 1;
    }
    printf("підмінника запущено; виходжу, щоб він переписав файли\n");
    return 0;
}

// Перевірка маніфесту з файлу: чи сходиться підпис і що саме в ньому.
//
// Потрібне двічі. По-перше, це відповідь на «чому не оновлюється» — видно, чи
// річ у підписі. По-друге, саме так ганяються підміни: беремо СПРАВЖНІЙ
// маніфест, міняємо в ньому по одному полю й дивимося, що ловиться
// (release_smoke.py).
int verify_release(const wchar_t* path) {
    const std::string text = read_file(path);
    if (text.empty()) {
        fprintf(stderr, "порожній або не прочитався файл\n");
        return 2;
    }
    std::string err;
    const Release rel = parse_manifest(text, "stable", &err);
    if (!err.empty()) {
        printf("ні: %s\n", err.c_str());
        return 1;
    }
    printf("так: %s %s (%s)\n", rel.channel.c_str(), rel.version.c_str(),
           rel.kind.c_str());
    return 0;
}

// Найпростіша перевірка: розібрати тривіальну сторінку НАШИМ контейнером.
// Ділить навпіл: якщо падає і тут — річ у контейнері, а не в розмітці чату.

}  // namespace hominka
