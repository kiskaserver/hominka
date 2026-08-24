// Малювання кадру чату поверх кадру гри для DirectX 12.
//
// DX12 не має «глобального стану»: усе робиться через списки команд і чергу.
// Present ми вже ловимо тим самим DXGI-хуком, що й DX11 (свопчейн — спільний
// IDXGISwapChain). Але щоб щось намалювати, потрібна ЧЕРГА КОМАНД гри — а зі
// свопчейна її не дістати. Тому окремо перехоплюємо ID3D12CommandQueue::
// ExecuteCommandLists (спільна vtable) і запам'ятовуємо чергу типу DIRECT, якою
// гра малює. Далі в Present пишемо свій список команд: перехід заднього буфера в
// RENDER_TARGET, малювання квадрата з текстурою чату, перехід назад — і кладемо
// список у ту саму чергу.
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdint.h>

#include "../common/log.h"
#include "../common/shared_frame.h"
#include "shared_frame_reader.h"

namespace hominka {

// Байткод шейдерів компілюємо в рантаймі (як у DX11) — d3dcompiler_47 є в
// System32. Функції в overlay_dx12.cpp немає; тримаємо все тут інлайново.
struct D3D12Shaders { ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; };
bool dx12_compile_shaders(D3D12Shaders* out);

class OverlayDX12 {
public:
    // Викликається з хука ExecuteCommandLists: запам'ятовуємо чергу DIRECT.
    void capture_queue(ID3D12CommandQueue* q) {
        if (queue_ || !q) return;
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        queue_ = q;
        log("overlay(dx12): чергу команд (DIRECT) захоплено");
    }

    void draw(IDXGISwapChain* swap) {
        if (!queue_) return;                 // ще не знаємо, куди слати команди
        if (!reader_.ensure_open()) return;
        if (!ensure_init(swap)) return;

        FrameView f;
        bool got = reader_.read(&f);
        bool have_new = false;
        if (got) {
            if (f.target_pid && f.target_pid != GetCurrentProcessId()) return;
            if (!logged_) { logged_ = true;
                log("overlay(dx12): кадр — enabled=%u target=%u ми=%u розмір=%ux%u",
                    (unsigned)f.enabled, f.target_pid, (unsigned)GetCurrentProcessId(),
                    f.width, f.height); }
            if (!f.enabled) { enabled_ = false; return; }
            enabled_ = true;
            if (f.seq != tex_seq_ || f.width != tex_w_ || f.height != tex_h_) {
                if (!ensure_texture(f.width, f.height)) return;
                stage_pixels(f);
                have_new = true;
                tex_seq_ = f.seq;
            }
            last_ = f;
        } else if (!enabled_ || tex_seq_ == 0) {
            return;
        }
        if (!srv_ok_ || tex_w_ == 0) return;

        blit(have_new);
    }

    void release() {
        wait_idle();
        rel(fence_); rel(srv_heap_); rel(rtv_heap_); rel(cmd_list_);
        for (auto& a : alloc_) rel(a);
        rel(pso_); rel(root_); rel(tex_); rel(upload_);
        for (auto& b : back_) rel(b);
        rel(device_);
        // queue_ належить грі — не звільняємо.
        reader_.close();
        inited_ = false;
    }

private:
    template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

    bool ensure_init(IDXGISwapChain* swap) {
        if (inited_) return true;
        if (FAILED(swap->GetDevice(__uuidof(ID3D12Device), (void**)&device_)) || !device_)
            return false;   // не DX12

        DXGI_SWAP_CHAIN_DESC sd;
        if (FAILED(swap->GetDesc(&sd))) return false;
        buffers_ = sd.BufferCount;
        if (buffers_ > kMaxBuf) buffers_ = kMaxBuf;
        rtv_format_ = sd.BufferDesc.Format;

        D3D12_DESCRIPTOR_HEAP_DESC rh = {};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = buffers_;
        if (FAILED(device_->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void**)&rtv_heap_)))
            return false;
        rtv_step_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < buffers_; ++i) {
            if (FAILED(swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&back_[i]))) return false;
            device_->CreateRenderTargetView(back_[i], nullptr, h);
            rtv_[i] = h;
            h.ptr += rtv_step_;
        }

        for (UINT i = 0; i < buffers_; ++i)
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    __uuidof(ID3D12CommandAllocator), (void**)&alloc_[i]))) return false;
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0],
                nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cmd_list_))) return false;
        cmd_list_->Close();

        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                (void**)&fence_))) return false;
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // SRV-купа (видима шейдеру) на одну текстуру.
        D3D12_DESCRIPTOR_HEAP_DESC sh = {};
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.NumDescriptors = 1;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&sh, __uuidof(ID3D12DescriptorHeap), (void**)&srv_heap_)))
            return false;

        if (!build_pipeline()) return false;

        inited_ = true;
        log("overlay(dx12): конвеєр готовий (буферів=%u)", buffers_);
        return true;
    }

    bool build_pipeline() {
        // Root signature: таблиця з 1 SRV (t0) + корневий 32-бітний констант
        // (прозорість, b0) + статичний семплер.
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;   // b0
        params[1].Constants.Num32BitValues = 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2;
        rs.pParameters = params;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &samp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            log("overlay(dx12): serialize root sig не вдалося");
            rel(err); return false;
        }
        HRESULT hr = device_->CreateRootSignature(0, sig->GetBufferPointer(),
            sig->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&root_);
        rel(sig); rel(err);
        if (FAILED(hr)) return false;

        D3D12Shaders sh;
        if (!dx12_compile_shaders(&sh)) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = root_;
        pd.VS = { sh.vs->GetBufferPointer(), sh.vs->GetBufferSize() };
        pd.PS = { sh.ps->GetBufferPointer(), sh.ps->GetBufferSize() };
        pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = rtv_format_;
        pd.SampleDesc.Count = 1;
        hr = device_->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&pso_);
        rel(sh.vs); rel(sh.ps);
        if (FAILED(hr)) { log("overlay(dx12): PSO не створено, hr=0x%lx", (unsigned long)hr); return false; }
        return true;
    }

    bool ensure_texture(uint32_t w, uint32_t h) {
        if (tex_ && w == tex_w_ && h == tex_h_) return true;
        wait_idle();
        rel(tex_); rel(upload_);
        srv_ok_ = false;

        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                __uuidof(ID3D12Resource), (void**)&tex_))) return false;

        // Розкладку рядків для upload-буфера дає GetCopyableFootprints.
        UINT64 total = 0;
        device_->GetCopyableFootprints(&td, 0, 1, 0, &footprint_, &rows_, &row_bytes_, &total);
        upload_bytes_ = total;

        D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = total; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.SampleDesc.Count = 1;
        ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                __uuidof(ID3D12Resource), (void**)&upload_))) { rel(tex_); return false; }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(tex_, &srv, srv_heap_->GetCPUDescriptorHandleForHeapStart());

        tex_w_ = w; tex_h_ = h; srv_ok_ = true;
        return true;
    }

    // Кладе пікселі кадру в upload-буфер із правильним вирівнюванням рядків.
    void stage_pixels(const FrameView& f) {
        uint8_t* dst = nullptr;
        D3D12_RANGE none = {0, 0};
        if (FAILED(upload_->Map(0, &none, (void**)&dst))) return;
        uint8_t* base = dst + footprint_.Offset;
        uint32_t src_row = f.width * 4;
        for (uint32_t y = 0; y < f.height; ++y)
            memcpy(base + (size_t)y * footprint_.Footprint.RowPitch,
                   f.pixels + (size_t)y * src_row, src_row);
        upload_->Unmap(0, nullptr);
    }

    void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                 D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br = {};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter = b;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &br);
    }

    void blit(bool copy_tex) {
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(swap_->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) || !sc3) return;
        UINT idx = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
        if (idx >= buffers_) return;

        // Чекаємо, поки попереднє наше подання на цей allocator завершилось.
        if (fence_val_[idx] && fence_->GetCompletedValue() < fence_val_[idx]) {
            fence_->SetEventOnCompletion(fence_val_[idx], fence_event_);
            WaitForSingleObject(fence_event_, 100);
        }
        alloc_[idx]->Reset();
        cmd_list_->Reset(alloc_[idx], pso_);

        if (copy_tex) {
            barrier(cmd_list_, tex_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION d = {}; d.pResource = tex_;
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION s = {}; s.pResource = upload_;
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = footprint_;
            cmd_list_->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            barrier(cmd_list_, tex_, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        barrier(cmd_list_, back_[idx], D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd_list_->OMSetRenderTargets(1, &rtv_[idx], FALSE, nullptr);

        D3D12_RESOURCE_DESC bd = back_[idx]->GetDesc();
        float ow = (float)tex_w_, oh = (float)tex_h_;
        float sw = (float)bd.Width, sh = (float)bd.Height;
        float x, y;
        switch (last_.anchor) {
            case ANCHOR_TOP_RIGHT:    x = sw - ow - last_.margin_x; y = (float)last_.margin_y; break;
            case ANCHOR_BOTTOM_LEFT:  x = (float)last_.margin_x; y = sh - oh - last_.margin_y; break;
            case ANCHOR_BOTTOM_RIGHT: x = sw - ow - last_.margin_x; y = sh - oh - last_.margin_y; break;
            default:                  x = (float)last_.margin_x; y = (float)last_.margin_y; break;
        }
        D3D12_VIEWPORT vp = { x, y, ow, oh, 0.f, 1.f };
        D3D12_RECT sr = { (LONG)x, (LONG)y, (LONG)(x + ow), (LONG)(y + oh) };
        cmd_list_->RSSetViewports(1, &vp);
        cmd_list_->RSSetScissorRects(1, &sr);

        ID3D12DescriptorHeap* heaps[] = { srv_heap_ };
        cmd_list_->SetDescriptorHeaps(1, heaps);
        cmd_list_->SetGraphicsRootSignature(root_);
        cmd_list_->SetGraphicsRootDescriptorTable(0, srv_heap_->GetGPUDescriptorHandleForHeapStart());
        float op = last_.opacity / 255.0f;
        cmd_list_->SetGraphicsRoot32BitConstants(1, 1, &op, 0);
        cmd_list_->SetPipelineState(pso_);
        cmd_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmd_list_->DrawInstanced(4, 1, 0, 0);

        barrier(cmd_list_, back_[idx], D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT);
        cmd_list_->Close();

        ID3D12CommandList* lists[] = { cmd_list_ };
        queue_->ExecuteCommandLists(1, lists);
        fence_val_[idx] = ++fence_counter_;
        queue_->Signal(fence_, fence_val_[idx]);
    }

    void wait_idle() {
        if (!queue_ || !fence_ || !fence_event_) return;
        UINT64 v = ++fence_counter_;
        if (SUCCEEDED(queue_->Signal(fence_, v))) {
            if (fence_->GetCompletedValue() < v) {
                fence_->SetEventOnCompletion(v, fence_event_);
                WaitForSingleObject(fence_event_, 200);
            }
        }
    }

public:
    void set_swap(IDXGISwapChain* s) { swap_ = s; }

private:
    static const UINT kMaxBuf = 8;
    SharedFrameReader reader_;
    IDXGISwapChain* swap_ = nullptr;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;     // гри, не наша
    ID3D12DescriptorHeap* rtv_heap_ = nullptr;
    ID3D12DescriptorHeap* srv_heap_ = nullptr;
    ID3D12Resource* back_[kMaxBuf] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_[kMaxBuf] = {};
    ID3D12CommandAllocator* alloc_[kMaxBuf] = {};
    UINT64 fence_val_[kMaxBuf] = {};
    ID3D12GraphicsCommandList* cmd_list_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_counter_ = 0;
    ID3D12Resource* tex_ = nullptr;
    ID3D12Resource* upload_ = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_ = {};
    UINT rows_ = 0; UINT64 row_bytes_ = 0, upload_bytes_ = 0;
    UINT buffers_ = 2, rtv_step_ = 0;
    DXGI_FORMAT rtv_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t tex_w_ = 0, tex_h_ = 0, tex_seq_ = 0;
    bool inited_ = false, srv_ok_ = false, enabled_ = false, logged_ = false;
    FrameView last_;
};

}  // namespace hominka
