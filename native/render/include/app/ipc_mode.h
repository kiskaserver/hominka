// Режим, у якому програмою керує Hominka на Python: кадри приходять каналом,
// назад ідуть події рамки.
//
// Лишається поруч із самостійним режимом навмисно. Новий шлях уже вміє все, але
// ламати робочий, доки новий не побував у бою, — зарано.
#pragma once

#include <cstdint>
#include <string>

#include "common/dcomp_window.h"
#include "core/feed.h"
#include "core/look.h"
#include "gfx/imgcache.h"
#include "platform/ipc.h"

namespace hominka {

// Стан оверлея, вкладеного в гру. Його задає Hominka — вона ж і вкладає DLL.
struct InjectState {
    bool on = false;
    uint32_t pid = 0;          // малювати лише в цьому процесі (0 = у будь-якому)
    uint32_t opacity = 235;
    bool hide_obs = false;
};

// Застосовує один кадр із каналу. true — картинку варто перемалювати.
bool apply_frame(const IpcFrame& fr, Feed* feed, ImageCache* images, Look* look,
                 InjectState* inj, int* want_w, int* want_h, bool* enabled, bool* bye,
                 std::string* shot_path);

// Розповідає Hominka, що людина зробила у вікні.
void report_chrome(IpcServer* ipc, const ChromeEvents& ev, const Look& look,
                   DCompWindow& win, bool* user_sizing, Feed* feed);

}  // namespace hominka
