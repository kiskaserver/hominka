#include "update/update_view.h"

#include <cstdio>

#include "core/version.h"

namespace hominka {

// Стан оновлювача → те, що бачить людина. Складаємо тут, бо панель про
// Windows нічого не знає й знати не повинна.
UpdateView update_view(const Updater& up) {
    UpdateView v;
    v.supported = true;
    const Release rel = up.release();

    // Опис випуску показуємо там, де є що описувати: коли оновлення пропонують
    // або вже завантажили. У решті станів це просто чужа новина.
    const bool describe = up.state() == Updater::State::Available ||
                          up.state() == Updater::State::Downloading ||
                          up.state() == Updater::State::Ready;
    if (describe && !rel.version.empty()) {
        v.title = channel_label(rel.channel) + " " + rel.version + " · " +
                  kind_label(rel.kind);
        v.notes = rel.notes;
        v.warning = rel.warning;
        if (rel.size > 0) {
            char buf[32];
            snprintf(buf, sizeof buf, "%.0f МБ", (double)rel.size / 1e6);
            v.size = buf;
        }
        v.mandatory = rel.mandatory;
    }

    switch (up.state()) {
    case Updater::State::Idle:
        v.status = "Версія " HOMINKA_VERSION ".";
        v.can_check = true;
        break;
    case Updater::State::Checking:
        v.status = "Питаю сервер оновлень…";
        break;
    case Updater::State::UpToDate:
        v.status = "У вас найсвіжіша версія (" HOMINKA_VERSION ").";
        v.can_check = true;
        break;
    case Updater::State::Available:
        v.status = "Є що поставити.";
        v.available = true;
        v.can_download = true;
        v.can_check = true;
        break;
    case Updater::State::Downloading:
        v.status = "Завантажую " + rel.version + "…";
        v.percent = up.percent();
        break;
    case Updater::State::Ready:
        v.status = "Завантажено й перевірено. Програма закриється й відкриється вже "
                   "оновленою.";
        v.can_install = true;
        break;
    case Updater::State::Failed:
        v.status = "Не вийшло: " + up.error();
        v.can_check = true;
        break;
    }
    return v;
}

}  // namespace hominka
