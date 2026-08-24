// Мінімальний інлайн-хук для випадків, коли підміна vtable не працює.
//
// Потрібен там, де перехоплюємо ПЛОСКУ функцію (не COM-метод у vtable): DX9
// EndScene має власну vtable на кожен пристрій; wglSwapBuffers і
// vkQueuePresentKHR — звичайні експорти. На початок функції кладемо стрибок на
// наш обробник, а збережений пролог + стрибок назад стають «трампліном», через
// який кличемо оригінал.
//
// Головний принцип — БЕЗПЕКА: розбираємо лише ті форми інструкцій, у довжині
// яких упевнені, і на будь-чому незнайомому чесно відмовляємось (install →
// false), не чіпаючи чужий код. Гірший наслідок відмови — оверлея не видно.
//
// Дві тонкощі x64, без яких реальні прологи (той самий wglSwapBuffers) не
// перенести:
//   * RIP-відносні операнди (`mov rax,[rip+disp]`) при копіюванні в трамплін
//     треба перерахувати — інакше вони вкажуть не туди;
//   * щоб перерахований disp32 «дотягнувся», трамплін виділяємо ПОРУЧ із
//     функцією (в межах ±2 ГБ).
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

// Довжина інструкції або 0 (форму не розпізнано → відмова). Якщо в інструкції є
// RIP-відносний disp32 (лише x64), *rip_off отримує його зсув усередині
// інструкції, інакше -1.
inline int insn_len(const uint8_t* p, int* rip_off) {
    if (rip_off) *rip_off = -1;
    int i = 0;
    bool rexw = false, op66 = false;

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
    if ((p[i] & 0xF0) == 0x40) { rexw = (p[i] & 0x08) != 0; ++i; }
#endif
    uint8_t op = p[i++];

    if (op >= 0x50 && op <= 0x5F) return i;          // push/pop reg
    if (op == 0x90 || op == 0xC3 || op == 0xC9) return i;
    if (op == 0x6A) return i + 1;                    // push imm8
    if (op == 0x68) return i + (op66 ? 2 : 4);       // push imm32

    bool has_modrm = false;
    int imm = 0;
    switch (op) {
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
        case 0x01: case 0x03: case 0x29: case 0x2B: case 0x31: case 0x33:
        case 0x39: case 0x3B: case 0x85: case 0x84: case 0x63:
            has_modrm = true; imm = 0; break;
        case 0x83: has_modrm = true; imm = 1; break;
        case 0x81: case 0xC7: has_modrm = true; imm = op66 ? 2 : 4; break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            return i + (rexw ? 8 : (op66 ? 2 : 4));
        default:
            return 0;
    }

    if (has_modrm) {
        uint8_t modrm = p[i++];
        uint8_t mod = modrm >> 6, rm = modrm & 7;
        if (mod != 3) {
            if (rm == 4) {                            // SIB
                uint8_t sib = p[i++];
                if (mod == 0 && (sib & 7) == 5) i += 4;
            } else if (mod == 0 && rm == 5) {
#ifdef _WIN64
                if (rip_off) *rip_off = i;            // RIP-відносний disp32
                i += 4;
#else
                i += 4;                               // x86: абсолютний disp32
#endif
            }
            if (mod == 1) i += 1;
            else if (mod == 2) i += 4;
        }
    }
    return i + imm;
}

class InlineHook {
public:
    bool install(void* target, void* detour) {
        if (!target || !detour) return false;
        uint8_t* t = reinterpret_cast<uint8_t*>(target);

        // Розбираємо пролог: скільки цілих інструкцій перекриє наш стрибок і де
        // в них RIP-відносні операнди.
        int copied = 0;
        int rip_at[8]; int rip_n = 0;
        while (copied < kPatchLen) {
            int off = -1;
            int n = insn_len(t + copied, &off);
            if (n <= 0) { log("overlay: інлайн-хук відмовився — незнайомий пролог"); return false; }
            if (off >= 0 && rip_n < 8) rip_at[rip_n++] = copied + off;
            copied += n;
        }
        if (copied > 32) return false;

        tramp_ = alloc_near(t);
        if (!tramp_) { log("overlay: інлайн-хук — не виділив трамплін поруч"); return false; }
        memcpy(tramp_, t, copied);

#ifdef _WIN64
        // Перерахунок RIP-відносних disp32: різниця адрес та сама для всіх
        // скопійованих інструкцій, тож дельта одна.
        int64_t delta = (int64_t)t - (int64_t)tramp_;
        for (int j = 0; j < rip_n; ++j) {
            int32_t* d = reinterpret_cast<int32_t*>(tramp_ + rip_at[j]);
            int64_t nd = (int64_t)*d + delta;
            if (nd < INT32_MIN || nd > INT32_MAX) {
                log("overlay: інлайн-хук — RIP-операнд не дотягується");
                VirtualFree(tramp_, 0, MEM_RELEASE); tramp_ = nullptr;
                return false;
            }
            *d = (int32_t)nd;
        }
#endif
        write_jmp(tramp_ + copied, t + copied);   // трамплін → назад у функцію

        DWORD old;
        if (!VirtualProtect(t, copied, PAGE_EXECUTE_READWRITE, &old)) {
            VirtualFree(tramp_, 0, MEM_RELEASE); tramp_ = nullptr;
            return false;
        }
        write_jmp(t, reinterpret_cast<uint8_t*>(detour));
        for (int j = kPatchLen; j < copied; ++j) t[j] = 0x90;   // NOP-и «хвоста»
        VirtualProtect(t, copied, old, &old);
        FlushInstructionCache(GetCurrentProcess(), t, copied);

        target_ = t; copied_ = copied;
        return true;
    }

    void remove() {
        if (!target_) return;
        DWORD old;
        if (VirtualProtect(target_, copied_, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(target_, tramp_, copied_);
            VirtualProtect(target_, copied_, old, &old);
            FlushInstructionCache(GetCurrentProcess(), target_, copied_);
        }
        if (tramp_) { VirtualFree(tramp_, 0, MEM_RELEASE); tramp_ = nullptr; }
        target_ = nullptr;
    }

    template <typename T> T original() const { return reinterpret_cast<T>(tramp_); }
    bool installed() const { return target_ != nullptr; }

private:
    static void write_jmp(uint8_t* from, uint8_t* to) {
#ifdef _WIN64
        from[0] = 0xFF; from[1] = 0x25;
        *reinterpret_cast<uint32_t*>(from + 2) = 0;
        *reinterpret_cast<uint64_t*>(from + 6) = reinterpret_cast<uint64_t>(to);
#else
        from[0] = 0xE9;
        *reinterpret_cast<int32_t*>(from + 1) = (int32_t)(to - (from + 5));
#endif
    }

    // Виділяє 64 байти під трамплін ПОРУЧ із target (у межах ±2 ГБ), щоб
    // перерахований RIP-відносний disp32 дотягувався. На x86 адреса будь-яка.
    static uint8_t* alloc_near(uint8_t* target) {
#ifdef _WIN64
        const uint64_t GB2 = 0x60000000ULL;   // трохи менше за 2 ГБ, із запасом
        const uint64_t step = 0x10000ULL;     // гранулярність VirtualAlloc
        uint64_t base = (uint64_t)target;
        for (uint64_t off = step; off < GB2; off += step) {
            for (int dir = 0; dir < 2; ++dir) {
                uint64_t addr = dir ? base + off : base - off;
                void* p = VirtualAlloc((void*)(addr & ~(step - 1)), 64,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (p) return (uint8_t*)p;
            }
        }
        return nullptr;
#else
        return (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
#endif
    }

    uint8_t* target_ = nullptr;
    uint8_t* tramp_ = nullptr;
    int copied_ = 0;
};

}  // namespace hominka
