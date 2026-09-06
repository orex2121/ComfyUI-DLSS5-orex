// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ComfyUI-DLSS5-NR contributors
//
// NVIDIA Optical Flow D3D11 integration.
//
// Ported from lisitskyaa/ComfyUI-DLSS5-NR (MIT). The public NVOF common ABI
// defines the session/init/execute structures and the S10.5 NV_OF_FLOW_VECTOR
// representation. The D3D11 function-table layout used here was independently
// exercised by the MIT-licensed NIGos/dlss5-bridge project. See
// THIRD_PARTY_NOTICES.md for attribution and license text.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nvof_flow.h"

using Microsoft::WRL::ComPtr;

namespace {

using NvOfStatus = long;
static constexpr NvOfStatus kOfSuccess = 0;
static constexpr NvOfStatus kOfRaised = 0x7FFFFFFF;
static constexpr uint32_t kOfApiVersion = 0x20; // public 2.0 ABI layout
static constexpr uint32_t kGrid = 2;            // 2x2 flow cells: good quality / cost balance
static constexpr uint32_t kPerf = 20;           // NV_OF_PERF_LEVEL_FAST

struct NvOfInitParams {
    uint32_t width;
    uint32_t height;
    uint32_t outGridSize;
    uint32_t hintGridSize;
    uint32_t mode;
    uint32_t perfLevel;
    uint32_t enableExternalHints;
    uint32_t enableOutputCost;
    void* hPrivData;
    uint32_t disparityRange;
    uint32_t enableRoi;
};

struct NvOfExecuteInputParams {
    void* inputFrame;
    void* referenceFrame;
    void* externalHints;
    uint32_t disableTemporalHints;
    uint32_t padding;
    void* hPrivData;
    uint32_t padding2;
    uint32_t numRois;
    void* roiData;
};

struct NvOfExecuteOutputParams {
    void* outputBuffer;
    void* outputCostBuffer;
    void* hPrivData;
};

static_assert(sizeof(NvOfInitParams) == 48, "Unexpected NVOF init ABI layout");
static_assert(offsetof(NvOfInitParams, hPrivData) == 32, "Unexpected NVOF init ABI layout");
static_assert(sizeof(NvOfExecuteInputParams) == 56, "Unexpected NVOF execute ABI layout");
static_assert(offsetof(NvOfExecuteInputParams, numRois) == 44, "Unexpected NVOF execute ABI layout");

using PFN_CreateInstance = NvOfStatus(__stdcall*)(uint32_t, void*);
using PFN_CreateD3D11 = NvOfStatus(__stdcall*)(ID3D11Device*, ID3D11DeviceContext*, void**);
using PFN_Init = NvOfStatus(__stdcall*)(void*, const NvOfInitParams*);
using PFN_Register = NvOfStatus(__stdcall*)(void*, ID3D11Resource*, void**);
using PFN_Unregister = NvOfStatus(__stdcall*)(void*);
using PFN_Execute = NvOfStatus(__stdcall*)(void*, const NvOfExecuteInputParams*, NvOfExecuteOutputParams*);
using PFN_Destroy = NvOfStatus(__stdcall*)(void*);
using PFN_LastError = NvOfStatus(__stdcall*)(void*, char*, uint32_t*);

struct OfaState {
    HMODULE lib = nullptr;
    void* slot[64]{};
    void* session = nullptr;
    void* reg_src[2]{};
    void* reg_flow = nullptr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> src[2];
    ComPtr<ID3D11Texture2D> flow;
    ComPtr<ID3D11Texture2D> flow_staging;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t flow_width = 0;
    uint32_t flow_height = 0;
    int current = 0;
    bool primed = false;

    LUID adapter_luid{};
    bool have_luid = false;
    std::vector<uint8_t> luma_bgra;
};

static OfaState g_ofa;

static const char* StatusName(NvOfStatus s) {
    switch (s) {
    case 0: return "SUCCESS";
    case 1: return "INVALID_PTR";
    case 2: return "INVALID_PARAM";
    case 3: return "INVALID_CALL";
    case 4: return "INVALID_VERSION";
    case 5: return "OUT_OF_MEMORY";
    case 6: return "NOT_INITIALIZED";
    case 7: return "UNSUPPORTED_FEATURE";
    case 8: return "GENERIC";
    case 9: return "OF_NOT_AVAILABLE";
    case 10: return "UNSUPPORTED_DEVICE";
    case 11: return "DEVICE_DOES_NOT_EXIST";
    default: return "UNKNOWN_STATUS";
    }
}

// The D3D11 table is a driver ABI. Guard calls so an unexpected driver/API
// mismatch is reported as an error instead of taking down ComfyUI.
static NvOfStatus SafeCreateInstance(PFN_CreateInstance fn, uint32_t version, void* slots) {
    __try { return fn(version, slots); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfRaised; }
}
static NvOfStatus SafeCreateSession(PFN_CreateD3D11 fn, ID3D11Device* d, ID3D11DeviceContext* c, void** out) {
    __try { return fn(d, c, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfRaised; }
}
static NvOfStatus SafeInit(PFN_Init fn, void* session, const NvOfInitParams* params) {
    __try { return fn(session, params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfRaised; }
}
static NvOfStatus SafeRegister(PFN_Register fn, void* session, ID3D11Resource* resource, void** out) {
    __try { return fn(session, resource, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfRaised; }
}
static NvOfStatus SafeUnregister(PFN_Unregister fn, void* handle) {
    __try { return fn(handle); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfRaised; }
}
static NvOfStatus SafeExecute(PFN_Execute fn, void* session, const NvOfExecuteInputParams* in, NvOfExecuteOutputParams* out) {
    __try { return fn(session, in, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfRaised; }
}
static NvOfStatus SafeDestroy(PFN_Destroy fn, void* session) {
    __try { return fn(session); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfRaised; }
}
static NvOfStatus SafeLastError(PFN_LastError fn, void* session, char* buf, uint32_t* cap) {
    __try { return fn(session, buf, cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfRaised; }
}

static std::string LastError() {
    if (!g_ofa.session || !g_ofa.slot[8]) return {};
    char buf[1024]{};
    uint32_t cap = static_cast<uint32_t>(sizeof(buf));
    auto fn = reinterpret_cast<PFN_LastError>(g_ofa.slot[8]);
    const NvOfStatus r = SafeLastError(fn, g_ofa.session, buf, &cap);
    if (r == kOfRaised) buf[0] = '\0';
    buf[sizeof(buf) - 1] = '\0';
    return std::string(buf);
}

static bool SameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static void CloseSessionInternal() {
    auto unreg = reinterpret_cast<PFN_Unregister>(g_ofa.slot[5]);
    if (unreg) {
        for (int i = 0; i < 2; ++i) {
            if (g_ofa.reg_src[i]) SafeUnregister(unreg, g_ofa.reg_src[i]);
        }
        if (g_ofa.reg_flow) SafeUnregister(unreg, g_ofa.reg_flow);
    }
    g_ofa.reg_src[0] = g_ofa.reg_src[1] = g_ofa.reg_flow = nullptr;

    if (g_ofa.session) {
        auto destroy = reinterpret_cast<PFN_Destroy>(g_ofa.slot[7]);
        if (destroy) SafeDestroy(destroy, g_ofa.session);
        g_ofa.session = nullptr;
    }

    g_ofa.src[0].Reset();
    g_ofa.src[1].Reset();
    g_ofa.flow.Reset();
    g_ofa.flow_staging.Reset();
    g_ofa.context.Reset();
    g_ofa.device.Reset();
    g_ofa.width = g_ofa.height = 0;
    g_ofa.flow_width = g_ofa.flow_height = 0;
    g_ofa.current = 0;
    g_ofa.primed = false;
    g_ofa.have_luid = false;
    g_ofa.luma_bgra.clear();
}

static bool EnsureFunctionTable(std::string& error) {
    if (g_ofa.lib && g_ofa.slot[0] && g_ofa.slot[1] && g_ofa.slot[4] && g_ofa.slot[5] && g_ofa.slot[6] && g_ofa.slot[7])
        return true;

    if (!g_ofa.lib) {
        g_ofa.lib = LoadLibraryW(L"nvofapi64.dll");
        if (!g_ofa.lib) {
            error = "NVIDIA Optical Flow: could not load nvofapi64.dll from the NVIDIA display driver.";
            return false;
        }
    }

    auto create = reinterpret_cast<PFN_CreateInstance>(GetProcAddress(g_ofa.lib, "NvOFAPICreateInstanceD3D11"));
    if (!create) {
        error = "NVIDIA Optical Flow: nvofapi64.dll does not export NvOFAPICreateInstanceD3D11.";
        return false;
    }

    memset(g_ofa.slot, 0, sizeof(g_ofa.slot));
    const NvOfStatus r = SafeCreateInstance(create, kOfApiVersion, g_ofa.slot);
    if (r != kOfSuccess) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "NVIDIA Optical Flow: API 0x20 function-table creation failed: 0x%08lX (%s).",
                      static_cast<unsigned long>(r), r == kOfRaised ? "driver call raised an exception" : StatusName(r));
        error = msg;
        return false;
    }
    if (!g_ofa.slot[0] || !g_ofa.slot[1] || !g_ofa.slot[4] || !g_ofa.slot[5] || !g_ofa.slot[6] || !g_ofa.slot[7]) {
        error = "NVIDIA Optical Flow: D3D11 function table is incomplete on this driver.";
        return false;
    }
    return true;
}

static ComPtr<ID3D11Texture2D> MakeTexture(
    ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
    D3D11_USAGE usage, UINT bind_flags, UINT cpu_access) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = usage;
    desc.BindFlags = bind_flags;
    desc.CPUAccessFlags = cpu_access;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex))) return nullptr;
    return tex;
}

static bool OpenSession(IDXGIAdapter1* adapter, uint32_t width, uint32_t height, std::string& error) {
    if (!adapter) {
        error = "NVIDIA Optical Flow: selected DXGI adapter is null.";
        return false;
    }
    if (!EnsureFunctionTable(error)) return false;

    DXGI_ADAPTER_DESC1 ad{};
    if (FAILED(adapter->GetDesc1(&ad))) {
        error = "NVIDIA Optical Flow: failed to query the selected DXGI adapter.";
        return false;
    }

    if (g_ofa.session && g_ofa.width == width && g_ofa.height == height &&
        g_ofa.have_luid && SameLuid(g_ofa.adapter_luid, ad.AdapterLuid)) {
        return true;
    }

    CloseSessionInternal();

    D3D_FEATURE_LEVEL feature_level{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        g_ofa.device.GetAddressOf(),
        &feature_level,
        g_ofa.context.GetAddressOf());
    if (FAILED(hr) || !g_ofa.device || !g_ofa.context) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "NVIDIA Optical Flow: D3D11CreateDevice on the selected NVIDIA adapter failed: 0x%08X.", static_cast<unsigned>(hr));
        error = msg;
        return false;
    }

    g_ofa.width = width;
    g_ofa.height = height;
    g_ofa.flow_width = (width + kGrid - 1) / kGrid;
    g_ofa.flow_height = (height + kGrid - 1) / kGrid;
    g_ofa.adapter_luid = ad.AdapterLuid;
    g_ofa.have_luid = true;

    const UINT io_bind = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    g_ofa.src[0] = MakeTexture(g_ofa.device.Get(), width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, io_bind, 0);
    g_ofa.src[1] = MakeTexture(g_ofa.device.Get(), width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, io_bind, 0);
    g_ofa.flow = MakeTexture(g_ofa.device.Get(), g_ofa.flow_width, g_ofa.flow_height, DXGI_FORMAT_R16G16_SINT, D3D11_USAGE_DEFAULT, io_bind, 0);
    g_ofa.flow_staging = MakeTexture(g_ofa.device.Get(), g_ofa.flow_width, g_ofa.flow_height, DXGI_FORMAT_R16G16_SINT, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ);
    if (!g_ofa.src[0] || !g_ofa.src[1] || !g_ofa.flow || !g_ofa.flow_staging) {
        error = "NVIDIA Optical Flow: failed to create D3D11 input/flow textures.";
        CloseSessionInternal();
        return false;
    }

    auto create_session = reinterpret_cast<PFN_CreateD3D11>(g_ofa.slot[0]);
    NvOfStatus r = SafeCreateSession(create_session, g_ofa.device.Get(), g_ofa.context.Get(), &g_ofa.session);
    if (r != kOfSuccess || !g_ofa.session) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "NVIDIA Optical Flow: could not open a session on the selected GPU: 0x%08lX (%s).",
                      static_cast<unsigned long>(r), r == kOfRaised ? "driver call raised an exception" : StatusName(r));
        error = msg;
        CloseSessionInternal();
        return false;
    }

    NvOfInitParams init{};
    init.width = width;
    init.height = height;
    init.outGridSize = kGrid;
    init.hintGridSize = kGrid;
    init.mode = 1;            // NV_OF_MODE_OPTICALFLOW
    init.perfLevel = kPerf;   // FAST
    init.enableExternalHints = 0;
    init.enableOutputCost = 0;
    init.hPrivData = nullptr;
    init.disparityRange = 0;
    init.enableRoi = 0;

    auto init_fn = reinterpret_cast<PFN_Init>(g_ofa.slot[1]);
    r = SafeInit(init_fn, g_ofa.session, &init);
    if (r != kOfSuccess) {
        const std::string detail = LastError();
        char msg[512];
        std::snprintf(msg, sizeof(msg), "NVIDIA Optical Flow: nvOFInit refused %ux%u grid %u / FAST: 0x%08lX (%s). %s",
                      width, height, kGrid, static_cast<unsigned long>(r),
                      r == kOfRaised ? "driver call raised an exception" : StatusName(r), detail.c_str());
        error = msg;
        CloseSessionInternal();
        return false;
    }

    auto reg = reinterpret_cast<PFN_Register>(g_ofa.slot[4]);
    NvOfStatus r0 = SafeRegister(reg, g_ofa.session, g_ofa.src[0].Get(), &g_ofa.reg_src[0]);
    NvOfStatus r1 = SafeRegister(reg, g_ofa.session, g_ofa.src[1].Get(), &g_ofa.reg_src[1]);
    NvOfStatus r2 = SafeRegister(reg, g_ofa.session, g_ofa.flow.Get(), &g_ofa.reg_flow);
    if (r0 != kOfSuccess || r1 != kOfSuccess || r2 != kOfSuccess ||
        !g_ofa.reg_src[0] || !g_ofa.reg_src[1] || !g_ofa.reg_flow) {
        const std::string detail = LastError();
        char msg[512];
        std::snprintf(msg, sizeof(msg), "NVIDIA Optical Flow: resource registration failed (%ld, %ld, %ld). %s",
                      r0, r1, r2, detail.c_str());
        error = msg;
        CloseSessionInternal();
        return false;
    }

    g_ofa.current = 0;
    g_ofa.primed = false;
    g_ofa.luma_bgra.resize(static_cast<size_t>(width) * height * 4);
    return true;
}

static void UploadLuma(const float* rgb) {
    const size_t pixels = static_cast<size_t>(g_ofa.width) * g_ofa.height;
    if (g_ofa.luma_bgra.size() != pixels * 4) g_ofa.luma_bgra.resize(pixels * 4);

    for (size_t i = 0; i < pixels; ++i) {
        const float r = std::clamp(rgb[i * 3 + 0], 0.0f, 1.0f);
        const float g = std::clamp(rgb[i * 3 + 1], 0.0f, 1.0f);
        const float b = std::clamp(rgb[i * 3 + 2], 0.0f, 1.0f);
        const float y = std::clamp(0.2126f * r + 0.7152f * g + 0.0722f * b, 0.0f, 1.0f);
        const uint8_t v = static_cast<uint8_t>(std::lround(y * 255.0f));
        g_ofa.luma_bgra[i * 4 + 0] = v;
        g_ofa.luma_bgra[i * 4 + 1] = v;
        g_ofa.luma_bgra[i * 4 + 2] = v;
        g_ofa.luma_bgra[i * 4 + 3] = 255;
    }

    g_ofa.context->UpdateSubresource(
        g_ofa.src[g_ofa.current].Get(), 0, nullptr,
        g_ofa.luma_bgra.data(), g_ofa.width * 4, 0);
}

} // namespace

bool NvofPrepareFrame(
    IDXGIAdapter1* adapter,
    const float* rgb,
    uint32_t width,
    uint32_t height,
    bool reset,
    NvofFlowFrame& out,
    std::string& error) {

    out = NvofFlowFrame{};
    error.clear();
    if (!rgb || width == 0 || height == 0) {
        error = "NVIDIA Optical Flow: invalid RGB frame or dimensions.";
        return false;
    }
    if (!OpenSession(adapter, width, height, error)) return false;

    if (reset) {
        // disableTemporalHints=1 makes the pairwise estimator independent of any
        // hidden prior driver history; resetting the ping-pong pair is therefore
        // sufficient to start a clean ComfyUI sequence.
        g_ofa.current = 0;
        g_ofa.primed = false;
    }

    UploadLuma(rgb);

    if (!g_ofa.primed) {
        // There is no previous frame. Prime one input slot and let DLSS receive
        // the explicit zero-MV texture for the first output frame.
        g_ofa.primed = true;
        g_ofa.current ^= 1;
        out.has_flow = false;
        out.grid = kGrid;
        return true;
    }

    NvOfExecuteInputParams in{};
    NvOfExecuteOutputParams exec_out{};
    in.inputFrame = g_ofa.reg_src[g_ofa.current];       // current
    in.referenceFrame = g_ofa.reg_src[g_ofa.current ^ 1]; // previous
    in.externalHints = nullptr;
    in.disableTemporalHints = 1;
    exec_out.outputBuffer = g_ofa.reg_flow;

    auto exec = reinterpret_cast<PFN_Execute>(g_ofa.slot[6]);
    const NvOfStatus r = SafeExecute(exec, g_ofa.session, &in, &exec_out);
    if (r != kOfSuccess) {
        const std::string detail = LastError();
        char msg[512];
        std::snprintf(msg, sizeof(msg), "NVIDIA Optical Flow: nvOFExecute failed: 0x%08lX (%s). %s",
                      static_cast<unsigned long>(r), r == kOfRaised ? "driver call raised an exception" : StatusName(r), detail.c_str());
        error = msg;
        return false;
    }

    // Copy to staging + Map is the synchronization point for the private D3D11
    // device. D3D11 NVOF handles device synchronization internally.
    g_ofa.context->CopyResource(g_ofa.flow_staging.Get(), g_ofa.flow.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = g_ofa.context->Map(g_ofa.flow_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "NVIDIA Optical Flow: flow staging Map failed: 0x%08X.", static_cast<unsigned>(hr));
        error = msg;
        return false;
    }

    out.has_flow = true;
    out.width = g_ofa.flow_width;
    out.height = g_ofa.flow_height;
    out.grid = kGrid;
    out.xy.resize(static_cast<size_t>(out.width) * out.height * 2);
    for (uint32_t y = 0; y < out.height; ++y) {
        const int16_t* src = reinterpret_cast<const int16_t*>(
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
        int16_t* dst = out.xy.data() + static_cast<size_t>(y) * out.width * 2;
        memcpy(dst, src, static_cast<size_t>(out.width) * 2 * sizeof(int16_t));
    }
    g_ofa.context->Unmap(g_ofa.flow_staging.Get(), 0);

    // Advance after successful delivery. Next frame writes the other input slot
    // and compares it against the frame just used as current.
    g_ofa.current ^= 1;
    return true;
}

void NvofReleaseSession() {
    CloseSessionInternal();
}

void NvofShutdown() {
    CloseSessionInternal();
    memset(g_ofa.slot, 0, sizeof(g_ofa.slot));
    if (g_ofa.lib) {
        FreeLibrary(g_ofa.lib);
        g_ofa.lib = nullptr;
    }
}

bool NvofDriverApiAvailable() {
    HMODULE lib = LoadLibraryW(L"nvofapi64.dll");
    if (!lib) return false;
    const bool ok = GetProcAddress(lib, "NvOFAPICreateInstanceD3D11") != nullptr;
    FreeLibrary(lib);
    return ok;
}

uint32_t NvofGridSize() { return kGrid; }
uint32_t NvofPerfLevel() { return kPerf; }
