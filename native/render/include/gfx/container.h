// Який контейнер litehtml брати на цій системі.
//
// Обидва — і віконний, і Linux-овий — мають однакові імена методів, тож
// стрічці досить одного псевдоніма. Розходження між ними було б помітно
// одразу: те, що є в одному й немає в іншому, просто не зібралося б.
#pragma once

#ifdef _WIN32
#include "gfx/container_d2d.h"
#else
#include "gfx/container_bl.h"
#endif

namespace hominka {

#ifdef _WIN32
using Container = ContainerD2D;
#else
using Container = ContainerBL;
#endif

}  // namespace hominka
