// Мінімальний інлайн-хук для випадків, коли підміна vtable не працює.
//
// DX9 на сучасній Windows дає КОЖНОМУ пристрою власну копію vtable (перевірено:
// адреси таблиць різні, а самих функцій — однакові). Тож підмінити метод у
// таблиці нашої проби марно — гру це не зачепить. Лишається перехопити саму
// функцію за її адресою: на початок кладемо стрибок на наш обробник, а
// збережений пролог + стрибок назад стають «трампліном», через який ми кличемо
// оригінал.
//
// Головний принцип тут — БЕЗПЕКА, а не повнота. Ми розбираємо лише ті форми
// інструкцій, у довжині яких упевнені, і на будь-чому незнайомому чесно
// відмовляємось (install повертає false). Гірший наслідок відмови — оверлея не
// видно; зіпсувати чужий код напівскопійованою інструкцією ми не можемо.
#pragma once

#include <windows.h>
#include <stdint.h>

#include "log.h"

namespace hominka {

#ifdef _WIN64
static const int kPatchLen = 14;   // FF 25 00000000 + 8 байтів абсолютної адреси
#else
static const int kPatchLen = 5;    // E9 + rel32
#endif

// Довжина однієї інструкції або 0, якщо форму не розпізнано (тоді відмова).
// Свідомо неповний декодер: покриває пролоґи, які реально бувають у API
// (mov/push/pop/sub/lea/mov-imm), і зупиняється на всьому іншому.
inline int insn_len(const uint8_t* p) {
    int i = 0;
    bool rexw = false;
    bool op66 = false;

    // Префікси.
    for (;; ++i) {
        uint8_t b = p[i];
        if (b == 0x66) { op66 = true; continue; }
        if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65)
            continue;
        break;
    }
#ifdef _WIN64
    if ((p[i] & 0xF0) == 0x40) {           // REX
        rexw = (p[i] & 0x08) != 0;
        ++i;
    }
#endif
    uint8_t op = p[i++];

    // push/pop reg, одно- й двобайтні no-modrm.
    if (op >= 0x50 && op <= 0x5F) return i;
    if (op == 0x90 || op == 0xC3 || op == 0xC9) return i;
    if (op == 0x6A) return i + 1;               // push imm8
    if (op == 0x68) return i + (op66 ? 2 : 4);  // push imm32

    // mov r/m,r | r,r/m ; lea ; grp1 83 ; sub/add/... 01/03/29/2B/31/33/39/3B ;
    // test 85 ; xor 33 — усі з ModR/M.
    bool has_modrm = false;
    int imm = 0;
    switch (op) {
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
        case 0x01: case 0x03: case 0x29: case 0x2B: case 0x31: case 0x33:
        case 0x39: case 0x3B: case 0x85: case 0x84: case 0x63:
            has_modrm = true; imm = 0; break;
        case 0x83:                       // grp1 Ev, Ib
            has_modrm = true; imm = 1; break;
        case 0x81:                       // grp1 Ev, Iz
            has_modrm = true; imm = op66 ? 2 : 4; break;
        case 0xC7:                       // mov Ev, Iz
            has_modrm = true; imm = op66 ? 2 : 4; break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:  // mov reg, imm
            return i + (rexw ? 8 : (op66 ? 2 : 4));
        default:
            return 0;                    // невідомо — відмова
    }

    if (has_modrm) {
        uint8_t modrm = p[i++];
        uint8_t mod = modrm >> 6, rm = modrm & 7;
        if (mod != 3) {
            if (rm == 4) {               // SIB
                uint8_t sib = p[i++];
                uint8_t base = sib & 7;
                if (mod == 0 && base == 5) i += 4;    // disp32
            } else if (mod == 0 && rm == 5) {
#ifdef _WIN64
                return 0;                // RIP-відносна — копіювати не можна, відмова
#else
                i += 4;                  // x86: абсолютний disp32 — копіюється як є
#endif
            }
            if (mod == 1) i += 1;        // disp8
            else if (mod == 2) i += 4;   // disp32
        }
    }
    return i + imm;
}

class InlineHook {
public:
    // target — адреса функції; detour — наш обробник. Повертає false БЕЗПЕЧНО,
    // не чіпаючи чужий код, якщо пролог не вдалося розібрати.
    bool install(void* target, void* detour) {
        if (!target || !detour) return false;
        uint8_t* t = reinterpret_cast<uint8_t*>(target);

        int copied = 0;
        while (copied < kPatchLen) {
            int n = insn_len(t + copied);
            if (n <= 0) {
                log("overlay: інлайн-хук відмовився — незнайомий пролог");
                return false;
            }
            copied += n;
        }
        if (copied > 32) return false;

        // Трамплін: [збережений пролог][стрибок на target+copied].
        tramp_ = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
        if (!tramp_) return false;
        memcpy(tramp_, t, copied);
        write_jmp(tramp_ + copied, t + copied);

        // На початок функції — стрибок на detour, решту скопійованого — NOP.
        DWORD old;
        if (!VirtualProtect(t, copied, PAGE_EXECUTE_READWRITE, &old)) {
            VirtualFree(tramp_, 0, MEM_RELEASE); tramp_ = nullptr;
            return false;
        }
        write_jmp(t, reinterpret_cast<uint8_t*>(detour));
        for (int i = kPatchLen; i < copied; ++i) t[i] = 0x90;
        VirtualProtect(t, copied, old, &old);
        FlushInstructionCache(GetCurrentProcess(), t, copied);

        target_ = t;
        copied_ = copied;
        return true;
    }

    void remove() {
        if (!target_) return;
        DWORD old;
        if (VirtualProtect(target_, copied_, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(target_, tramp_, copied_);   // повертаємо оригінальні байти
            VirtualProtect(target_, copied_, old, &old);
            FlushInstructionCache(GetCurrentProcess(), target_, copied_);
        }
        if (tramp_) { VirtualFree(tramp_, 0, MEM_RELEASE); tramp_ = nullptr; }
        target_ = nullptr;
    }

    template <typename T>
    T original() const { return reinterpret_cast<T>(tramp_); }

    bool installed() const { return target_ != nullptr; }

private:
    // Пише безумовний стрибок з `from` на `to`.
    static void write_jmp(uint8_t* from, uint8_t* to) {
#ifdef _WIN64
        // FF 25 00000000 — стрибок за адресою, що лежить одразу за інструкцією.
        from[0] = 0xFF; from[1] = 0x25;
        *reinterpret_cast<uint32_t*>(from + 2) = 0;
        *reinterpret_cast<uint64_t*>(from + 6) = reinterpret_cast<uint64_t>(to);
#else
        from[0] = 0xE9;                  // E9 rel32
        *reinterpret_cast<int32_t*>(from + 1) =
            (int32_t)(to - (from + 5));
#endif
    }

    uint8_t* target_ = nullptr;
    uint8_t* tramp_ = nullptr;
    int copied_ = 0;
};

}  // namespace hominka
