// Шейдери оверлея — компілюємо в рантаймі.
//
// Чому не готовий байткод у заголовку: щоб його зробити, потрібен fxc або
// D3DCompile на етапі збірки, а ми збираємо під mingw-w64, де d3dcompiler як
// бібліотеки лінкування немає. Тому вантажимо d3dcompiler_47.dll (він є в
// System32 на Windows 10/11) уже в грі й компілюємо там. Це один раз на старті,
// на кадри не впливає.
//
// Вершинний шейдер малює прямокутник на весь viewport за SV_VertexID (буфер не
// потрібен) і віддає UV; піксельний — семплить текстуру кадру чату й гасить її
// загальною прозорістю. Формат BGRA семпл повертає вже як RGBA, тож колір
// правильний без перестановки.

#include "overlay_dx11.h"
#include "overlay_dx12.h"

#include <windows.h>
#include <d3d11.h>

namespace hominka {

namespace {

const char* kVertexHLSL =
    "struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VOut main(uint id : SV_VertexID) {\n"
    "  float2 uv = float2((id == 1 || id == 3) ? 1.0 : 0.0,\n"
    "                     (id == 2 || id == 3) ? 1.0 : 0.0);\n"
    "  VOut o;\n"
    "  o.uv = uv;\n"
    "  o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n";

// b0.x — загальна прозорість 0..1 (додатково до альфи кадру).
const char* kPixelHLSL =
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "cbuffer Params : register(b0) { float opacity; float3 pad; };\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float4 c = tex.Sample(smp, uv);\n"
    "  c.a *= opacity;\n"
    "  return c;\n"
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
        log("overlay: d3dcompiler недоступний — кадр чату намалювати нічим");
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

    // Константний буфер прозорості. Оновлюємо його щоразу перед малюванням у
    // blit(); тут лише створюємо. 16 байтів — мінімум для cbuffer.
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&cbd, nullptr, &cbuf_);
    return true;
}

// Ті самі шейдери, скомпільовані під DX12 (модель 5_0). Повертає байткод; його
// вивільняє викликач (overlay_dx12.h).
bool dx12_compile_shaders(D3D12Shaders* out) {
    D3DCompileFn compiler = load_compiler();
    if (!compiler) { log("overlay(dx12): d3dcompiler недоступний"); return false; }
    out->vs = compile(compiler, kVertexHLSL, "vs_5_0");
    out->ps = compile(compiler, kPixelHLSL, "ps_5_0");
    if (!out->vs || !out->ps) {
        if (out->vs) out->vs->Release();
        if (out->ps) out->ps->Release();
        out->vs = out->ps = nullptr;
        return false;
    }
    return true;
}

}  // namespace hominka
