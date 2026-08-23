// Шейдери для тестового прямокутника — компілюємо в рантаймі.
//
// Чому не готовий байткод у заголовку: щоб його зробити, потрібен fxc або
// D3DCompile на етапі збірки, а ми збираємо під mingw-w64, де d3dcompiler як
// бібліотеки лінкування немає. Тому вантажимо d3dcompiler_47.dll (він є в
// System32 на Windows 10/11) уже в грі й компілюємо крихітний HLSL там. Це
// відбувається один раз на старті оверлея, тож на кадри не впливає.
//
// Якщо d3dcompiler_47.dll раптом немає — чесно пишемо про це в журнал і не
// малюємо; хук усе одно стоїть, і це видно за лічильником кадрів.

#include "overlay_dx11.h"

#include <windows.h>
#include <d3d11.h>

namespace hominka {

namespace {

// Прямокутник із SV_VertexID: чотири кути екранного простору viewport'а. Ніяких
// вхідних буферів — координати рахує сам шейдер.
const char* kVertexHLSL =
    "struct VOut { float4 pos : SV_Position; };\n"
    "VOut main(uint id : SV_VertexID) {\n"
    "  float2 uv = float2((id == 1 || id == 3) ? 1.0 : 0.0,\n"
    "                     (id == 2 || id == 3) ? 1.0 : 0.0);\n"
    "  VOut o;\n"
    "  o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n";

// Помітний напівпрозорий фіолетовий — колір акценту Hominka (#a855f7).
const char* kPixelHLSL =
    "float4 main() : SV_Target {\n"
    "  return float4(0.659, 0.333, 0.969, 0.72);\n"
    "}\n";

typedef HRESULT (WINAPI *D3DCompileFn)(
    LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob**, ID3DBlob**);

D3DCompileFn load_compiler() {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!m) m = LoadLibraryW(L"d3dcompiler_46.dll");
    if (!m) m = LoadLibraryW(L"d3dcompiler_43.dll");
    if (!m) return nullptr;
    return (D3DCompileFn)GetProcAddress(m, "D3DCompile");
}

ID3DBlob* compile(D3DCompileFn fn, const char* src, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = fn(src, strlen(src), nullptr, nullptr, nullptr,
                    "main", target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        log("overlay: компіляція %s не вдалася, hr=0x%lx %s", target,
            (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        if (code) code->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

}  // namespace

bool OverlayDX11::build_shaders() {
    D3DCompileFn compiler = load_compiler();
    if (!compiler) {
        log("overlay: d3dcompiler недоступний — прямокутник не намалюю, "
            "але хук працює (стежте за лічильником кадрів)");
        return false;
    }

    ID3DBlob* vsb = compile(compiler, kVertexHLSL, "vs_4_0");
    ID3DBlob* psb = compile(compiler, kPixelHLSL, "ps_4_0");
    if (!vsb || !psb) {
        if (vsb) vsb->Release();
        if (psb) psb->Release();
        return false;
    }

    HRESULT hr1 = device_->CreateVertexShader(
        vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    HRESULT hr2 = device_->CreatePixelShader(
        psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    vsb->Release();
    psb->Release();

    if (FAILED(hr1) || FAILED(hr2)) {
        log("overlay: створення шейдерів не вдалося, hr=0x%lx/0x%lx",
            (unsigned long)hr1, (unsigned long)hr2);
        return false;
    }
    return true;
}

}  // namespace hominka
