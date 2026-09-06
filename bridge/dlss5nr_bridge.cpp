// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ComfyUI-DLSS5-NR contributors
//
// dlss5nr_bridge.dll — D3D12 + NGX bridge for the DLSS 5 Neural Rendering
// feature (NGX feature id 18), with an optional zero-copy CUDA interop path.
//
// Two per-frame data paths:
//
//   CPU staging (dlss5nr_process): float32 RGB over PCIe into an upload
//   buffer, D3D12 copy into the RGBA16F input texture, NGX evaluate, copy
//   the output texture to a readback buffer, float32 RGB back over PCIe.
//   Always available; used when CUDA interop is not usable.
//
//   CUDA interop (dlss5nr_process_cuda): PyTorch already owns float32 RGB
//   tensors on this GPU. The two frame textures live on explicit D3D12 heaps
//   created with D3D12_HEAP_FLAG_SHARED; each heap is shared with the CUDA
//   driver (nvcuda.dll, loaded at runtime — no toolkit dependency) through
//   the documented external-memory mechanism:
//
//     ID3D12Device::CreateSharedHandle(heap)
//       -> cuImportExternalMemory(CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP)
//       -> cuExternalMemoryGetMappedBuffer  (raw device pointer, persistent)
//
//   and two small kernels from embedded PTX convert between torch tensors and
//   two shared-heap LINEAR buffers that live in VRAM (v0.5.8):
//
//     torch RGB(f32) --kernel--> g_inbuf(RGBA16F linear, pitch = copyable)
//     CopyTextureRegion(g_inbuf -> g_color)   [driver translates the layout]
//     NGX evaluate (D3D12)
//     CopyTextureRegion(g_output -> g_outbuf) [driver translates the layout]
//     torch RGB(f32) <--kernel-- g_outbuf(RGBA16F linear, pitch = copyable)
//
//   v0.5.8: the kernels used to address the frame textures directly (mapped
//   heap base + GetCopyableFootprints offset/pitch). On field drivers (RTX
//   5090 / 616.56) a placed shared-heap texture's physical row layout does not
//   match its copyable footprint, so the raw addressing sheared every row by a
//   growing amount — fine and coarse H/V moire stripes in every output while
//   the CPU path (CopyTextureRegion only) stayed clean. Routing the pixels
//   through plain linear buffers removes the assumption entirely: the kernels
//   only ever touch memory with a known layout, and CopyTextureRegion lets
//   the driver translate to the texture's true physical layout — the exact
//   mechanism of the proven-clean CPU path. All data still stays in VRAM (the
//   two buffer<->texture copies are GPU-internal, no PCIe).
//
//   Ordering between the caller's CUDA stream and the D3D12 queue is done
//   CPU-side with two short stream syncs (v0.5.7). The v0.5.6 GPU-side
//   external-semaphore handoff crashed inside cuSignalExternalSemaphoresAsync
//   on field GPUs (RTX 5090 / driver 616.56, torch-hosted process) while the
//   standalone probe passed every step — a driver-internal state interaction
//   we cannot fix, so the crashing API is gone:
//
//     input kernel -> cuStreamSynchronize (CPU blocks ~tens of us)
//     -> record [inbuf->g_color copy] + NGX evaluate + [g_output->outbuf copy]
//        in one command list; ExecuteAndWait (CPU blocks for the inference;
//        its fence wait proves g_outbuf is fully written)
//     -> output kernel (left queued on the caller's stream, async tail)
//
//   The syncs are cheap next to the ~100 ms/frame inference. No PCIe pixel
//   traffic. The recorded copies transition g_color NPSSR->COPY_DEST->NPSSR
//   and g_output UA->COPY_SOURCE->UA, ending in exactly the states the CPU
//   path leaves behind, so the two paths can be mixed freely. The kernels run
//   in the CUDA primary
//   context of the same physical GPU (matched by name against the D3D12
//   adapter) — the context PyTorch uses by default — so raw tensor pointers
//   are directly addressable. If anything is unavailable (no nvcuda.dll, old
//   driver, multi-GPU mismatch), dlss5nr_cuda_supported() returns 0 and the
//   host falls back to the CPU staging path.
//
//   Optical flow (temporal != 0 in dlss5nr_process): NVIDIA Optical Flow
//   (nvofapi64.dll, shipped by the display driver — no user file) estimates
//   per-cell motion between consecutive raw input frames on a private D3D11
//   device created from the same adapter. The S10.5 grid is uploaded to a
//   full-resolution R16G16_FLOAT normalized-UV texture and passed to the
//   feature as DLSSNR.MVec. See nvof_flow.cpp.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "cuda_kernels_ptx.h"
#include "nvof_flow.h"

using Microsoft::WRL::ComPtr;
using NGXResult = int;
static constexpr NGXResult NGX_SUCCESS = 1;
static constexpr int NR_FEATURE_ID = 18;
static constexpr unsigned long long APP_ID = 141959980ULL;
static constexpr const char* PROJECT_ID = "53f803cc-a12f-4d69-90d5-19b7599cad19";

struct NGXHandle { unsigned int Id; };

// Minimal ABI-compatible interface used by the NVIDIA NGX parameter object.
struct NGXParameter {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NGXResult Get(const char*, unsigned long long*) const = 0;
    virtual NGXResult Get(const char*, float*) const = 0;
    virtual NGXResult Get(const char*, double*) const = 0;
    virtual NGXResult Get(const char*, unsigned int*) const = 0;
    virtual NGXResult Get(const char*, int*) const = 0;
    virtual NGXResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NGXResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NGXResult Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

struct NGXPathListInfo {
    wchar_t const* const* Path;
    unsigned int Length;
};
enum NGXLoggingLevel { NGX_LOG_OFF = 0, NGX_LOG_ON = 1, NGX_LOG_VERBOSE = 2 };
using NGXLogCallback = void(__cdecl*)(const char*, NGXLoggingLevel, int);
struct NGXLoggingInfo {
    NGXLoggingLevel LoggingLevel;
    NGXLogCallback Callback;
    void* UserData;
    bool DisableOtherLoggingSinks;
};
struct NGXFeatureCommonInfoInternal;
struct NGXFeatureCommonInfo {
    NGXPathListInfo PathListInfo;
    NGXFeatureCommonInfoInternal* InternalData;
    NGXLoggingInfo LoggingInfo;
};

using InitExtFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using SnippetInitFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
using InitProjectIdFn = NGXResult(__cdecl*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
using AllocParamsFn = NGXResult(__cdecl*)(NGXParameter**);
using DestroyParamsFn = NGXResult(__cdecl*)(NGXParameter*);
using CreateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvaluateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ReleaseFeatureFn = NGXResult(__cdecl*)(NGXHandle*);
using ShutdownFn = NGXResult(__cdecl*)();

using ShimInitFn = NGXResult(__cdecl*)(void*, unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using ShimCreateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using ShimEvaluateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ShimReleaseFn = NGXResult(__cdecl*)(void*, NGXHandle*);

// ---------------------------------------------------------------------------
// CUDA driver API — minimal ABI, loaded dynamically from nvcuda.dll so the
// DLL has no build-time or link-time dependency on the CUDA toolkit.
// ---------------------------------------------------------------------------
typedef int CUresult;
typedef void* CUcontext;
typedef unsigned int CUdevice;
typedef struct CUmod_st* CUmodule;
typedef struct CUfunc_st* CUfunction;
typedef struct CUstream_st* CUstream;

// External memory ABI, copied field-for-field from cuda.h (CUDA 12.x) so the
// DLL needs no toolkit headers. The layouts are ABI-stable (_v1 structs with
// reserved padding).
typedef struct CUexternalMemory_st* CUexternalMemory;

static constexpr int CU_EXT_MEM_TYPE_D3D12_HEAP = 4;      // CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP
static constexpr unsigned int CU_EXT_MEM_DEDICATED = 0x1; // CUDA_EXTERNAL_MEMORY_DEDICATED

struct CuExternalMemoryHandleDesc {
    int type;
    union {
        int fd;
        struct { void* handle; const void* name; } win32;
        const void* nvSciBufObject;
    } handle;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
};

struct CuExternalMemoryBufferDesc {
    unsigned long long offset;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
};

using CuInitFn = CUresult(__cdecl*)(unsigned int);
using CuDeviceGetCountFn = CUresult(__cdecl*)(int*);
using CuDeviceGetNameFn = CUresult(__cdecl*)(char*, int, CUdevice);
using CuDevicePrimaryCtxRetainFn = CUresult(__cdecl*)(CUcontext*, CUdevice);
using CuDevicePrimaryCtxReleaseFn = CUresult(__cdecl*)(CUdevice);
using CuCtxSetCurrentFn = CUresult(__cdecl*)(CUcontext);
using CuImportExternalMemoryFn = CUresult(__cdecl*)(CUexternalMemory*, const CuExternalMemoryHandleDesc*);
using CuGetMappedBufferFn = CUresult(__cdecl*)(unsigned long long*, CUexternalMemory, const CuExternalMemoryBufferDesc*);
using CuDestroyExternalMemoryFn = CUresult(__cdecl*)(CUexternalMemory);
using CuModuleLoadDataFn = CUresult(__cdecl*)(CUmodule*, const void*);
using CuModuleGetFunctionFn = CUresult(__cdecl*)(CUfunction*, CUmodule, const char*);
using CuLaunchKernelFn = CUresult(__cdecl*)(CUfunction, unsigned int, unsigned int, unsigned int,
                                           unsigned int, unsigned int, unsigned int, unsigned int,
                                           CUstream, void**, void**);
using CuStreamSyncFn = CUresult(__cdecl*)(CUstream);
using CuStreamGetCtxFn = CUresult(__cdecl*)(CUstream, CUcontext*);
using CuModuleUnloadFn = CUresult(__cdecl*)(CUmodule);
using CuGetErrorStringFn = CUresult(__cdecl*)(CUresult, const char**);

static std::mutex g_mutex;
static std::string g_last_error;
static std::wstring g_runtime_dir;
static int g_gpu_index = 0;
static std::string g_gpu_name = "unknown";
static bool g_initialized = false;

static HMODULE g_core_mod = nullptr;
static HMODULE g_nr_mod = nullptr;
static HMODULE g_shim_mod = nullptr;
static InitExtFn g_core_init_ext = nullptr;
static InitProjectIdFn g_core_init_project = nullptr;
static AllocParamsFn g_alloc_params = nullptr;
static DestroyParamsFn g_destroy_params = nullptr;  // optional; older cores may lack it
static CreateFeatureFn g_core_create = nullptr;
static EvaluateFeatureFn g_core_eval = nullptr;
static ReleaseFeatureFn g_core_release = nullptr;
static ShutdownFn g_core_shutdown = nullptr;
static SnippetInitFn g_nr_init = nullptr;
static CreateFeatureFn g_nr_create = nullptr;
static EvaluateFeatureFn g_nr_eval = nullptr;
static ReleaseFeatureFn g_nr_release = nullptr;
static ShimInitFn g_shim_init = nullptr;
static ShimCreateFn g_shim_create = nullptr;
static ShimEvaluateFn g_shim_eval = nullptr;
static ShimReleaseFn g_shim_release = nullptr;

static ComPtr<ID3D12Device> g_device;
static ComPtr<ID3D12CommandQueue> g_queue;
static ComPtr<ID3D12CommandAllocator> g_cmd_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_cmd;
static ComPtr<ID3D12Fence> g_fence;
static UINT64 g_fence_value = 0;

static NGXParameter* g_params = nullptr;
static NGXHandle* g_feature = nullptr;
// Frame textures are placed on explicit D3D12_HEAP_FLAG_SHARED heaps so the
// same allocation can be imported by CUDA as external memory (see below).
static ComPtr<ID3D12Heap> g_color_heap;
static ComPtr<ID3D12Heap> g_output_heap;
static ComPtr<ID3D12Resource> g_color;
static ComPtr<ID3D12Resource> g_output;
static ComPtr<ID3D12Resource> g_upload;
static ComPtr<ID3D12Resource> g_readback;
// v0.5.8: CUDA-path pixel staging (see the file header). The conversion
// kernels address these plain shared-heap linear buffers — row pitch is the
// copyable footprint's RowPitch, data starts at heap offset 0 — and
// CopyTextureRegion moves pixels buffer<->texture so the driver translates to
// the texture's true physical layout. Raw kernel addressing of the placed
// textures assumed physical pitch == copyable pitch, which field drivers
// violate (per-row shear -> H/V moire stripes in every output).
static ComPtr<ID3D12Heap> g_inbuf_heap;
static ComPtr<ID3D12Heap> g_outbuf_heap;
static ComPtr<ID3D12Resource> g_inbuf;   // created COPY_SOURCE (feeds the color copy)
static ComPtr<ID3D12Resource> g_outbuf;  // created COPY_DEST (receives the output copy)
static UINT64 g_buf_heap_bytes = 0;
static UINT g_width = 0, g_height = 0, g_row_pitch = 0;
static UINT64 g_total_bytes = 0;
static UINT64 g_tex_heap_bytes = 0;  // SizeInBytes of each frame-texture heap
static UINT64 g_tex_offset = 0;      // placed-resource offset inside the heap
static int g_feature_style = -999;
static int g_feature_preset = -999;

// Motion vectors (NVIDIA Optical Flow). The texture is a plain committed
// resource on purpose: it never leaves D3D12, so the shared-heap placed path
// — and its driver placement quirks — is not needed for it.
static ComPtr<ID3D12Resource> g_mvec;
static ComPtr<ID3D12Resource> g_mvec_upload;
static UINT64 g_mvec_row_pitch = 0;
static UINT64 g_mvec_total_bytes = 0;
static int g_feature_motion = -999;

// Session-level fallback for the experimental MVec contract. Some runtime
// builds reject the DLSSNR.MVec parameter key outright (CommandList::Close
// returns E_INVALIDARG on the very first frame). Once a frame fails while
// motion vectors are active, optical flow is latched off for the rest of the
// session and frames continue in auto mode so the video completes instead of
// throwing. g_motion_note carries the reason for dlss5nr_motion_status().
static bool g_motion_disabled = false;
static std::string g_motion_note;

// Set by the SEH trampolines when a native access violation is caught inside
// closed-source runtime code. A CPU-side AV (e.g. a write to 0x0) does not
// put the D3D12 device into removed state, so DeviceHealthy() alone cannot
// see the damage: the runtime/core internal state stays untrustworthy and
// every later evaluate crashes until ComfyUI restarts. Since v0.9.17 the
// process functions fail fast on this latch instead of attempting an
// in-process rebuild (observed twice to wedge). Cleared after a frame
// completes successfully (or on a fresh init).
static bool g_native_av_pending = false;

// The DXGI adapter the D3D12 device was created on. NVOF opens its private
// D3D11 device from this same physical GPU.
static ComPtr<IDXGIAdapter1> g_adapter;

// CUDA interop state.
static HMODULE g_cuda_mod = nullptr;
static bool g_cuda_available = false;
static std::string g_cuda_note;  // why interop is unavailable ("" when available)
static CuInitFn g_cu_init = nullptr;
static CuDeviceGetCountFn g_cu_device_get_count = nullptr;
static CuDeviceGetNameFn g_cu_device_get_name = nullptr;
static CuDevicePrimaryCtxRetainFn g_cu_primary_ctx_retain = nullptr;
static CuDevicePrimaryCtxReleaseFn g_cu_primary_ctx_release = nullptr;
static CuCtxSetCurrentFn g_cu_ctx_set_current = nullptr;
static CuImportExternalMemoryFn g_cu_import_mem = nullptr;
static CuGetMappedBufferFn g_cu_get_mapped_buffer = nullptr;
static CuDestroyExternalMemoryFn g_cu_destroy_mem = nullptr;
static CuModuleLoadDataFn g_cu_module_load_data = nullptr;
static CuModuleGetFunctionFn g_cu_module_get_function = nullptr;
static CuLaunchKernelFn g_cu_launch_kernel = nullptr;
static CuStreamSyncFn g_cu_stream_sync = nullptr;
static CuStreamGetCtxFn g_cu_stream_get_ctx = nullptr;
static CuModuleUnloadFn g_cu_module_unload = nullptr;
static CuGetErrorStringFn g_cu_get_error_string = nullptr;
static CUdevice g_cuda_device = 0;
static CUcontext g_cuda_ctx = nullptr;
static CUmodule g_ptx_module = nullptr;
static CUfunction g_fn_rgb_to_rgba16f = nullptr;
static CUfunction g_fn_rgba16f_to_rgb = nullptr;
// External-memory objects for the current staging-buffer heaps (valid only
// while g_width/g_height match g_reg_w/g_reg_h). The mapped device pointers
// stay valid until ReleaseCudaExternal(), so the per-frame path never maps
// or unmaps anything. v0.5.8: these are the linear staging buffers, not the
// frame-texture heaps — the kernels no longer address textures at all.
static CUexternalMemory g_cu_inbuf_mem = nullptr;
static CUexternalMemory g_cu_outbuf_mem = nullptr;
static void* g_cu_inbuf_ptr = nullptr;   // mapped in-buffer (heap offset 0)
static void* g_cu_outbuf_ptr = nullptr;  // mapped out-buffer (heap offset 0)
static UINT g_reg_w = 0, g_reg_h = 0;
// Caller stream of the last CUDA frame. Its tail (the deliberately async
// output kernel) may still be pending when a size change or shutdown triggers
// ReleaseCudaExternal, and destroying the external memory while that kernel
// still reads/writes the mapped heap is undefined behavior — so that stream
// is synchronized first.
static CUstream g_last_cuda_stream = nullptr;

static void SetError(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_last_error = buf;
}

static void CopyError(char* dst, int cap) {
    if (!dst || cap <= 0) return;
    const size_t n = std::min<size_t>(g_last_error.size(), static_cast<size_t>(cap - 1));
    memcpy(dst, g_last_error.data(), n);
    dst[n] = '\0';
}

static void CopyString(char* dst, int cap, const std::string& s) {
    if (!dst || cap <= 0) return;
    const size_t n = std::min<size_t>(s.size(), static_cast<size_t>(cap - 1));
    memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

static void SetCudaError(const char* what, CUresult r) {
    const char* s = nullptr;
    if (g_cu_get_error_string) g_cu_get_error_string(r, &s);
    SetError("%s failed: %s (0x%08X)", what, s ? s : "unknown", static_cast<unsigned>(r));
}

static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t c = a.back();
    if (c == L'\\' || c == L'/') return a + b;
    return a + L"\\" + b;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned long long FileTimeKey(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static HMODULE LoadCoreNGX(const std::wstring& runtime) {
    // 1) Explicit local override. This is also the quickest workaround if
    // DriverStore auto-discovery ever misses a vendor-specific INF name.
    const std::wstring local = Join(runtime, L"_nvngx.dll");
    if (FileExists(local)) {
        if (HMODULE m = LoadLibraryW(local.c_str())) return m;
    }

    // 2) Normal loader search (works on systems where NVIDIA exposes it).
    if (HMODULE m = LoadLibraryW(L"_nvngx.dll")) return m;

    // 3) NVIDIA ships NGX core inside the active display-driver package in
    // DriverStore. The INF prefix is NOT always nv_dispi: depending on OEM,
    // notebook/desktop package and driver generation it can be nvddi, nvaci,
    // nvhmui, etc. Scan every NVIDIA-looking *.inf_* package instead.
    wchar_t windows_dir[MAX_PATH] = {};
    UINT windows_len = GetWindowsDirectoryW(windows_dir, MAX_PATH);
    if (windows_len == 0 || windows_len >= MAX_PATH) return nullptr;
    const std::wstring repo = std::wstring(windows_dir) + L"\\System32\\DriverStore\\FileRepository";
    const std::wstring pat = repo + L"\\nv*.inf_*";

    struct Candidate {
        std::wstring path;
        unsigned long long stamp;
    };
    std::vector<Candidate> candidates;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

            std::wstring candidate = repo + L"\\" + fd.cFileName + L"\\_nvngx.dll";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fad) &&
                !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                candidates.push_back({candidate, FileTimeKey(fad.ftLastWriteTime)});
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Prefer the newest package. DriverStore often retains older drivers after
    // updates, and loading a stale NGX core is worse than not finding one.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.stamp > b.stamp; });

    for (const Candidate& c : candidates) {
        if (HMODULE m = LoadLibraryW(c.path.c_str())) return m;
    }

    return nullptr;
}

static ComPtr<ID3D12Device> CreateDevice(int nvidia_index) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

    int seen = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE) continue;
        if (seen++ != nvidia_index) continue;
        char gpu_utf8[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu_utf8, static_cast<int>(sizeof(gpu_utf8)), nullptr, nullptr);
        if (gpu_utf8[0]) g_gpu_name = gpu_utf8;
        g_adapter = adapter; // NVOF reuses this physical GPU for its D3D11 device.
        ComPtr<ID3D12Device> d;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d))))
            return d;
        return nullptr;
    }
    return nullptr;
}

static bool SetupD3D12() {
    g_device = CreateDevice(g_gpu_index);
    if (!g_device) { SetError("Could not create a D3D12 device for NVIDIA GPU index %d", g_gpu_index); return false; }

    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&g_queue)))) { SetError("CreateCommandQueue failed"); return false; }
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_alloc)))) { SetError("CreateCommandAllocator failed"); return false; }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&g_cmd)))) { SetError("CreateCommandList failed"); return false; }
    // D3D12_FENCE_FLAG_SHARED is retained from the v0.5.6 external-semaphore
    // design (this driver rejects CreateSharedHandle() on a FLAG_NONE fence
    // with E_INVALIDARG, probe_fence_shared.exe, v0.9.15). Ordering between
    // the caller's CUDA stream and this queue is CPU-side since v0.5.7, so
    // the flag is no longer required — it only enables sharing;
    // Signal/GetEvent/WaitForSingleObject semantics are the same, so
    // ExecuteAndWait/WaitQueueIdle need no changes.
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_fence)))) { SetError("CreateFence failed"); return false; }
    return true;
}

static bool ExecuteAndWait() {
    __try {
        HRESULT hr = g_cmd->Close();
        if (FAILED(hr)) {
            SetError("CommandList::Close failed (0x%08X)", static_cast<unsigned>(hr));
            // Nothing was submitted yet (Close precedes ExecuteCommandLists), so
            // the queue is untouched — just recycle the list. A caller retrying
            // the frame (e.g. the motion fallback) then starts from a clean list
            // instead of re-recording on top of the rejected commands.
            g_cmd_alloc->Reset();
            g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
            return false;
        }
        ID3D12CommandList* lists[] = { g_cmd.Get() };
        g_queue->ExecuteCommandLists(1, lists);
        ++g_fence_value;
        hr = g_queue->Signal(g_fence.Get(), g_fence_value);
        if (FAILED(hr)) { SetError("Queue::Signal failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
        bool done = true;
        if (g_fence->GetCompletedValue() < g_fence_value) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!ev) { SetError("CreateEvent failed"); return false; }
            g_fence->SetEventOnCompletion(g_fence_value, ev);
            DWORD w = WaitForSingleObject(ev, 30000);
            CloseHandle(ev);
            if (w != WAIT_OBJECT_0) {
                SetError("Timed out waiting for DLSS5 NR GPU work");
                done = false;
            }
        }
        // Always recycle the allocator so the next call starts from a clean list.
        // After a timeout the queue has very likely finished (the inference takes
        // milliseconds); if it truly hung, the driver TDR will handle it.
        g_cmd_alloc->Reset();
        g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
        return done;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // A raw access violation from the D3D12 driver — typically a device
        // that just entered removed state. Nothing was submitted before Close,
        // so the queue is untouched; recycle the list and report a clean error.
        SetError("Native access violation during command submission");
        g_cmd_alloc->Reset();
        g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
        return false;
    }
}

static void WaitQueueIdle() {
    if (!g_queue || !g_fence) return;
    __try {
        ++g_fence_value;
        if (SUCCEEDED(g_queue->Signal(g_fence.Get(), g_fence_value)) && g_fence->GetCompletedValue() < g_fence_value) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev) {
                g_fence->SetEventOnCompletion(g_fence_value, ev);
                WaitForSingleObject(ev, 30000);
                CloseHandle(ev);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Device removed: the queue cannot be drained. Callers detect a dead
        // device via GetDeviceRemovedReason and rebuild the session.
    }
}

// --- Native exception guard --------------------------------------------------
// The NGX runtime (nvngx_dlssnr.dll) and parts of the D3D12 driver are closed
// source. This runtime build has been observed to raise a raw access violation
// instead of an error code when presented with the experimental DLSSNR.MVec
// contract — which would take down the whole ComfyUI process, and leave the
// D3D12 device removed so that every later call fails until a restart. Every
// call into that territory is routed through one of these __try trampolines:
// an exception becomes a clean failure the fallback latch and session
// reinitialization can handle. The trampolines keep POD-only locals so they
// compile under /EHsc.

static int SehEvaluate() {
    __try {
        return g_shim_eval(reinterpret_cast<void*>(g_nr_eval), g_cmd.Get(), g_feature, g_params, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_native_av_pending = true;  // session state is untrustworthy from here on
        return -1;  // sentinel: native access violation
    }
}

static int SehCreateFeature() {
    __try {
        if (g_nr_create && g_shim_create)
            return g_shim_create(reinterpret_cast<void*>(g_nr_create), g_cmd.Get(), NR_FEATURE_ID, g_params, &g_feature);
        return g_core_create(g_cmd.Get(), NR_FEATURE_ID, g_params, &g_feature);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_native_av_pending = true;  // session state is untrustworthy from here on
        return -1;  // sentinel: native access violation
    }
}

static void SehReleaseFeature() {
    if (!g_feature) return;
    __try {
        if (g_nr_release && g_shim_release) g_shim_release(reinterpret_cast<void*>(g_nr_release), g_feature);
        else if (g_core_release) g_core_release(g_feature);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Device gone mid-teardown: the handle leaks, but the session is being
        // discarded anyway. Swallowing keeps the teardown running to completion.
    }
}

static void SehDestroyParams() {
    if (!g_params) return;
    __try { if (g_destroy_params) g_destroy_params(g_params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* same as above: leak, keep tearing down */ }
}

static void SehCoreShutdown() {
    if (!g_core_shutdown) return;
    __try { g_core_shutdown(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* session is being discarded anyway */ }
}

static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

static ComPtr<ID3D12Resource> CreateLinearBuffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static uint16_t FloatToHalf(float f) {
    uint32_t x; memcpy(&x, &f, sizeof(x));
    uint32_t s = (x >> 16) & 0x8000u;
    int32_t e = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(s);
        m = (m | 0x800000u) >> (1 - e);
        return static_cast<uint16_t>(s | (m >> 13));
    }
    if (e >= 31) return static_cast<uint16_t>(s | 0x7c00u);
    return static_cast<uint16_t>(s | (static_cast<uint32_t>(e) << 10) | (m >> 13));
}

static float HalfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    if (e == 0) {
        if (m == 0) x = s << 31;
        else {
            e = 1;
            while (!(m & 0x400)) { m <<= 1; --e; }
            m &= 0x3ff;
            x = (s << 31) | ((e + 112) << 23) | (m << 13);
        }
    } else if (e == 0x1f) x = (s << 31) | 0x7f800000u | (m << 13);
    else x = (s << 31) | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &x, sizeof(f)); return f;
}

// ---------------------------------------------------------------------------
// CUDA interop layer.
// ---------------------------------------------------------------------------

static std::string FoldGpuName(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool GpuNamesMatch(const std::string& a, const std::string& b) {
    const std::string x = FoldGpuName(a), y = FoldGpuName(b);
    if (x.empty() || y.empty()) return false;
    return x == y || x.find(y) != std::string::npos || y.find(x) != std::string::npos;
}

static void CleanupCudaModule() {
    // Defensive: by the time this runs, ReleaseFeatureAndResources() has
    // already released the external objects (it runs first in shutdown). If
    // init failed partway, some may still be alive — release what we can.
    if (g_cu_inbuf_mem && g_cu_destroy_mem) { g_cu_destroy_mem(g_cu_inbuf_mem); g_cu_inbuf_mem = nullptr; }
    if (g_cu_outbuf_mem && g_cu_destroy_mem) { g_cu_destroy_mem(g_cu_outbuf_mem); g_cu_outbuf_mem = nullptr; }
    g_cu_inbuf_ptr = g_cu_outbuf_ptr = nullptr;
    g_fn_rgb_to_rgba16f = nullptr;
    g_fn_rgba16f_to_rgb = nullptr;
    if (g_ptx_module && g_cu_module_unload) g_cu_module_unload(g_ptx_module);
    g_ptx_module = nullptr;
    if (g_cuda_ctx && g_cu_primary_ctx_release) g_cu_primary_ctx_release(g_cuda_device);
    g_cuda_ctx = nullptr;
    if (g_cuda_mod) { FreeLibrary(g_cuda_mod); g_cuda_mod = nullptr; }
    g_cu_init = nullptr;
    g_cu_device_get_count = nullptr;
    g_cu_device_get_name = nullptr;
    g_cu_primary_ctx_retain = nullptr;
    g_cu_primary_ctx_release = nullptr;
    g_cu_ctx_set_current = nullptr;
    g_cu_import_mem = nullptr;
    g_cu_get_mapped_buffer = nullptr;
    g_cu_destroy_mem = nullptr;
    g_cu_module_load_data = nullptr;
    g_cu_module_get_function = nullptr;
    g_cu_launch_kernel = nullptr;
    g_cu_stream_sync = nullptr;
    g_cu_stream_get_ctx = nullptr;
    g_cu_module_unload = nullptr;
    g_cu_get_error_string = nullptr;
    g_last_cuda_stream = nullptr;
    g_cuda_available = false;
}

// Best-effort: never fails the bridge init. On any problem the CPU staging
// path remains fully functional and dlss5nr_cuda_supported() reports 0.
static bool InitCudaInterop() {
    g_cuda_note.clear();
    auto fail = [](const char* msg) {
        g_cuda_note = msg;
        CleanupCudaModule();
    };

    g_cuda_mod = LoadLibraryW(L"nvcuda.dll");
    if (!g_cuda_mod) { fail("nvcuda.dll not found (no NVIDIA display driver?)"); return false; }

#define LOADCU(sym, field) \
    field = reinterpret_cast<decltype(field)>(GetProcAddress(g_cuda_mod, sym));
    LOADCU("cuInit", g_cu_init);
    LOADCU("cuDeviceGetCount", g_cu_device_get_count);
    LOADCU("cuDeviceGetName", g_cu_device_get_name);
    LOADCU("cuDevicePrimaryCtxRetain", g_cu_primary_ctx_retain);
    LOADCU("cuDevicePrimaryCtxRelease", g_cu_primary_ctx_release);
    LOADCU("cuCtxSetCurrent", g_cu_ctx_set_current);
    // D3D12 interop goes through external memory — the documented mechanism.
    // (Note: cuGraphicsD3D12RegisterResource does NOT exist in the CUDA driver
    // API; only D9/D10/D11 graphics registration is exported by nvcuda.dll.)
    LOADCU("cuImportExternalMemory", g_cu_import_mem);
    LOADCU("cuExternalMemoryGetMappedBuffer", g_cu_get_mapped_buffer);
    LOADCU("cuDestroyExternalMemory", g_cu_destroy_mem);
    LOADCU("cuModuleLoadData", g_cu_module_load_data);
    LOADCU("cuModuleGetFunction", g_cu_module_get_function);
    LOADCU("cuLaunchKernel", g_cu_launch_kernel);
    LOADCU("cuStreamSynchronize", g_cu_stream_sync);
    // Optional diagnostic (not in the required-export list): validates that
    // the caller's stream belongs to our primary context before we launch on
    // it. Absent only on very old drivers, where the check is skipped.
    LOADCU("cuStreamGetCtx", g_cu_stream_get_ctx);
    LOADCU("cuModuleUnload", g_cu_module_unload);
    LOADCU("cuGetErrorString", g_cu_get_error_string);
#undef LOADCU

    if (!g_cu_init || !g_cu_device_get_count || !g_cu_device_get_name ||
        !g_cu_primary_ctx_retain || !g_cu_primary_ctx_release || !g_cu_ctx_set_current ||
        !g_cu_import_mem || !g_cu_get_mapped_buffer || !g_cu_destroy_mem ||
        !g_cu_module_load_data || !g_cu_module_get_function ||
        !g_cu_launch_kernel || !g_cu_stream_sync) {
        fail("nvcuda.dll is missing required driver API exports (external memory set)");
        return false;
    }

    CUresult r = g_cu_init(0);
    if (r != 0) { SetCudaError("cuInit", r); fail(g_last_error.c_str()); return false; }

    // Find the CUDA device that is our D3D12 GPU. Do NOT assume index parity:
    // match by name instead, which is stable across driver/OS enumeration
    // orders and multi-GPU setups.
    int count = 0;
    r = g_cu_device_get_count(&count);
    if (r != 0) { SetCudaError("cuDeviceGetCount", r); fail(g_last_error.c_str()); return false; }
    bool found = false;
    for (int i = 0; i < count && !found; ++i) {
        char name[256] = {};
        if (g_cu_device_get_name(name, sizeof(name), static_cast<CUdevice>(i)) != 0) continue;
        if (GpuNamesMatch(g_gpu_name, name)) { g_cuda_device = static_cast<CUdevice>(i); found = true; }
    }
    if (!found) {
        char msg[512];
        snprintf(msg, sizeof(msg), "no CUDA device matches D3D12 GPU '%s' (CUDA sees %d device(s))", g_gpu_name.c_str(), count);
        fail(msg);
        return false;
    }

    // PyTorch uses the primary context by default; retain the same one so raw
    // tensor pointers are addressable from our kernels.
    r = g_cu_primary_ctx_retain(&g_cuda_ctx, g_cuda_device);
    if (r != 0 || !g_cuda_ctx) { SetCudaError("cuDevicePrimaryCtxRetain", r); fail(g_last_error.c_str()); return false; }
    r = g_cu_ctx_set_current(g_cuda_ctx);
    if (r != 0) { SetCudaError("cuCtxSetCurrent", r); fail(g_last_error.c_str()); return false; }

    r = g_cu_module_load_data(&g_ptx_module, kDlss5NrKernelPtx);
    if (r != 0) {
        SetCudaError("cuModuleLoadData(embedded PTX)", r);
        fail(g_last_error.c_str());
        return false;
    }
    r = g_cu_module_get_function(&g_fn_rgb_to_rgba16f, g_ptx_module, "dlss5nr_rgb_to_rgba16f");
    if (r != 0 || !g_fn_rgb_to_rgba16f) { SetCudaError("cuModuleGetFunction(rgb_to_rgba16f)", r); fail(g_last_error.c_str()); return false; }
    r = g_cu_module_get_function(&g_fn_rgba16f_to_rgb, g_ptx_module, "dlss5nr_rgba16f_to_rgb");
    if (r != 0 || !g_fn_rgba16f_to_rgb) { SetCudaError("cuModuleGetFunction(rgba16f_to_rgb)", r); fail(g_last_error.c_str()); return false; }

    // v0.5.7: the D3D12-fence external semaphore that used to be imported here
    // is gone — its signal call (cuSignalExternalSemaphoresAsync) crashed on
    // field GPUs, and ordering between the caller's stream and our queue is now
    // CPU-side (see the file header).

    g_cuda_available = true;
    return true;
}

// Must run while the D3D12 heaps are still alive: it is called from
// ReleaseFeatureAndResources() before the ComPtrs are reset, and from
// shutdown after the queue has been drained. Destroying the external memory
// releases the driver's reference to the heaps held through the shared
// handles; the stream sync first waits for the previous frame's async output
// kernel (see g_last_cuda_stream), which still reads/writes the mapped heap.
static void ReleaseCudaExternal() {
    g_reg_w = g_reg_h = 0;
    if (!g_cuda_mod) return;
    if (g_cu_ctx_set_current) g_cu_ctx_set_current(g_cuda_ctx);
    // Wait for the previous frame's async output kernel before destroying the
    // external memory objects. Only happens on size change or shutdown.
    if (g_cu_stream_sync && g_last_cuda_stream) {
        CUresult r = g_cu_stream_sync(g_last_cuda_stream);
        if (r != 0) SetCudaError("cuStreamSynchronize(before release)", r);
    }
    g_last_cuda_stream = nullptr;
    if (g_cu_inbuf_mem && g_cu_destroy_mem) { g_cu_destroy_mem(g_cu_inbuf_mem); g_cu_inbuf_mem = nullptr; }
    if (g_cu_outbuf_mem && g_cu_destroy_mem) { g_cu_destroy_mem(g_cu_outbuf_mem); g_cu_outbuf_mem = nullptr; }
    g_cu_inbuf_ptr = g_cu_outbuf_ptr = nullptr;
}

// Import one shared D3D12 heap as CUDA external memory and map it to a raw
// device pointer. The NT handle from CreateSharedHandle is closed after the
// import: ownership is NOT transferred, the driver keeps its own reference
// for the lifetime of the external memory object. v0.5.8: this imports the
// linear staging-buffer heaps (the kernels no longer address texture heaps).
static bool ImportSharedHeap(ID3D12Heap* heap, UINT64 heap_bytes, UINT64 res_offset,
                             CUexternalMemory* memOut, void** ptrOut, const char* which) {
    // Defensive: on any early failure the out-params must read as "no import",
    // never as driver-written garbage.
    *memOut = nullptr;
    *ptrOut = nullptr;
    // cuda.h: the D3D12_HEAP external-memory handle must be "a valid shared NT
    // handle returned by ID3D12Device::CreateSharedHandle when referring to a
    // ID3D12Heap object". The CUDA driver maps the heap for read+write.
    //
    // v0.9.15: this driver only accepts GENERIC_ALL here — the previous
    // GENERIC_READ|GENERIC_WRITE mask failed with E_INVALIDARG even on a
    // D3D12_HEAP_FLAG_SHARED heap (probe_fence_shared.exe). The heaps are
    // created shared in AllocateFrameResources, so sharing is permitted.
    HANDLE h = nullptr;
    HRESULT shr = g_device->CreateSharedHandle(heap, nullptr, GENERIC_ALL, nullptr, &h);
    if (FAILED(shr)) {
        SetError("CreateSharedHandle(%s heap) failed: HRESULT 0x%08X on adapter %s",
                 which, static_cast<unsigned>(shr), g_gpu_name.c_str());
        return false;
    }

    CuExternalMemoryHandleDesc md{};
    md.type = CU_EXT_MEM_TYPE_D3D12_HEAP;
    md.handle.win32.handle = h;
    md.size = heap_bytes;
    md.flags = CU_EXT_MEM_DEDICATED;  // single resource in a dedicated allocation

    CUresult r = g_cu_import_mem(memOut, &md);
    CloseHandle(h);
    if (r != 0 || !*memOut) { SetCudaError((std::string("cuImportExternalMemory(") + which + ")").c_str(), r); return false; }

    CuExternalMemoryBufferDesc bd{};
    bd.offset = 0;
    bd.size = heap_bytes;
    unsigned long long dev_ptr = 0;
    r = g_cu_get_mapped_buffer(&dev_ptr, *memOut, &bd);
    if (r != 0 || !dev_ptr) { SetCudaError((std::string("cuExternalMemoryGetMappedBuffer(") + which + ")").c_str(), r); return false; }

    // The placed resource starts at res_offset inside the heap.
    *ptrOut = reinterpret_cast<void*>(dev_ptr + res_offset);
    return true;
}

// Import the current staging-buffer heaps into CUDA and validate the mapping.
// Called once per size change (buffers were just allocated). The kernels use
// 8-byte (uint2) accesses, so the base pointer and row pitch must be
// 8-byte aligned; otherwise fall back to the CPU path.
static bool RegisterCudaBuffers() {
    CUresult r = g_cu_ctx_set_current(g_cuda_ctx);
    if (r != 0) { SetCudaError("cuCtxSetCurrent", r); return false; }

    if (!ImportSharedHeap(g_inbuf_heap.Get(), g_buf_heap_bytes, 0, &g_cu_inbuf_mem, &g_cu_inbuf_ptr, "in-buffer")) {
        ReleaseCudaExternal();
        return false;
    }
    if (!ImportSharedHeap(g_outbuf_heap.Get(), g_buf_heap_bytes, 0, &g_cu_outbuf_mem, &g_cu_outbuf_ptr, "out-buffer")) {
        ReleaseCudaExternal();
        return false;
    }

    const size_t pitch = static_cast<size_t>(g_row_pitch);
    for (int i = 0; i < 2; ++i) {
        const char* which = i == 0 ? "in-buffer" : "out-buffer";
        void* p = i == 0 ? g_cu_inbuf_ptr : g_cu_outbuf_ptr;
        if (pitch < static_cast<size_t>(g_width) * 8u) {
            SetError("CUDA mapped %s is too small (pitch %llu bytes for %ux%u RGBA16F)", which,
                     static_cast<unsigned long long>(pitch), g_width, g_height);
            ReleaseCudaExternal();
            return false;
        }
        if (reinterpret_cast<uintptr_t>(p) % 8 != 0 || pitch % 8 != 0) {
            SetError("CUDA mapped %s is not 8-byte aligned (base 0x%llX, pitch %llu); "
                     "the conversion kernels require it", which,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)),
                     static_cast<unsigned long long>(pitch));
            ReleaseCudaExternal();
            return false;
        }
    }

    g_reg_w = g_width;
    g_reg_h = g_height;
    return true;
}

static bool LaunchConversion(CUfunction fn, const void* src, void* dst, size_t pitch, int w, int h, CUstream stream) {
    uint64_t s = reinterpret_cast<uintptr_t>(src);
    uint64_t d = reinterpret_cast<uintptr_t>(dst);
    uint64_t p = static_cast<uint64_t>(pitch);
    int wi = w, hi = h;
    void* args[] = { &s, &d, &p, &wi, &hi };
    const unsigned gx = (static_cast<unsigned>(w) + 31u) / 32u;
    const unsigned gy = (static_cast<unsigned>(h) + 7u) / 8u;
    CUresult r = g_cu_launch_kernel(fn, gx, gy, 1, 32, 8, 1, 0, stream, args, nullptr);
    if (r != 0) { SetCudaError("cuLaunchKernel", r); return false; }
    return true;
}

// Turn off the CUDA path for the rest of the session. g_last_error must
// already hold the specific failure; it is preserved as the status note.
static void DisableCuda() {
    if (g_cuda_available) {
        g_cuda_note = "disabled after runtime error: " + g_last_error;
        g_cuda_available = false;
    }
}

// Uploads one NVOF flow frame into g_mvec as full-resolution normalized-UV
// motion vectors (R16G16_FLOAT). The grid values are S10.5 fixed-point
// displacements in 1/32-pixel units, current -> previous; DLSSNR.MVec wants
// uv = displacement / frame_size per axis, nearest-upsampled from the grid.
// flow == nullptr uploads an explicit zero-MV texture — what the first frame
// and scene resets need. execute_now=false leaves the copy queued on g_cmd
// so the caller's NGX evaluate runs in the same submission.
static bool UploadMotionVectorTexture(const NvofFlowFrame* flow, bool execute_now) {
    void* mapped = nullptr;
    HRESULT hr = g_mvec_upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) { SetError("MVec upload Map failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    memset(mapped, 0, g_mvec_total_bytes);

    if (flow && flow->has_flow) {
        const uint32_t gw = flow->width;   // ceil(width / grid)
        const uint32_t gh = flow->height;  // ceil(height / grid)
        const uint32_t cell = flow->grid ? flow->grid : 2u;
        if (gw > 0 && gh > 0 && flow->xy.size() >= static_cast<size_t>(gw) * gh * 2) {
            for (UINT y = 0; y < g_height; ++y) {
                const uint32_t gy = std::min(y / cell, gh - 1);
                auto* row = reinterpret_cast<uint16_t*>(mapped) + static_cast<size_t>(y) * (g_mvec_row_pitch / 2);
                for (UINT x = 0; x < g_width; ++x) {
                    const uint32_t gx = std::min(x / cell, gw - 1);
                    const size_t i = (static_cast<size_t>(gy) * gw + gx) * 2;
                    const float uvx = flow->xy[i + 0] * (1.0f / 32.0f) / static_cast<float>(g_width);
                    const float uvy = flow->xy[i + 1] * (1.0f / 32.0f) / static_cast<float>(g_height);
                    row[x * 2 + 0] = FloatToHalf(uvx);
                    row[x * 2 + 1] = FloatToHalf(uvy);
                }
            }
        } else {
            // Malformed flow frame: keep the zero MVs rather than feed garbage.
        }
    }
    g_mvec_upload->Unmap(0, nullptr);

    auto b1 = Barrier(g_mvec.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_cmd->ResourceBarrier(1, &b1);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_mvec.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_mvec_upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
    src.PlacedFootprint.Footprint.Width = g_width; src.PlacedFootprint.Footprint.Height = g_height;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(g_mvec_row_pitch);
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto b2 = Barrier(g_mvec.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_cmd->ResourceBarrier(1, &b2);
    return !execute_now || ExecuteAndWait();
}

static void ReleaseFeatureAndResources() {
    WaitQueueIdle();
    ReleaseCudaExternal();
    SehReleaseFeature();
    g_feature = nullptr;
    // The parameter table is owned by the feature: it was allocated for this
    // exact resource set, so it is destroyed with the feature and a fresh
    // object is allocated on the next rebuild. No stale keys (e.g. a DLSSNR.MVec
    // resource pointer from a previous motion-mode session) can survive a mode
    // switch or size change. On cores without NVSDK_NGX_D3D12_DestroyParameters
    // the object simply leaks per rebuild — rare and small.
    SehDestroyParams();
    g_params = nullptr;
    g_color.Reset(); g_output.Reset(); g_upload.Reset(); g_readback.Reset();
    g_mvec.Reset(); g_mvec_upload.Reset();
    g_color_heap.Reset(); g_output_heap.Reset();
    g_inbuf.Reset(); g_outbuf.Reset();
    g_inbuf_heap.Reset(); g_outbuf_heap.Reset();
    g_width = g_height = g_row_pitch = 0;
    g_total_bytes = 0;
    g_tex_heap_bytes = g_tex_offset = 0;
    g_buf_heap_bytes = 0;
    g_mvec_row_pitch = g_mvec_total_bytes = 0;
    g_feature_style = -999; g_feature_preset = -999; g_feature_motion = -999;
}

static bool AllocateFrameResources(UINT w, UINT h, bool use_motion_vectors) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // The exact footprint the driver will use for copies and placed resources.
    // The CPU staging path copies with this same pitch, so both paths stay
    // consistent regardless of how the driver rounds the row size.
    // Note: GetCopyableFootprints returns void (out-params only).
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT nr_rows = 0;
    UINT64 row_pitch = 0, required = 0;
    g_device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &nr_rows, &row_pitch, &required);

    // Explicit shared heaps. v0.5.8: CUDA interop no longer imports these
    // frame heaps (the kernels address the linear staging-buffer heaps
    // instead), but keeping them shared costs nothing — committed-vs-placed
    // performance is identical — so the CPU path pays nothing for it.
    //
    // Shared-heap sizes must be a multiple of 64 KB
    // (D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT), and the heap must hold the
    // driver's true placement footprint. On this GPU/driver (RTX 5090, 616.56)
    // GetResourceAllocationInfo returns {0, 1} for these descriptions, so the
    // placement is estimated directly: the driver rounds the height up to a
    // multiple of 128 rows (pitch x ceil(h/128) x 128). Field data fits this
    // exactly across all observed sizes — every failure had h % 128 != 0,
    // every success h % 128 == 0 (768x1344 failed where 896x1152 with the
    // identical copyable total passed; 1344x1728 needed 294 x 64 KB).
    D3D12_RESOURCE_ALLOCATION_INFO alloc = g_device->GetResourceAllocationInfo(1, 0, &d);

    const UINT h_placement = ((h + 127) / 128) * 128;
    const UINT64 placement_bytes = row_pitch * static_cast<UINT64>(h_placement);

    UINT64 heap_bytes = required;
    const UINT64 data_end = fp.Offset + row_pitch * static_cast<UINT64>(h);
    if (data_end > heap_bytes) heap_bytes = data_end;
    if (placement_bytes > heap_bytes) heap_bytes = placement_bytes;
    UINT64 alloc_size = alloc.SizeInBytes;
    if (alloc.Alignment > 1) {
        alloc_size = (alloc_size + alloc.Alignment - 1) & ~(alloc.Alignment - 1);
    }
    if (alloc_size > heap_bytes) heap_bytes = alloc_size;
    heap_bytes = (heap_bytes + 0xFFFFull) & ~0xFFFFull;

    // Create one shared-heap + placed-texture pair, growing the heap in
    // 64 KB steps if the driver rejects the placement. The size only ever
    // grows, so the second pair starts from what the first one proved.
    auto create_frame_pair = [&](const char* name, D3D12_RESOURCE_STATES state,
                                 ComPtr<ID3D12Heap>& heap_out,
                                 ComPtr<ID3D12Resource>& tex_out) -> bool {
        for (int attempt = 0; ; ++attempt) {
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes = heap_bytes;
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Flags = D3D12_HEAP_FLAG_SHARED;

            HRESULT hr = g_device->CreateHeap(&hd, IID_PPV_ARGS(&heap_out));
            if (SUCCEEDED(hr)) {
                hr = g_device->CreatePlacedResource(heap_out.Get(), fp.Offset, &d, state,
                                                    nullptr, IID_PPV_ARGS(&tex_out));
            }
            if (SUCCEEDED(hr)) return true;
            heap_out.Reset();
            tex_out.Reset();
            if (attempt >= 31) {
                SetError("CreatePlacedResource(%s) failed after %d x 64 KB growth: 0x%08X "
                         "(w=%u h=%u pitch=%llu offset=%llu copyable=%llu alloc=%llu align=%llu heap=%llu)",
                         name, attempt, static_cast<unsigned>(hr), w, h,
                         (unsigned long long)row_pitch, (unsigned long long)fp.Offset,
                         (unsigned long long)required, (unsigned long long)alloc.SizeInBytes,
                         (unsigned long long)alloc.Alignment,
                         (unsigned long long)heap_bytes);
                return false;
            }
            heap_bytes += 0x10000ull;
        }
    };

    if (!create_frame_pair("color", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g_color_heap, g_color)) return false;
    if (!create_frame_pair("output", D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_output_heap, g_output)) return false;

    g_row_pitch = static_cast<UINT>(fp.Footprint.RowPitch);
    g_tex_heap_bytes = heap_bytes;
    g_tex_offset = fp.Offset;
    g_total_bytes = static_cast<UINT64>(g_row_pitch) * h;
    g_upload = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g_readback = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g_upload || !g_readback) { SetError("Failed to create D3D12 upload/readback buffers"); return false; }

    // v0.5.8: shared DEFAULT-heap linear staging buffers for the CUDA path (see
    // the globals). Each sits at offset 0 of its own dedicated shared heap, so
    // the imported pointer needs no offset and the heap size is just the data
    // rounded up to the 64 KB shared-heap alignment — no texture-placement
    // growth loop needed (buffers have no driver placement quirk).
    const UINT64 buf_bytes = (g_total_bytes + 0xFFFFull) & ~0xFFFFull;
    g_buf_heap_bytes = buf_bytes;
    auto create_shared_buffer = [&](D3D12_RESOURCE_STATES state, ComPtr<ID3D12Heap>& heap_out,
                                    ComPtr<ID3D12Resource>& buf_out, const char* name) -> bool {
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = buf_bytes;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Flags = D3D12_HEAP_FLAG_SHARED;
        HRESULT hr = g_device->CreateHeap(&hd, IID_PPV_ARGS(&heap_out));
        if (FAILED(hr)) {
            SetError("CreateHeap(%s staging buffer) failed: 0x%08X", name, static_cast<unsigned>(hr));
            return false;
        }
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = g_total_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = g_device->CreatePlacedResource(heap_out.Get(), 0, &bd, state, nullptr, IID_PPV_ARGS(&buf_out));
        if (FAILED(hr)) {
            SetError("CreatePlacedResource(%s staging buffer) failed: 0x%08X", name, static_cast<unsigned>(hr));
            heap_out.Reset();
            return false;
        }
        return true;
    };
    if (!create_shared_buffer(D3D12_RESOURCE_STATE_COPY_SOURCE, g_inbuf_heap, g_inbuf, "in")) return false;
    if (!create_shared_buffer(D3D12_RESOURCE_STATE_COPY_DEST, g_outbuf_heap, g_outbuf, "out")) return false;

    if (use_motion_vectors) {
        // Committed resource on purpose: the shared-heap placed path exists
        // only for CUDA external-memory import, and this texture never leaves
        // D3D12.
        D3D12_RESOURCE_DESC md{};
        md.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        md.Width = w; md.Height = h; md.DepthOrArraySize = 1; md.MipLevels = 1;
        md.Format = DXGI_FORMAT_R16G16_FLOAT; md.SampleDesc.Count = 1;
        md.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &md,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_mvec)))) {
            SetError("Failed to create R16G16_FLOAT motion-vector texture");
            return false;
        }
        g_mvec_row_pitch = (static_cast<UINT64>(w) * 4u + 255u) & ~255ull;
        g_mvec_total_bytes = g_mvec_row_pitch * h;
        g_mvec_upload = CreateLinearBuffer(g_mvec_total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!g_mvec_upload) { SetError("Failed to create motion-vector upload buffer"); return false; }
        // Feature creation always starts with a defined zero-MV resource.
        if (!UploadMotionVectorTexture(nullptr, true)) return false;
    }

    g_width = w; g_height = h;
    return true;
}

static void SetCommonParams(int style, int preset, float intensity, float tone, float structure, float skin, int automask, int reset, bool use_motion_vectors) {
    g_params->Set("DLSSNR.Width", g_width);
    g_params->Set("DLSSNR.Height", g_height);
    g_params->Set("DLSSNR.Enabled", 1);
    g_params->Set("DLSSNR.Reset", reset);
    g_params->Set("DLSSNR.Style", style);
    g_params->Set("DLSSNR.Hint.Render.Preset", preset);
    g_params->Set("DLSSNR.Intensity", intensity);
    g_params->Set("DLSSNR.LocalToneStrength", tone);
    g_params->Set("DLSSNR.LocalStructureStrength", structure);
    g_params->Set("DLSSNR.SkinStructureStrength", skin);
    g_params->Set("DLSSNR.UseAutoMask", automask);
    g_params->Set("DLSSNR.UICorrection", 0);
    g_params->Set("DLSSNR.DepthInverted", 1);
    g_params->Set("DLSSNR.ScalingRatio", 1.0f);
    if (use_motion_vectors && g_mvec) {
        // Full-resolution normalized-UV vectors: scale = frame size, subrect
        // covers the whole frame.
        g_params->Set("DLSSNR.MVec", g_mvec.Get());
        g_params->Set("DLSSNR.MVecScaleX", static_cast<float>(g_width));
        g_params->Set("DLSSNR.MVecScaleY", static_cast<float>(g_height));
        g_params->Set("DLSSNR.MVecSubrectBaseX", 0);
        g_params->Set("DLSSNR.MVecSubrectBaseY", 0);
        g_params->Set("DLSSNR.MVecSubrectWidth", g_width);
        g_params->Set("DLSSNR.MVecSubrectHeight", g_height);
    } else {
        // No motion input. This runtime build rejects the very presence of the
        // DLSSNR.MVec key in the table (CommandList::Close returns
        // E_INVALIDARG), so mirror the verified-working configuration exactly:
        // unit scale only, no MVec resource or subrect keys at all — what
        // NVIDIA's own CLI diagnostic log shows for a motionless run.
        g_params->Set("DLSSNR.MVecScaleX", 1.0f);
        g_params->Set("DLSSNR.MVecScaleY", 1.0f);
    }
    g_params->Set("DLSSNR.Color", g_color.Get());
    g_params->Set("DLSSNR.Output", g_output.Get());
    g_params->Set("DLSSNR.Backbuffer", g_output.Get());
    g_params->Set("DLSSNR.ColorSubrectBaseX", 0);
    g_params->Set("DLSSNR.ColorSubrectBaseY", 0);
    g_params->Set("DLSSNR.ColorSubrectWidth", g_width);
    g_params->Set("DLSSNR.ColorSubrectHeight", g_height);
    g_params->Set("DLSSNR.OutputSubrectBaseX", 0);
    g_params->Set("DLSSNR.OutputSubrectBaseY", 0);
    g_params->Set("DLSSNR.OutputSubrectWidth", g_width);
    g_params->Set("DLSSNR.OutputSubrectHeight", g_height);
}

static bool EnsureFeature(UINT w, UINT h, int style, int preset, float intensity, float tone, float structure, float skin, int automask, bool use_motion_vectors) {
    const int motion_key = use_motion_vectors ? 1 : 0;
    const bool rebuild = !g_feature || w != g_width || h != g_height || style != g_feature_style || preset != g_feature_preset || motion_key != g_feature_motion;
    if (!rebuild) {
        SetCommonParams(style, preset, intensity, tone, structure, skin, automask, 0, use_motion_vectors);
        return true;
    }

    ReleaseFeatureAndResources();
    // Record from a clean list: a previous frame may have died mid-recording
    // (native exception in evaluate), leaving partial commands behind. The
    // queue was just drained by WaitQueueIdle, so nothing submitted references
    // these allocator pages.
    g_cmd_alloc->Reset();
    g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
    if (!AllocateFrameResources(w, h, use_motion_vectors)) return false;
    // Fresh parameter object for this resource set (the previous one was
    // destroyed with the feature above). If allocation fails here, the next
    // call rebuilds from scratch again — self-healing.
    NGXResult ar = g_alloc_params(&g_params);
    if (ar != NGX_SUCCESS || !g_params) { SetError("NVSDK_NGX_D3D12_AllocateParameters failed: 0x%08X", static_cast<unsigned>(ar)); return false; }
    SetCommonParams(style, preset, intensity, tone, structure, skin, automask, 1, use_motion_vectors);

    const int cr_raw = SehCreateFeature();
    if (cr_raw == -1) {
        // The runtime raised a raw access violation instead of an error code.
        // g_feature may hold a half-created handle — drop it; the next call
        // rebuilds from scratch.
        SetError("Native access violation inside CreateFeature(18)");
        g_feature = nullptr;
        return false;
    }
    if (static_cast<NGXResult>(cr_raw) != NGX_SUCCESS || !g_feature) {
        SetError("CreateFeature(18) failed: 0x%08X. Check GPU support, driver, nvngx_dlssnr.dll, and caller shim.", static_cast<unsigned>(cr_raw));
        g_feature = nullptr;
        return false;
    }
    g_feature_style = style;
    g_feature_preset = preset;
    g_feature_motion = motion_key;
    return true;
}

static bool LoadNGX() {
    g_core_mod = LoadCoreNGX(g_runtime_dir);
    if (!g_core_mod) {
        SetError("Could not load NVIDIA NGX core _nvngx.dll. Tried runtime\\_nvngx.dll, normal DLL search, and NVIDIA DriverStore packages matching nv*.inf_*. You can copy the _nvngx.dll from your active NVIDIA DriverStore folder into runtime\\_nvngx.dll as an explicit override.");
        return false;
    }

    const std::wstring nr_path = Join(g_runtime_dir, L"nvngx_dlssnr.dll");
    if (!FileExists(nr_path)) { SetError("nvngx_dlssnr.dll not found in runtime folder"); return false; }
    g_nr_mod = LoadLibraryW(nr_path.c_str());
    if (!g_nr_mod) { SetError("LoadLibrary(nvngx_dlssnr.dll) failed: Win32 %lu", GetLastError()); return false; }

    std::wstring shim_path = Join(Join(g_runtime_dir, L"caller"), L"nvngx.dll_comfy.dll");
    if (!FileExists(shim_path)) {
        // Backward-compatible fallback for older builds.
        shim_path = Join(Join(g_runtime_dir, L"caller"), L"nvngx.dll");
    }
    if (!FileExists(shim_path)) { SetError("caller shim not found (expected caller\\nvngx.dll_comfy.dll)"); return false; }
    g_shim_mod = LoadLibraryW(shim_path.c_str());
    if (!g_shim_mod) { SetError("LoadLibrary(caller shim) failed: Win32 %lu", GetLastError()); return false; }

    g_core_init_ext = reinterpret_cast<InitExtFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_Ext"));
    g_core_init_project = reinterpret_cast<InitProjectIdFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_ProjectID"));
    g_alloc_params = reinterpret_cast<AllocParamsFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_AllocateParameters"));
    // Optional: present in the current driver cores (export right after
    // AllocateParameters). Without it a parameter object leaks per feature
    // rebuild — small, and only on older cores.
    g_destroy_params = reinterpret_cast<DestroyParamsFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_DestroyParameters"));
    g_core_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_CreateFeature"));
    g_core_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g_core_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));
    g_core_shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Shutdown"));

    g_nr_init = reinterpret_cast<SnippetInitFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_Init_Ext"));
    g_nr_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_CreateFeature"));
    g_nr_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g_nr_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));

    g_shim_init = reinterpret_cast<ShimInitFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallInit"));
    g_shim_create = reinterpret_cast<ShimCreateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallCreate"));
    g_shim_eval = reinterpret_cast<ShimEvaluateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallEvaluate"));
    g_shim_release = reinterpret_cast<ShimReleaseFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallRelease"));

    if (!g_core_init_ext || !g_alloc_params || !g_core_create || !g_core_eval || !g_core_release || !g_core_shutdown) {
        SetError("Required NGX core exports are missing"); return false;
    }
    if (!g_nr_init || !g_nr_create || !g_nr_eval || !g_nr_release) {
        SetError("Required DLSSNR exports are missing from nvngx_dlssnr.dll"); return false;
    }
    if (!g_shim_init || !g_shim_create || !g_shim_eval || !g_shim_release) {
        SetError("Required caller shim exports are missing"); return false;
    }
    return true;
}

static bool InitNGXSession() {
    const wchar_t* paths[1] = { g_runtime_dir.c_str() };
    NGXPathListInfo pli{ paths, 1 };
    NGXFeatureCommonInfo fci{};
    fci.PathListInfo = pli;
    fci.LoggingInfo.LoggingLevel = NGX_LOG_OFF;

    bool core_ok = false;
    if (g_core_init_project) {
        for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
            NGXResult r = g_core_init_project(PROJECT_ID, 0, "0.2.0", g_runtime_dir.c_str(), g_device.Get(), ver, nullptr);
            core_ok = (r == NGX_SUCCESS);
        }
    }
    if (!core_ok) {
        for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
            NGXResult r = g_core_init_ext(APP_ID, g_runtime_dir.c_str(), g_device.Get(), ver, &fci);
            core_ok = (r == NGX_SUCCESS);
        }
    }
    if (!core_ok) { SetError("NGX core initialization failed for API versions 0x13..0x20"); return false; }

    NGXResult sr = g_shim_init(reinterpret_cast<void*>(g_nr_init), APP_ID, g_runtime_dir.c_str(), g_device.Get(), 0x15, &fci);
    if (sr != NGX_SUCCESS) {
        wchar_t shim_self[MAX_PATH] = L"<unknown>";
        GetModuleFileNameW(g_shim_mod, shim_self, MAX_PATH);
        char shim_utf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, shim_self, -1, shim_utf8, static_cast<int>(sizeof(shim_utf8)), nullptr, nullptr);
        SetError("DLSSNR snippet Init_Ext via caller shim failed: 0x%08X; loaded shim=%s", static_cast<unsigned>(sr), shim_utf8);
        return false;
    }

    NGXResult ar = g_alloc_params(&g_params);
    if (ar != NGX_SUCCESS || !g_params) {
        SetError("NVSDK_NGX_D3D12_AllocateParameters failed: 0x%08X", static_cast<unsigned>(ar));
        return false;
    }
    return true;
}

static void ShutdownUnlocked() {
    // Releases the CUDA external objects while the D3D12 heaps are still
    // alive, then releases feature/heaps/textures/buffers.
    ReleaseFeatureAndResources();
    SehCoreShutdown();
    g_params = nullptr;
    g_device.Reset(); g_queue.Reset(); g_cmd_alloc.Reset(); g_cmd.Reset(); g_fence.Reset();
    // The NVOF D3D11 session lives on the same adapter; drop it once the
    // D3D12 device is gone.
    NvofShutdown();
    if (g_shim_mod) FreeLibrary(g_shim_mod);
    if (g_nr_mod) FreeLibrary(g_nr_mod);
    if (g_core_mod) FreeLibrary(g_core_mod);
    g_shim_mod = g_nr_mod = g_core_mod = nullptr;

    g_core_init_ext = nullptr;
    g_core_init_project = nullptr;
    g_alloc_params = nullptr;
    g_destroy_params = nullptr;
    g_core_create = nullptr;
    g_core_eval = nullptr;
    g_core_release = nullptr;
    g_core_shutdown = nullptr;
    g_nr_init = nullptr;
    g_nr_create = nullptr;
    g_nr_eval = nullptr;
    g_nr_release = nullptr;
    g_shim_init = nullptr;
    g_shim_create = nullptr;
    g_shim_eval = nullptr;
    g_shim_release = nullptr;

    // CUDA: PTX module and primary-context reference go last. Textures were
    // already unregistered above, so nothing references our D3D12 device here.
    if (g_cuda_mod) CleanupCudaModule();
    g_cuda_note.clear();

    g_initialized = false;
}

// True while the D3D12 device can still be used. After a native access
// violation in the runtime/driver the device typically enters removed state
// and every subsequent call fails until it is recreated.
static bool DeviceHealthy() {
    return g_device.Get() != nullptr && SUCCEEDED(g_device->GetDeviceRemovedReason());
}

// Full session rebuild from the stored init parameters (runtime dir + GPU
// index). Used after a native crash left the D3D12 device removed: without it
// every frame fails until ComfyUI restarts. Mirrors dlss5nr_init minus the
// argument parsing; the motion latch is session state the callers manage.
// Safe to call while holding g_mutex from any prior state — ShutdownUnlocked
// is a no-op on pristine globals, and SetupD3D12 recreates the adapter/queue/
// fence that NVOF and CUDA interop depend on.
static bool ReinitUnlocked() {
    ShutdownUnlocked();
    if (g_runtime_dir.empty()) { SetError("No runtime directory stored; cannot reinitialize"); return false; }
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!SetupD3D12() || !LoadNGX() || !InitNGXSession()) {
        const std::string reason = g_last_error.empty() ? std::string("unknown") : g_last_error;
        ShutdownUnlocked();
        SetError("Bridge reinitialization failed after native crash: %s", reason.c_str());
        return false;
    }
    g_initialized = true;
    // Best-effort, mirrors dlss5nr_init: failure only disables the fast path.
    InitCudaInterop();
    return true;
}

extern "C" {

__declspec(dllexport) const char* __cdecl dlss5nr_version() {
    return "0.5.8-bufcopy";
}

__declspec(dllexport) const char* __cdecl dlss5nr_gpu_name() {
    return g_gpu_name.c_str();
}

// NVIDIA Optical Flow availability and fixed settings (informational; the
// host uses these to decide whether temporal_mode=optical_flow can be honored).
__declspec(dllexport) int __cdecl dlss5nr_nvof_available(void) {
    return NvofDriverApiAvailable() ? 1 : 0;
}

__declspec(dllexport) int __cdecl dlss5nr_nvof_grid(void) {
    return static_cast<int>(NvofGridSize());
}

__declspec(dllexport) int __cdecl dlss5nr_nvof_perf(void) {
    return static_cast<int>(NvofPerfLevel());
}

__declspec(dllexport) int __cdecl dlss5nr_cuda_supported(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    return (g_initialized && g_cuda_available) ? 1 : 0;
}

// Returns 1 when the CUDA interop path is active, 0 otherwise. The buffer
// receives "ok" or a human-readable reason for the fallback — useful for
// surfacing why a machine silently uses the slower CPU staging path.
__declspec(dllexport) int __cdecl dlss5nr_cuda_status(char* buf, int cap) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized) { CopyString(buf, cap, "bridge not initialized"); return 0; }
    if (g_cuda_available) { CopyString(buf, cap, "ok"); return 1; }
    CopyString(buf, cap, g_cuda_note.empty() ? std::string("unavailable") : g_cuda_note);
    return 0;
}

// Returns 1 while optical flow is active for this session, 0 once the
// fallback latch has tripped. The buffer receives "ok" or a human-readable
// reason — the host prints it so a video that silently continued in auto
// mode stays visible in the log.
__declspec(dllexport) int __cdecl dlss5nr_motion_status(char* buf, int cap) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized) { CopyString(buf, cap, "bridge not initialized"); return 0; }
    if (!g_motion_disabled) { CopyString(buf, cap, "ok"); return 1; }
    CopyString(buf, cap, g_motion_note.empty() ? std::string("disabled") : g_motion_note);
    return 0;
}

__declspec(dllexport) int __cdecl dlss5nr_init(int gpu_index, const wchar_t* runtime_dir, char* err, int err_cap) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_last_error.clear();
    g_motion_disabled = false;
    g_motion_note.clear();
    g_native_av_pending = false;
    if (g_initialized) { CopyError(err, err_cap); return 1; }
    if (!runtime_dir || !*runtime_dir) { SetError("runtime_dir is empty"); CopyError(err, err_cap); return 0; }

    g_gpu_index = gpu_index;
    g_runtime_dir = runtime_dir;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)co; // RPC_E_CHANGED_MODE is harmless for this use.

    if (!SetupD3D12() || !LoadNGX() || !InitNGXSession()) {
        ShutdownUnlocked();
        CopyError(err, err_cap);
        return 0;
    }
    g_initialized = true;

    // Best-effort: failure only disables the fast path, never the bridge.
    InitCudaInterop();

    CopyError(err, err_cap);
    return 1;
}

__declspec(dllexport) int __cdecl dlss5nr_process(
    const float* rgb_in, float* rgb_out, int width, int height,
    int style, int preset, float intensity, float tone, float structure, float skin,
    int automask, int reset, int temporal, char* err, int err_cap) {

    std::lock_guard<std::mutex> guard(g_mutex);
    g_last_error.clear();
    if (!g_initialized) { SetError("DLSS5 NR bridge is not initialized"); CopyError(err, err_cap); return 0; }
    if (!rgb_in || !rgb_out || width <= 0 || height <= 0) { SetError("Invalid image buffer/dimensions"); CopyError(err, err_cap); return 0; }
    if (width > 16384 || height > 16384) { SetError("Image dimensions are unreasonably large"); CopyError(err, err_cap); return 0; }

    if (g_native_av_pending) {
        // v0.9.17: a previous frame died with a caught access violation inside
        // closed-source runtime code. The runtime/core state is untrustworthy,
        // and the in-process session rebuild that used to follow has been
        // observed twice to wedge (blocked on a GPU fence, zero CPU/GPU load) —
        // so fail fast with restart advice instead of rebuilding into the same
        // poisoned state.
        SetError("session corrupted by a previous native crash — RESTART ComfyUI before the next run");
        CopyError(err, err_cap);
        return 0;
    }
    if (!DeviceHealthy()) {
        // The D3D12 device entered removed state without a caught access
        // violation (driver reset, TDR, ...): rebuilding the session from the
        // stored init parameters is safe and necessary — every frame would fail
        // until it happens. Latch optical flow off while we are at it: if the
        // failure was MVec-related, retrying it would just fail again.
        if (!g_motion_disabled) {
            g_motion_disabled = true;
            g_motion_note = "optical flow disabled after native crash; bridge reinitialized, continuing in auto mode";
        }
        if (!ReinitUnlocked()) { CopyError(err, err_cap); return 0; }
    }

    const bool use_motion_vectors = temporal != 0 && !g_motion_disabled;
    // Leaving motion mode frees the NVOF D3D11 session (its textures and the
    // loaded driver module) until it is requested again.
    if (!use_motion_vectors) NvofReleaseSession();

    // The per-frame body as a lambda so an experimental-contract failure can
    // be retried once in auto mode (see below).
    auto run_frame = [&](bool use_mv) -> bool {
        if (!EnsureFeature(static_cast<UINT>(width), static_cast<UINT>(height), style, preset, intensity, tone, structure, skin, automask, use_mv)) return false;

        if (use_mv) {
            // Flow is estimated on the raw input frames (not NR output) so the
            // estimator never sees its own feedback. reset=1 also restarts the
            // NVOF ping-pong pair, matching the luma-diff scene cuts.
            NvofFlowFrame flow;
            std::string of_error;
            if (!NvofPrepareFrame(g_adapter.Get(), rgb_in, static_cast<UINT>(width), static_cast<UINT>(height), reset != 0, flow, of_error)) {
                SetError("%s", of_error.c_str());
                return false;
            }
            // execute_now=false: the MVec copy queues on g_cmd right before the
            // color upload and the NGX evaluate in this same submission.
            if (!UploadMotionVectorTexture(flow.has_flow ? &flow : nullptr, false)) return false;
        }

        SetCommonParams(style, preset, intensity, tone, structure, skin, automask, reset ? 1 : 0, use_mv);

        void* mapped = nullptr;
        HRESULT hr = g_upload->Map(0, nullptr, &mapped);
        if (FAILED(hr) || !mapped) { SetError("Upload buffer Map failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
        memset(mapped, 0, static_cast<size_t>(g_total_bytes));
        auto* dst_base = static_cast<uint8_t*>(mapped);
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<uint16_t*>(dst_base + static_cast<size_t>(y) * g_row_pitch);
            const float* src = rgb_in + static_cast<size_t>(y) * width * 3;
            for (int x = 0; x < width; ++x) {
                row[x * 4 + 0] = FloatToHalf(std::clamp(src[x * 3 + 0], 0.0f, 1.0f));
                row[x * 4 + 1] = FloatToHalf(std::clamp(src[x * 3 + 1], 0.0f, 1.0f));
                row[x * 4 + 2] = FloatToHalf(std::clamp(src[x * 3 + 2], 0.0f, 1.0f));
                row[x * 4 + 3] = FloatToHalf(1.0f);
            }
        }
        g_upload->Unmap(0, nullptr);

        auto b1 = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        g_cmd->ResourceBarrier(1, &b1);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_color.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = g_upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        src.PlacedFootprint.Footprint.Width = width; src.PlacedFootprint.Footprint.Height = height;
        src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
        g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto b2 = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_cmd->ResourceBarrier(1, &b2);

        const int er_raw = SehEvaluate();
        if (er_raw == -1) {
            // The runtime raised a raw access violation instead of an error
            // code (observed with this build's MVec contract). Nothing was
            // submitted, but the list may hold partially recorded commands —
            // recycle it so no later frame re-submits them. If the device is
            // removed, the entry health check rebuilds everything anyway.
            SetError("Native access violation inside DLSSNR evaluate");
            g_cmd_alloc->Reset();
            g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
            return false;
        }
        if (static_cast<NGXResult>(er_raw) != NGX_SUCCESS) {
            SetError("DLSSNR EvaluateFeature failed: 0x%08X", static_cast<unsigned>(er_raw));
            // Reset command list to a clean state before returning.
            ExecuteAndWait();
            return false;
        }

        auto b3 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_cmd->ResourceBarrier(1, &b3);
        D3D12_TEXTURE_COPY_LOCATION rd{};
        rd.pResource = g_readback.Get(); rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.PlacedFootprint.Footprint.Width = width; rd.PlacedFootprint.Footprint.Height = height;
        rd.PlacedFootprint.Footprint.Depth = 1; rd.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
        D3D12_TEXTURE_COPY_LOCATION rs{};
        rs.pResource = g_output.Get(); rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g_cmd->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
        auto b4 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_cmd->ResourceBarrier(1, &b4);

        if (!ExecuteAndWait()) return false;

        void* rmap = nullptr;
        hr = g_readback->Map(0, nullptr, &rmap);
        if (FAILED(hr) || !rmap) { SetError("Readback Map failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
        const auto* base = static_cast<const uint8_t*>(rmap);
        for (int y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * g_row_pitch);
            float* dstf = rgb_out + static_cast<size_t>(y) * width * 3;
            for (int x = 0; x < width; ++x) {
                // Return the resource channels exactly as stored. Some stock/reference
                // DLSSNR builds have been observed to produce B,G,R,A while patched
                // Ada builds may produce R,G,B,A. Python selects/auto-detects the
                // correct interpretation instead of hard-coding a swap here.
                dstf[x * 3 + 0] = std::clamp(HalfToFloat(row[x * 4 + 0]), 0.0f, 1.0f);
                dstf[x * 3 + 1] = std::clamp(HalfToFloat(row[x * 4 + 1]), 0.0f, 1.0f);
                dstf[x * 3 + 2] = std::clamp(HalfToFloat(row[x * 4 + 2]), 0.0f, 1.0f);
            }
        }
        g_readback->Unmap(0, nullptr);
        return true;
    };

    bool ok = run_frame(use_motion_vectors);
    if (!ok && use_motion_vectors) {
        // The frame failed while motion vectors were active. A caught access
        // violation leaves the session corrupted and no longer rebuilds
        // in-process (v0.9.17 — the rebuild has been observed to wedge), so it
        // fails fast with restart advice. Only a clean rejection of the
        // experimental MVec contract (typically CommandList::Close E_INVALIDARG
        // on the very first frame) latches optical flow off and retries the
        // SAME frame in auto mode so the video completes instead of throwing;
        // subsequent frames skip NVOF entirely. (Optical flow was removed from
        // the UI in v0.9.15, so this block is dormant.)
        if (g_native_av_pending) {
            SetError("session corrupted by a previous native crash — RESTART ComfyUI before the next run");
            CopyError(err, err_cap);
            return 0;
        }
        g_motion_disabled = true;
        g_motion_note = "optical flow disabled after frame error (" + g_last_error + "); continuing in auto mode";
        NvofReleaseSession();
        ok = run_frame(false);
    }
    if (ok) g_native_av_pending = false;
    CopyError(err, err_cap);
    return ok ? 1 : 0;
}

// --- SEH guard for the whole per-frame CUDA body ----------------------------
// v0.9.16: the first field run of the zero-copy path died with a raw access
// violation ("reading 0x8") inside dlss5nr_process_cuda on frame 0. CPython's
// Win32 exception converter turned it into an OSError, but the interrupted
// driver call left the D3D12/CUDA session poisoned (the next run hung at
// ~20%). The standalone handshake probe passes every step at full frame size
// on the same GPU/driver, so the fault is a driver-internal state interaction
// this process shape triggers; we cannot fix the driver, only contain it.
// Every native call in the per-frame body now runs inside one trampoline: a
// caught access violation becomes a clean error (the stage names the culprit
// for the field log), CUDA interop is disabled for this session, and the
// session is marked untrustworthy so every later call fails fast with restart
// advice instead of taking ComfyUI down. v0.9.17: the second field run pinned
// the crash to cuSignalExternalSemaphoresAsync (stage 'signal fence (cuda)')
// and showed that the post-crash CPU-staging retry wedged inside the session
// rebuild — so a native AV now fails the whole run with restart advice instead
// of falling back, and the semaphore itself is gone (ordering is CPU-side, see
// the file header).

enum CudaAvStage {
    CUDA_AV_NONE = 0,
    CUDA_AV_ENSURE_FEATURE,
    CUDA_AV_SET_PARAMS,
    CUDA_AV_REGISTER_BUFFERS,
    CUDA_AV_CTX_CURRENT,
    CUDA_AV_STREAM_CTX_CHECK,
    CUDA_AV_INPUT_KERNEL,
    CUDA_AV_INPUT_SYNC,
    CUDA_AV_INPUT_COPY,
    CUDA_AV_EVALUATE,
    CUDA_AV_OUTPUT_COPY,
    CUDA_AV_EXECUTE,
    CUDA_AV_OUTPUT_KERNEL,
};

static int g_cuda_av_stage = CUDA_AV_NONE;

static const char* CudaAvStageName(int stage) {
    switch (stage) {
        case CUDA_AV_ENSURE_FEATURE:     return "ensure feature";
        case CUDA_AV_SET_PARAMS:        return "set params";
        case CUDA_AV_REGISTER_BUFFERS:  return "register cuda buffers";
        case CUDA_AV_CTX_CURRENT:       return "cuCtxSetCurrent";
        case CUDA_AV_STREAM_CTX_CHECK:  return "stream context check";
        case CUDA_AV_INPUT_KERNEL:      return "input conversion kernel";
        case CUDA_AV_INPUT_SYNC:        return "input stream sync (cuda)";
        case CUDA_AV_INPUT_COPY:        return "input buffer->texture copy (d3d12)";
        case CUDA_AV_EVALUATE:          return "evaluate (ngx)";
        case CUDA_AV_OUTPUT_COPY:       return "output texture->buffer copy (d3d12)";
        case CUDA_AV_EXECUTE:           return "execute/wait (d3d12)";
        case CUDA_AV_OUTPUT_KERNEL:     return "output conversion kernel";
        default:                        return "unknown";
    }
}

struct CudaFrameParams {
    uint64_t src_dev;
    uint64_t dst_dev;
    int width;
    int height;
    CUstream stream;
    int style;
    int preset;
    float intensity;
    float tone;
    float structure;
    float skin;
    int automask;
    int reset;
};

// Runs from the trampoline's __except handler, so every step is individually
// guarded: with a corrupted context cuStreamSynchronize or cuDestroy* can
// raise again, and that must not take the process down. Leftover external
// objects leak in the worst case — they die with the process, and the host
// refuses to reuse this session anyway (fail-fast latch).
static void CudaAvCleanup(CUstream stream) {
    // Drain the caller's stream first: an orphaned input kernel may still be
    // queued there, and it writes the mapped heap. The kernel is a pure GPU op
    // with no D3D12 dependency, so this cannot deadlock.
    if (g_cu_stream_sync && stream) {
        __try { g_cu_stream_sync(stream); }
        __except (EXCEPTION_EXECUTE_HANDLER) { /* context already bad; leak */ }
    }
    __try { ReleaseCudaExternal(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cu_inbuf_ptr = g_cu_outbuf_ptr = nullptr;
        g_reg_w = g_reg_h = 0;
    }
}

// Returns 0 on success, 1 on any failure (g_last_error already set). POD-only
// locals so it compiles under /EHsc; the functions it calls may use C++
// objects, this __try scope may not.
static int SehCudaFrame(const CudaFrameParams* p) {
    __try {
        g_cuda_av_stage = CUDA_AV_ENSURE_FEATURE;
        if (!EnsureFeature(static_cast<UINT>(p->width), static_cast<UINT>(p->height), p->style, p->preset,
                           p->intensity, p->tone, p->structure, p->skin, p->automask, false))
            return 1;

        g_cuda_av_stage = CUDA_AV_SET_PARAMS;
        SetCommonParams(p->style, p->preset, p->intensity, p->tone, p->structure, p->skin, p->automask, p->reset ? 1 : 0, false);

        g_cuda_av_stage = CUDA_AV_REGISTER_BUFFERS;
        if (g_reg_w != static_cast<UINT>(p->width) || g_reg_h != static_cast<UINT>(p->height)) {
            if (!RegisterCudaBuffers()) { DisableCuda(); return 1; }
        }

        g_cuda_av_stage = CUDA_AV_CTX_CURRENT;
        CUresult r = g_cu_ctx_set_current(g_cuda_ctx);
        if (r != 0) { SetCudaError("cuCtxSetCurrent", r); DisableCuda(); return 1; }

        // The caller's stream must belong to our (primary) context, or the
        // driver may dereference foreign state. PyTorch always uses the
        // primary context, so a mismatch means an unexpected host setup —
        // disable cleanly instead of gambling on it.
        if (p->stream && g_cu_stream_get_ctx) {
            g_cuda_av_stage = CUDA_AV_STREAM_CTX_CHECK;
            CUcontext sctx = nullptr;
            if (g_cu_stream_get_ctx(p->stream, &sctx) == 0 && sctx != g_cuda_ctx) {
                SetError("caller CUDA stream belongs to a different context than the bridge primary context");
                DisableCuda();
                return 1;
            }
        }

        // --- Input: torch RGB(f32) -> g_inbuf(RGBA16F linear), on the caller's
        // stream. The mapped pointer is persistent (external memory), so there
        // is no per-frame map/unmap; the handoff below is a CPU-side stream sync.
        g_cuda_av_stage = CUDA_AV_INPUT_KERNEL;
        if (!LaunchConversion(g_fn_rgb_to_rgba16f, reinterpret_cast<const void*>(p->src_dev), g_cu_inbuf_ptr, g_row_pitch, p->width, p->height, p->stream)) {
            DisableCuda(); return 1;
        }

        // --- CPU-side handoff (v0.5.7): the v0.5.6 GPU-side semaphore signal
        // crashed inside cuSignalExternalSemaphoresAsync on field GPUs, so the
        // stream is simply synchronized before the D3D12 queue runs. Tens of
        // microseconds against a ~100 ms inference. After this sync g_inbuf is
        // fully written and untouched by CUDA until the NEXT frame's input
        // kernel — queued on the same stream after this frame's async output
        // tail, so it cannot start before D3D12 has consumed the buffer below.
        g_cuda_av_stage = CUDA_AV_INPUT_SYNC;
        r = g_cu_stream_sync(p->stream);
        if (r != 0) {
            SetCudaError("cuStreamSynchronize(after input conversion)", r);
            DisableCuda(); return 1;
        }

        // --- v0.5.8: move the converted pixels into the frame texture with a
        // D3D12 copy. CopyTextureRegion translates to the driver's true
        // physical layout; the old raw kernel addressing of the placed texture
        // assumed physical pitch == copyable pitch, which field drivers violate
        // (per-row shear -> H/V moire stripes in every output). The barriers
        // end in exactly the states the CPU path leaves behind, so mixed
        // sessions stay consistent. All locals are POD — safe under /EHsc __try.
        g_cuda_av_stage = CUDA_AV_INPUT_COPY;
        {
            auto b1 = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g_cmd->ResourceBarrier(1, &b1);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_color.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = g_inbuf.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            src.PlacedFootprint.Footprint.Width = p->width; src.PlacedFootprint.Footprint.Height = p->height;
            src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
            g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            auto b2 = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_cmd->ResourceBarrier(1, &b2);
        }

        // --- NGX inference on the D3D12 queue (records into the same command
        // list, right after the input copy above).
        g_cuda_av_stage = CUDA_AV_EVALUATE;
        const int er_raw = SehEvaluate();
        if (er_raw == -1) {
            // Native access violation inside evaluate: nothing was submitted yet
            // (Close has not run), so there is no queue work to drain — the
            // partially recorded list is recycled below. The host treats a
            // native AV as fatal for this process (fail-fast latch).
            SetError("Native access violation inside DLSSNR evaluate");
            g_cmd_alloc->Reset();
            g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
            return 1;
        }
        if (static_cast<NGXResult>(er_raw) != NGX_SUCCESS) {
            SetError("DLSSNR EvaluateFeature failed: 0x%08X", static_cast<unsigned>(er_raw));
            // Nothing complete was submitted to the queue this frame (Close has
            // not run); recycle the partially recorded list. Do NOT disable CUDA
            // (this is an NGX issue; the CPU path would fail too).
            g_cmd_alloc->Reset();
            g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
            return 1;
        }

        // --- v0.5.8: mirror of the input side — pull the inference result out
        // of g_output into the linear staging buffer so the output kernel reads
        // unambiguous row-major memory (see CUDA_AV_INPUT_COPY).
        g_cuda_av_stage = CUDA_AV_OUTPUT_COPY;
        {
            auto b3 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g_cmd->ResourceBarrier(1, &b3);
            D3D12_TEXTURE_COPY_LOCATION rd{};
            rd.pResource = g_outbuf.Get(); rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rd.PlacedFootprint.Footprint.Width = p->width; rd.PlacedFootprint.Footprint.Height = p->height;
            rd.PlacedFootprint.Footprint.Depth = 1; rd.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
            D3D12_TEXTURE_COPY_LOCATION rs{};
            rs.pResource = g_output.Get(); rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g_cmd->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
            auto b4 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_cmd->ResourceBarrier(1, &b4);
        }

        g_cuda_av_stage = CUDA_AV_EXECUTE;
        if (!ExecuteAndWait()) return 1;
        // ExecuteAndWait's CPU fence wait covers the whole submission — input
        // copy + inference + output copy — so g_outbuf is fully written and no
        // GPU-side handoff back is needed. The next D3D12 write of g_outbuf
        // (next frame's output copy) is recorded only after this frame's
        // cuStreamSynchronize, which waits for the async output kernel above to
        // finish reading it — no cross-frame hazard on either buffer.

        g_cuda_av_stage = CUDA_AV_OUTPUT_KERNEL;
        if (!LaunchConversion(g_fn_rgba16f_to_rgb, g_cu_outbuf_ptr, reinterpret_cast<void*>(p->dst_dev), g_row_pitch, p->width, p->height, p->stream)) {
            DisableCuda(); return 1;
        }
        // From this point on an async tail (fence wait + kernel) may be pending on
        // this stream; remember it so a later ReleaseCudaExternal can wait for it.
        g_last_cuda_stream = p->stream;
        g_cuda_av_stage = CUDA_AV_NONE;
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // A raw access violation escaped the inner guards: it came from a driver
        // call this path makes directly (nvcuda.dll or D3D12). The stage names
        // the culprit for the field log. Since v0.9.17 the session is NOT rebuilt
        // in-process after such a crash (the rebuild has been observed to wedge):
        // the host fails the run with restart advice instead of falling back.
        g_native_av_pending = true;
        SetError("Native access violation in CUDA zero-copy path at stage '%s'; "
                 "the bridge session is corrupted — RESTART ComfyUI before the next run",
                 CudaAvStageName(g_cuda_av_stage));
        g_cmd_alloc->Reset();
        g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
        CudaAvCleanup(p->stream);
        DisableCuda();
        return 1;
    }
}

// Zero-copy path for tensors that already live on this GPU (PyTorch).
// src_dev/dst_dev are raw device pointers into contiguous float32 RGB arrays
// of width*height*3 elements each; cuda_stream is the caller's CUDA stream
// (0 = default). v0.5.8: the two linear staging buffers (not the frame
// textures) are imported once as CUDA external memory, so the conversion
// kernels use persistent mapped pointers — no per-frame map/unmap — and every
// texture<->buffer pixel move is a CopyTextureRegion, which makes the driver
// translate to the texture's true physical layout. Ordering between the
// caller's stream and our D3D12 queue is CPU-side since v0.5.7 (the v0.5.6
// GPU-side external-semaphore handoff crashed inside
// cuSignalExternalSemaphoresAsync on field GPUs):
//
//   input kernel -> cuStreamSynchronize (CPU blocks ~tens of us)
//   -> record [inbuf->g_color copy] + eval + [g_output->outbuf copy];
//      ExecuteAndWait (CPU blocks for the inference; its fence wait proves
//      g_outbuf is fully written)
//   -> output kernel (left queued on the caller's stream, async tail)
//
// so subsequent PyTorch work on that stream is ordered after the output
// kernel with no extra round trip. On any non-native CUDA failure the interop
// path is disabled for the rest of the session and 0 is returned — the host
// retries the frame through dlss5nr_process (CPU staging). NGX-level failures
// are NOT disabling: they would fail on the CPU path too, so the error is
// surfaced. The whole body runs inside SehCudaFrame (see above): a native
// access violation is contained and reported as a clean stage-tagged error,
// but it is fatal for this process — the session cannot be rebuilt in-process
// reliably, so the host fails the run with restart advice.
__declspec(dllexport) int __cdecl dlss5nr_process_cuda(
    uint64_t src_dev, uint64_t dst_dev, int width, int height,
    uint64_t cuda_stream, int style, int preset,
    float intensity, float tone, float structure, float skin,
    int automask, int reset, char* err, int err_cap) {

    std::lock_guard<std::mutex> guard(g_mutex);
    g_last_error.clear();
    if (!g_initialized) { SetError("DLSS5 NR bridge is not initialized"); CopyError(err, err_cap); return 0; }
    if (!src_dev || !dst_dev || width <= 0 || height <= 0) { SetError("Invalid image pointer/dimensions"); CopyError(err, err_cap); return 0; }
    if (width > 16384 || height > 16384) { SetError("Image dimensions are unreasonably large"); CopyError(err, err_cap); return 0; }
    if (g_native_av_pending) {
        // v0.9.17: same fail-fast as the CPU path — a caught access violation
        // leaves the session corrupted and the in-process rebuild has been
        // observed to wedge, so the run stops with restart advice instead of
        // retrying on a poisoned session.
        SetError("session corrupted by a previous native crash — RESTART ComfyUI before the next run");
        CopyError(err, err_cap);
        return 0;
    }
    if (!DeviceHealthy()) {
        // Device removed without a caught access violation: rebuild from the
        // stored init parameters (mirrors the CPU path). Optical flow never
        // runs on this path, so no latch needed.
        if (!ReinitUnlocked()) { CopyError(err, err_cap); return 0; }
    }
    if (!g_cuda_available) {
        SetError("CUDA interop unavailable: %s", g_cuda_note.empty() ? std::string("unknown reason").c_str() : g_cuda_note.c_str());
        CopyError(err, err_cap); return 0;
    }

    // The zero-copy path never carries motion vectors; temporal mode forces
    // the CPU staging path in the host, so EnsureFeature's use_motion_vectors
    // stays false by construction (inside the trampoline).
    CudaFrameParams p{};
    p.src_dev = src_dev;
    p.dst_dev = dst_dev;
    p.width = width;
    p.height = height;
    p.stream = reinterpret_cast<CUstream>(cuda_stream);
    p.style = style;
    p.preset = preset;
    p.intensity = intensity;
    p.tone = tone;
    p.structure = structure;
    p.skin = skin;
    p.automask = automask;
    p.reset = reset;

    if (SehCudaFrame(&p) == 0) {
        // Deliberately NOT synchronizing: the output kernel stays queued on
        // the caller's stream, and every later PyTorch op on that stream is
        // ordered after it. The CPU returns while the GPU finishes. A clean
        // frame also proves a just-rebuilt session is healthy — clear the
        // crash latch.
        g_native_av_pending = false;
        CopyError(err, err_cap);
        return 1;
    }
    CopyError(err, err_cap);
    return 0;
}

__declspec(dllexport) void __cdecl dlss5nr_shutdown() {
    std::lock_guard<std::mutex> guard(g_mutex);
    ShutdownUnlocked();
}

}
