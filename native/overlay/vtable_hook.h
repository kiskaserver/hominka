// Найпростіший хук, який тільки буває: підміна одного вказівника у vtable.
//
// COM-обʼєкт (свопчейн, пристрій D3D) починається з вказівника на таблицю
// методів. Підмінивши в ній один запис, ми перехоплюємо метод, не чіпаючи його
// код, — жодних трамплінів, жодного дизасемблера, жодних залежностей. Саме тому
// воно чисто збирається під mingw-w64, на відміну від інлайн-хуків.
//
// Оригінал зберігаємо, щоб викликати його зі свого обробника і повернути на
// місце перед вивантаженням: лишити в грі вказівник на код бібліотеки, якої вже
// немає, — певний спосіб її обвалити.
#pragma once

#include <windows.h>

namespace hominka {

class VtableHook {
public:
    // instance — сам COM-обʼєкт (перший його QWORD — вказівник на vtable).
    // index  — номер методу в таблиці (для IDXGISwapChain::Present це 8).
    // detour — наш обробник.
    bool install(void* instance, int index, void* detour) {
        if (!instance) return false;
        void** vtable = *reinterpret_cast<void***>(instance);
        slot_ = &vtable[index];
        original_ = *slot_;

        DWORD old_protect = 0;
        if (!VirtualProtect(slot_, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect))
            return false;
        *slot_ = detour;
        VirtualProtect(slot_, sizeof(void*), old_protect, &old_protect);
        installed_ = true;
        return true;
    }

    void remove() {
        if (!installed_ || !slot_) return;
        DWORD old_protect = 0;
        if (VirtualProtect(slot_, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
            *slot_ = original_;
            VirtualProtect(slot_, sizeof(void*), old_protect, &old_protect);
        }
        installed_ = false;
    }

    template <typename T>
    T original() const { return reinterpret_cast<T>(original_); }

    bool installed() const { return installed_; }

private:
    void** slot_ = nullptr;   // місце у vtable, куди ми записали свій detour
    void* original_ = nullptr;
    bool installed_ = false;
};

}  // namespace hominka
