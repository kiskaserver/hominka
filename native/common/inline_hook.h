// Мінімальний інлайн-хук у стилі MinHook для випадків, коли підміна vtable не
// працює: DX9 EndScene (у кожного пристрою власна vtable), wglSwapBuffers і
// vkQueuePresentKHR (плоскі експорти), а також саме тіло IDXGISwapChain::Present
// (щоб малювати ГЛИБШЕ за захоплення OBS).
//
// Головні принципи:
//
//  1. БЕЗПЕКА розбору. Розбираємо лише ті форми інструкцій, у довжині яких
//     упевнені; на будь-чому незнайомому чесно відмовляємось (install → false),
//     не чіпаючи чужий код. Гірший наслідок відмови — оверлея не видно.
//
//  2. ЛАТКА РІВНО 5 БАЙТІВ (E9 rel32) — не 14. Раніше ми клали 14-байтний
//     абсолютний стрибок (FF25 + адреса), бо наш детур далі ±2 ГБ. Але це
//     затирало байти ПІСЛЯ входу — а в dxgi у Present є внутрішній «швидкий
//     вхід» на Present+5, і його ламало (краш саме там). Тепер кладемо 5-байтний
//     відносний E9 на «острівець» ПОРУЧ (у межах ±2 ГБ), а вже острівець робить
//     далекий абсолютний стрибок на детур. Перша інструкція Present — рівно 5
//     байтів, тож Present+5 лишається цілим.
//
//  3. ЗАМОРОЗКА ПОТОКІВ під час підміни (як MinHook): жоден інший потік не
//     виконує ці байти, а якщо чийсь IP стоїть усередині латки — переносимо його
//     на копію в трампліні. Без цього підміна гарячої функції інколи трапляється
//     саме тоді, коли інший потік її виконує, і краш — на півзаписаній інструкції.
#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>

#include "log.h"

namespace hominka {

#ifdef _WIN64
static const int kFarLen = 14;   // FF 25 00000000 + 8 байтів абсолютної адреси
#else
static const int kFarLen = 5;    // E9 + rel32 (на x86 дотягується будь-куди)
#endif
static const int kJmpLen = 5;    // E9 rel32 — рівно стільки затираємо на вході

// Заморожує всі ІНШІ потоки процесу на час підміни й повертає «застряглий»
// усередині латки IP на еквівалентну адресу в трампліні. Саме так робить
// MinHook; це стандартне, а не самодіяльне рішення.
class ThreadFreezer {
public:
    ~ThreadFreezer() { thaw(); }

    void freeze_others() {
        DWORD me_pid = GetCurrentProcessId();
        DWORD me_tid = GetCurrentThreadId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != me_pid) continue;
                if (te.th32ThreadID == me_tid) continue;
                HANDLE h = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                    FALSE, te.th32ThreadID);
                if (!h) continue;
                if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
                if (n_ < kMax) handles_[n_++] = h; else { ResumeThread(h); CloseHandle(h); }
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    // Якщо IP замороженого потоку в [from, from+len) — переносимо його на
    // other+(IP-from). Прологи в цілі й трампліні однакової довжини, тож зсув
    // інструкції збігається.
    void relocate_ip(uint8_t* from, int len, uint8_t* other) {
        for (int i = 0; i < n_; ++i) {
            alignas(16) CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(handles_[i], &ctx)) continue;
#ifdef _WIN64
            uint64_t ip = ctx.Rip;
#else
            uint64_t ip = ctx.Eip;
#endif
            uint64_t lo = (uint64_t)from, hi = lo + (uint64_t)len;
            if (ip >= lo && ip < hi) {
                uint64_t nip = (uint64_t)other + (ip - lo);
#ifdef _WIN64
                ctx.Rip = nip;
#else
                ctx.Eip = (DWORD)nip;
#endif
                SetThreadContext(handles_[i], &ctx);
                log("overlay: інлайн-хук — потік стояв у латці, IP перенесено");
            }
        }
    }

    void thaw() {
        for (int i = 0; i < n_; ++i) { ResumeThread(handles_[i]); CloseHandle(handles_[i]); }
        n_ = 0;
    }

private:
    static const int kMax = 256;
    HANDLE handles_[kMax];
    int n_ = 0;
};

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
    // target — куди чіпляємось; detour — наш обробник. Повертає false, якщо
    // пролог не піддається безпечному розбору (тоді нічого не чіпаємо).
    bool install(void* target, void* detour) {
        if (!target || !detour) return false;
        uint8_t* t = reinterpret_cast<uint8_t*>(target);

        // Виділяємо блок ПОРУЧ (у межах ±2 ГБ) — щоб 5-байтний E9 зі входу до
        // нього дотягнувся. У ньому живе і трамплін (оригінальний пролог + шлях
        // назад), і «острівець» далекого стрибка на детур.
        block_ = alloc_near(t);
        if (!block_) return false;

        // Різновид А — плоский перехідник (E9 rel32 / EB rel8): напр. експорти
        // vulkan-1.dll. Оригіналом лишаємо ЦІЛЬ цього стрибка (справжню функцію),
        // а сам 5-байтний стрибок перенаправляємо на острівець → детур. Розмір
        // той самий (5 байтів), тож сусідів не чіпаємо; добивка не потрібна.
        if (t[0] == 0xE9 || t[0] == 0xEB) {
            uint8_t* jt = (t[0] == 0xE9)
                ? t + 5 + *reinterpret_cast<int32_t*>(t + 1)
                : t + 2 + *reinterpret_cast<int8_t*>(t + 1);
            uint8_t* island = block_;
            write_far(island, reinterpret_cast<uint8_t*>(detour));

            // Оригінал E9 — одна 5-байтна інструкція: заморозки досить, бо
            // єдиний можливий IP усередині — сам вхід t, а звідти потік просто
            // піде вже нашим стрибком (той теж веде на детур).
            memcpy(saved_, t, kJmpLen);
            ThreadFreezer fz; fz.freeze_others();
            DWORD old;
            if (!VirtualProtect(t, kJmpLen, PAGE_EXECUTE_READWRITE, &old)) { fz.thaw(); return fail(); }
            write_e9(t, island);
            VirtualProtect(t, kJmpLen, old, &old);
            FlushInstructionCache(GetCurrentProcess(), t, kJmpLen);
            fz.thaw();

            target_ = t; copied_ = kJmpLen; is_thunk_ = true; call_orig_ = jt;
            return true;
        }

        // Різновид Б — звичайна функція. Копіюємо цілі інструкції прологу, поки
        // не накриємо 5 байтів, будуємо трамплін і кладемо на вхід 5-байтний E9.
        int copied = 0;
        int rip_at[8]; int rip_n = 0;
        while (copied < kJmpLen) {
            int off = -1;
            int n = insn_len(t + copied, &off);
            if (n <= 0) { log("overlay: інлайн-хук відмовився — незнайомий пролог"); return fail(); }
            if (off >= 0 && rip_n < 8) rip_at[rip_n++] = copied + off;
            copied += n;
        }
        if (copied > 32) return fail();

        // Розкладка блоку: [трамплін: copied байтів + стрибок назад][острівець].
        uint8_t* tramp = block_;
        memcpy(tramp, t, copied);
        memcpy(saved_, t, copied);            // оригінал для відновлення

#ifdef _WIN64
        // Перерахунок RIP-відносних disp32 у скопійованому пролозі.
        int64_t delta = (int64_t)t - (int64_t)tramp;
        for (int j = 0; j < rip_n; ++j) {
            int32_t* d = reinterpret_cast<int32_t*>(tramp + rip_at[j]);
            int64_t nd = (int64_t)*d + delta;
            if (nd < INT32_MIN || nd > INT32_MAX) {
                log("overlay: інлайн-хук — RIP-операнд не дотягується");
                return fail();
            }
            *d = (int32_t)nd;
        }
#endif
        write_far(tramp + copied, t + copied);            // трамплін → назад у функцію
        uint8_t* island = tramp + copied + kFarLen;
        write_far(island, reinterpret_cast<uint8_t*>(detour));   // острівець → детур

        // Латка входу — 5 байтів E9 на острівець. Морозимо потоки й переносимо
        // IP тих, хто стоїть у [t, t+copied) (ми чіпаємо і NOP-хвіст).
        ThreadFreezer fz; fz.freeze_others();
        fz.relocate_ip(t, copied, tramp);
        DWORD old;
        if (!VirtualProtect(t, copied, PAGE_EXECUTE_READWRITE, &old)) { fz.thaw(); return fail(); }
        write_e9(t, island);
        for (int j = kJmpLen; j < copied; ++j) t[j] = 0x90;   // NOP-хвіст (недосяжний)
        VirtualProtect(t, copied, old, &old);
        FlushInstructionCache(GetCurrentProcess(), t, copied);
        fz.thaw();

        target_ = t; copied_ = copied; call_orig_ = tramp;
        return true;
    }

    void remove() {
        if (!target_) return;
        ThreadFreezer fz; fz.freeze_others();
        if (!is_thunk_ && call_orig_)
            fz.relocate_ip(reinterpret_cast<uint8_t*>(call_orig_), copied_, target_);
        DWORD old;
        if (VirtualProtect(target_, copied_, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(target_, saved_, copied_);
            VirtualProtect(target_, copied_, old, &old);
            FlushInstructionCache(GetCurrentProcess(), target_, copied_);
        }
        fz.thaw();
        if (block_) { VirtualFree(block_, 0, MEM_RELEASE); block_ = nullptr; }
        target_ = nullptr; call_orig_ = nullptr; is_thunk_ = false;
    }

    template <typename T> T original() const { return reinterpret_cast<T>(call_orig_); }
    bool installed() const { return target_ != nullptr; }

private:
    bool fail() {
        if (block_) { VirtualFree(block_, 0, MEM_RELEASE); block_ = nullptr; }
        return false;
    }

    // 5-байтний відносний стрибок E9.
    static void write_e9(uint8_t* from, uint8_t* to) {
        from[0] = 0xE9;
        *reinterpret_cast<int32_t*>(from + 1) = (int32_t)(to - (from + 5));
    }

    // Далекий стрибок: x64 — абсолютний FF25+адреса (14 б); x86 — E9 (дотягнеться).
    static void write_far(uint8_t* from, uint8_t* to) {
#ifdef _WIN64
        from[0] = 0xFF; from[1] = 0x25;
        *reinterpret_cast<uint32_t*>(from + 2) = 0;
        *reinterpret_cast<uint64_t*>(from + 6) = reinterpret_cast<uint64_t>(to);
#else
        write_e9(from, to);
#endif
    }

    // Виділяє блок ПОРУЧ із target (у межах ±2 ГБ), щоб 5-байтний E9 дотягнувся
    // до острівця, а перерахований RIP-операнд трампліна — до своєї цілі.
    static uint8_t* alloc_near(uint8_t* target) {
#ifdef _WIN64
        const uint64_t GB2 = 0x60000000ULL;   // трохи менше за 2 ГБ, із запасом
        const uint64_t step = 0x10000ULL;     // гранулярність VirtualAlloc
        uint64_t base = (uint64_t)target;
        for (uint64_t off = step; off < GB2; off += step) {
            for (int dir = 0; dir < 2; ++dir) {
                uint64_t addr = dir ? base + off : base - off;
                void* p = VirtualAlloc((void*)(addr & ~(step - 1)), 128,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (p) return (uint8_t*)p;
            }
        }
        return nullptr;
#else
        return (uint8_t*)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
#endif
    }

    uint8_t* target_ = nullptr;
    uint8_t* block_ = nullptr;    // трамплін + острівець
    void* call_orig_ = nullptr;   // що повертає original(): трамплін або справжня функція
    int copied_ = 0;
    bool is_thunk_ = false;
    uint8_t saved_[32] = {0};      // оригінальні байти входу для відновлення
};

}  // namespace hominka
