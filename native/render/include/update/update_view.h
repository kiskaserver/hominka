// Стан оновлювача очима панелі налаштувань.
//
// Окремим файлом, бо потрібен обом системам, а живе між двома світами: знає і
// про Updater, і про те, що показують людині. Панель при цьому лишається
// такою ж переносимою, як решта малювання, — вона про оновлювач не знає.
#pragma once

#include "ui/settings_ui.h"
#include "update/updater.h"

namespace hominka {

UpdateView update_view(const Updater& up);

}  // namespace hominka
