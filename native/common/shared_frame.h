// Спільна память для кадру чату: домовленість між Python-частиною (пише) і
// overlay.dll (читає). Один заголовок фіксованого розміру, за ним пікселі BGRA.
//
// Чому саме так, а не черга подій: важке — сокети, шрифти, емоути, розбір —
// лишається в нашому процесі. У грі живе тільки «взяти готову картинку і
// намалювати». Упаде наш чат — гра цього навіть не помітить: DLL просто
// малюватиме останній кадр, поки він є.
//
// Узгодженість без блокувань — seqlock. Той, хто пише, робить seq непарним
// перед записом і парним після. Той, хто читає, бачить непарне або зміну seq
// між читаннями заголовка — значить, кадр саме зараз оновлюють, і його треба
// пропустити (намалювати попередній). Ні м'ютексів, ні очікувань — гра не
// стоїть через нас ніколи.
#pragma once

#include <stdint.h>

namespace hominka {

// «HMK2» задом наперед (little-endian): у памʼяті це байти 'H','M','K','2'.
static const uint32_t SHARED_FRAME_MAGIC = 0x324B4D48u;
static const uint32_t SHARED_FRAME_VERSION = 2;

// Стеля буфера. Кадр чату більший за це не буває; більший монітор не привід
// тримати в спільній памʼяті цілий екран — оверлей займає лише кут.
static const uint32_t SHARED_FRAME_MAX_W = 1920;
static const uint32_t SHARED_FRAME_MAX_H = 1080;

// Ім'я в Local-просторі імен: і гра, і наш процес — один сеанс одного
// користувача. Global\ не беремо навмисно — він вимагає привілею і світив би
// кадр іншим сеансам.
#define HOMINKA_SHARED_FRAME_NAME L"Local\\HominkaOverlayFrame"

enum FrameAnchor {
    ANCHOR_TOP_LEFT = 0,
    ANCHOR_TOP_RIGHT = 1,
    ANCHOR_BOTTOM_LEFT = 2,
    ANCHOR_BOTTOM_RIGHT = 3,
};

// Рівно 64 байти. Порядок і розмір полів — частина домовленості; міняти їх
// можна лише разом із версією.
#pragma pack(push, 4)
struct SharedFrameHeader {
    uint32_t magic;       // SHARED_FRAME_MAGIC — інакше це не наша память
    uint32_t version;     // SHARED_FRAME_VERSION
    uint32_t seq;         // seqlock: непарне = запис триває
    uint32_t width;       // пікселів у кадрі
    uint32_t height;
    uint32_t stride;      // байтів у рядку (= width*4)
    uint32_t anchor;      // FrameAnchor: до якого кута гри тулити
    int32_t  margin_x;    // відступ від кута, пікселів
    int32_t  margin_y;
    uint32_t opacity;     // 0..255 — загальна прозорість поверх альфи кадру
    uint32_t enabled;     // 0 = не малювати (чат сховано)
    uint32_t heartbeat;   // Python збільшує щопису: DLL бачить, що продюсер живий
    uint32_t reserved[4];
};
#pragma pack(pop)

static const uint32_t SHARED_FRAME_HEADER_SIZE = 64;
static const uint32_t SHARED_FRAME_DATA_MAX =
    SHARED_FRAME_MAX_W * SHARED_FRAME_MAX_H * 4;
static const uint32_t SHARED_FRAME_TOTAL =
    SHARED_FRAME_HEADER_SIZE + SHARED_FRAME_DATA_MAX;

}  // namespace hominka
