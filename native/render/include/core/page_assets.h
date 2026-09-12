// Розмітка й базові стилі чату — дзеркало hominka/feed/page.py.
//
// Чому дзеркало, а не «переписали як зручніше»: класи (.m, .n, .t, .em, .b,
// .bi, .at, .sys, .paid, .money, .re, .ico) і атрибути (data-platform,
// data-kind, data-event) — це ПУБЛІЧНА домовленість. На них написані теми
// користувачів і на них указує довідник у редакторі (hominka/cssui/catalog.py).
// Розійдеться тут — зламаються чужі теми, а не наш код.
//
// Єдина навмисна відмінність від сторінки в браузері: значок площадки йде
// <img class="ico" src="data:image/svg+xml;base64,…"> замість вбудованого
// <svg class="ico">. litehtml не малює SVG як елемент розмітки, зате малює
// його як картинку (див. imgcache.h, nanosvg). Селектор «.ico» — той самий,
// а саме він і описаний у довіднику.
#pragma once

#include <string>

namespace hominka {

// Значки площадок — ті самі логотипи (Simple Icons), що й на сторінці та на
// сайті (web/src/lib/platformIcons.tsx). Тут це цілі SVG-документи (зі
// xmlns), бо підуть у <img>, а не всередину розмітки.
struct PlatformIcon { const char* id; const char* svg; };

static const PlatformIcon PLATFORM_ICONS[] = {
    {"twitch",
     "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' fill='#a970ff'>"
     "<path d='M11.571 4.714h1.715v5.143H11.57zm4.715 0H18v5.143h-1.714zM6 0L1.714 "
     "4.286v15.428h5.143V24l4.286-4.286h3.428L22.286 12V0zm14.571 11.143l-3.428 "
     "3.428h-3.429l-3 3v-3H6.857V1.714h13.714Z'/></svg>"},
    {"kick",
     "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' fill='#53fc18'>"
     "<path d='M1.333 0h8v5.333H12V2.667h2.667V0h8v8H20v2.667h-2.667v2.666H20V16h2.667v8h-8"
     "v-2.667H12v-2.666H9.333V24h-8Z'/></svg>"},
    {"youtube",
     "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' fill='#ff0033'>"
     "<path d='M23.498 6.186a3.016 3.016 0 0 0-2.122-2.136C19.505 3.545 12 3.545 12 "
     "3.545s-7.505 0-9.377.505A3.017 3.017 0 0 0 .502 6.186C0 8.07 0 12 0 12s0 3.93.502 "
     "5.814a3.016 3.016 0 0 0 2.122 2.136c1.871.505 9.376.505 9.376.505s7.505 0 9.377-.505a"
     "3.015 3.015 0 0 0 2.122-2.136C24 15.93 24 12 24 12s0-3.93-.502-5.814zM9.545 15.568V8.432"
     "L15.818 12l-6.273 3.568z'/></svg>"},
};

// Текстові плашки автора — набір і кольори ті самі, що на сторінці й на сайті.
// Малюються тоді, коли справжньої іконки значка немає (badgeIcons).
struct BadgeLabel { const char* id; const char* text; const char* bg; const char* fg; };

static const BadgeLabel BADGE_LABELS[] = {
    {"broadcaster", "HOST",  "#ef4444", "#fff"},
    {"mod",         "MOD",   "#34d399", "#062"},
    {"vip",         "VIP",   "#e879f9", "#fff"},
    {"sub",         "SUB",   "#8b5cf6", "#fff"},
    {"member",      "MEM",   "#10b981", "#fff"},
    {"verified",    "\xE2\x9C\x93", "#38bdf8", "#fff"},   // ✓
    {"staff",       "STAFF", "#71717a", "#fff"},
    {"og",          "OG",    "#f59e0b", "#221"},
    {"artist",      "ART",   "#f472b6", "#fff"},
};

// Частини рядка — той самий список і той самий порядок, що feed.page.PARTS.
// Порядок міняє користувач (⚙ → свій CSS → «Порядок»), програма шле його як
// chatLayout; вимкнена частина приходить з мінусом («-reply»).
static const char* const PART_IDS[] = {"ico", "badges", "reply", "name", "money", "text"};
static const int PART_COUNT = 6;

// Базові стилі — точна копія <style> зі сторінки. Свій CSS користувача
// підставляється НИЖЧЕ (litehtml, як і браузер, віддає перевагу останньому
// правилу тієї ж ваги), тож будь-яке правило перебиває типове без !important,
// а «скинути до типових» — це просто спорожнити користувацький блок.
//
// Чого тут навмисно немає: @keyframes in { … }. litehtml анімацій не вміє, і
// поява рядка робиться нативно (render/list.cpp) — так вона ще й не змушує
// перескладати розкладку 60 разів на секунду.
static const char* const BASE_CSS = R"CSS(
html, body { margin:0; padding:0; background:transparent; }
body {
  font: 20px/1.35 'Segoe UI', system-ui, sans-serif; color:#fff; font-weight:600;
  text-shadow: 0 2px 3px rgba(0,0,0,.95);
}
.m { display:block; }
.ico { width:1em; height:1em; vertical-align:-0.15em; margin-right:.3em; }
.b { display:inline-block; padding:0 .35em; border-radius:.35em; margin-right:.25em;
     font:800 .55em/1.7 'Segoe UI'; vertical-align:.15em; text-shadow:none; }
.bi { height:1.2em; width:auto; vertical-align:-0.2em; margin-right:.25em; }
.n { margin-right:.35em; }
.n::after { content: ':'; }
.money { display:inline-block; background:#fbbf24; color:#111; text-shadow:none;
         padding:0 .4em; border-radius:.35em; margin-right:.35em; font-weight:800;
         font-size:.75em; vertical-align:.1em; }
.t { }
.re { color:#a1a1aa; font-size:.8em; margin-right:.3em; }
.em { height:1.5em; width:auto; vertical-align:-0.35em; margin:0 1px; }
.gif { max-width:100%; max-height:4.5em; width:auto; height:auto;
       vertical-align:-0.35em; margin:0 2px; border-radius:.35em; }
.at { background:rgba(250,204,21,.22); color:#fde68a; border-radius:.3em; padding:0 .2em; }
.sys { color:#e9d5ff; font-style:italic; font-size:.85em; }
.paid { background:rgba(251,191,36,.16); border-left:3px solid #fbbf24;
        padding:2px 6px; border-radius:0 8px 8px 0; }
)CSS";

// Типові значення стрічки (#list) — їх на сторінці задає CSS, а тут розбирає
// сам застосунок (див. list_style.cpp): розкладку стовпця ми робимо нативно,
// щоб кешувати вже намальовані рядки й не перескладати всю стрічку на кожне
// нове повідомлення.
static const int LIST_INSET_DEFAULT = 8;   // #list { inset: 8px }
static const int LIST_GAP_DEFAULT   = 6;   // #list { gap: 6px }

}  // namespace hominka
