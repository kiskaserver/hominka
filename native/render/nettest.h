// Перевірка мережі проти справжніх серверів, спільна для Windows і Linux.
//
// Чому окремим файлом, а не в кожному main: перевіряємо ми ОДНЕ й те саме, і
// два майже однакові шматки коду розійшлися б через тиждень. Тут же видно, що
// «прочитано повідомлень: 0» на обох системах означає рівно одне.
#pragma once

#include <string>

namespace hominka {

// platform — «twitch» | «kick». Друкує повідомлення, доки не мине seconds.
// Повертає код виходу програми: 0 — читали чат, 1 — не під'єдналися.
int nettest(const std::string& platform, const std::string& channel, int seconds);

}  // namespace hominka
