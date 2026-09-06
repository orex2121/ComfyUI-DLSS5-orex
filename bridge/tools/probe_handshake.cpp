// probe_handshake.cpp — v0.9.16 diagnostic for the frame-0 access violation in
// dlss5nr_process_cuda (first-ever field run of the CUDA zero-copy path).
//
// The v0.9.15 probe validated CreateSharedHandle(fence/heap) +
// cuImportExternalSemaphore only. This one replays the FULL handshake the
// bridge performs on frame 0, step by step, each step wrapped in its own SEH
// guard so a driver access violation prints the culprit step instead of taking
// the process down:
//
//   1. D3D12 device + queue + fence(SHARED) + CreateSharedHandle(GENERIC_ALL)
//   2. placed R16G16B16A16_FLOAT texture heaps — byte-for-byte the bridge's
//      AllocateFrameResources layout (GetCopyableFootprints, same heap-size
//      formula, D3D12_HEAP_FLAG_SHARED, CreatePlacedResource at fp.Offset)
//   3. cuInit + CUDA device name match + primary context retain/set-current
//   4. cuImportExternalMemory(color heap) + cuExternalMemoryGetMappedBuffer
//   5. cuImportExternalMemory(output heap) + GetMappedBuffer
//   6. cuModuleLoadData(real bridge PTX) + cuModuleGetFunction x2
//   7. cuStreamCreate(NON_BLOCKING, like torch) + cuMemAlloc + HtoD pattern
//   8. launch dlss5nr_rgb_to_rgba16f: CUDA buffer -> imported heap pointer
//   9. cuImportExternalSemaphore(fence NT handle)
//  10. cuSignalExternalSemaphoresAsync(v=1) on the stream, D3D12 queue->Wait(1),
//      empty command list submit, Signal fence v=2, CPU event wait
//  11. cuWaitExternalSemaphoresAsync(v=2) on the stream
//  12. launch dlss5nr_rgba16f_to_rgb: imported heap pointer -> CUDA buffer
//  13. cuStreamSynchronize + DtoH + value check (f16 round-trip tolerance)
//
// VRAM footprint is tiny by default (128x128 textures, ~1 MB total); pass a
// second pair of args to test at real size, e.g. "probe_handshake.exe 768 1344"
// (~40 MB transient). No kernels run on the default path beyond two trivial
// conversion launches — safe while the card is busy with other work.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <stdio.h>
#include <string.h>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include "../cuda_kernels_ptx.h"

using Microsoft::WRL::ComPtr;

// --- cuda.h subset (layout verified against CUDA 12.8 headers) --------------
typedef int CUresult;
typedef void* CUcontext;
typedef void* CUmodule;
typedef void* CUfunction;
typedef void* CUstream;
typedef unsigned long long CUdeviceptr;
typedef void* CUexternalMemory;   // opaque driver handle
typedef void* CUexternalSemaphore;

enum {
    PROBE_EXT_MEM_TYPE_D3D12_HEAP = 4,  // CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP
    PROBE_EXT_MEM_DEDICATED = 0x1,      // CU_EXT_MEM_FLAG_DEDICATED
    PROBE_SEM_TYPE_D3D12_FENCE = 4,     // CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE
};

struct ProbeMemDesc {
    int type;
    union {
        int fd;
        struct { void* handle; const void* name; } win32;
        const void* nvSciSyncObj;
    } handle;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
};

// cuda.h CUDA_EXTERNAL_MEMORY_BUFFER_DESC: flags MUST be zero — the driver
// reads it, so the field must exist (a truncated struct makes it read stack
// garbage and fail with CUDA_ERROR_INVALID_VALUE).
struct ProbeBufferDesc {
    unsigned long long offset;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
};

struct ProbeSemDesc {
    int type;
    union {
        int fd;
        struct { void* handle; const void* name; } win32;
        const void* nvSciSyncObj;
    } handle;
    unsigned int flags;
    unsigned int reserved[16];
};

// cuda.h CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS / _WAIT_PARAMS: there is NO
// paramsType field - the anonymous params struct starts at offset 0, so
// fence.value lives at offset 0. (Signal and wait differ only inside
// keyedMutex/reserved; for a D3D12_FENCE semaphore only fence.value + the
// zeroed flags matter, and both layouts agree on those offsets.)
struct ProbeFenceParams {
    struct {
        struct { unsigned long long value; } fence;
        union { void* fence; unsigned long long reserved; } nvSciSync;
        struct { unsigned long long key; } keyedMutex;
        unsigned int reserved[12];
    } params;
    unsigned int flags;
    unsigned int reserved[16];
};

typedef CUresult (WINAPI *PfnCuInit)(unsigned);
typedef CUresult (WINAPI *PfnCuDeviceGetCount)(int*);
typedef CUresult (WINAPI *PfnCuDeviceGetName)(char*, int, unsigned);
typedef CUresult (WINAPI *PfnCuPrimaryCtxRetain)(void**, unsigned);
typedef CUresult (WINAPI *PfnCuPrimaryCtxRelease)(unsigned);
typedef CUresult (WINAPI *PfnCuCtxSetCurrent)(void*);
typedef CUresult (WINAPI *PfnCuImportMem)(CUexternalMemory*, const ProbeMemDesc*);
typedef CUresult (WINAPI *PfnCuGetMappedBuffer)(void**, CUexternalMemory, const ProbeBufferDesc*);
typedef CUresult (WINAPI *PfnCuDestroyMem)(CUexternalMemory);
typedef CUresult (WINAPI *PfnCuModuleLoadData)(CUmodule*, const void*);
typedef CUresult (WINAPI *PfnCuModuleGetFunction)(CUfunction*, CUmodule, const char*);
typedef CUresult (WINAPI *PfnCuStreamCreate)(CUstream*, unsigned);
typedef CUresult (WINAPI *PfnCuStreamSync)(CUstream);
typedef CUresult (WINAPI *PfnCuMemAlloc)(CUdeviceptr*, size_t);
typedef CUresult (WINAPI *PfnCuMemFree)(CUdeviceptr);
typedef CUresult (WINAPI *PfnCuMemcpyHtoD)(CUdeviceptr, const void*, size_t);
typedef CUresult (WINAPI *PfnCuMemcpyDtoH)(void*, CUdeviceptr, size_t);
typedef CUresult (WINAPI *PfnCuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                             unsigned, unsigned, unsigned, unsigned,
                                             CUstream, void**, void**);
typedef CUresult (WINAPI *PfnCuImportSem)(CUexternalSemaphore*, const ProbeSemDesc*);
typedef CUresult (WINAPI *PfnCuDestroySem)(CUexternalSemaphore);
typedef CUresult (WINAPI *PfnCuSignalSem)(const CUexternalSemaphore*, const ProbeFenceParams*, unsigned, CUstream);
typedef CUresult (WINAPI *PfnCuWaitSem)(const CUexternalSemaphore*, const ProbeFenceParams*, unsigned, CUstream);

// --- globals (file scope so the SEH-guarded steps keep POD-only locals) ------
static ComPtr<ID3D12Device> g_device;
static ComPtr<ID3D12CommandQueue> g_queue;
static ComPtr<ID3D12CommandAllocator> g_cmd_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_cmd;
static ComPtr<ID3D12Fence> g_fence;
static HANDLE g_fence_nt = INVALID_HANDLE_VALUE;
static unsigned long long g_fence_value = 0;

static UINT g_w = 128, g_h = 128;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp{};
static UINT64 g_row_pitch = 0, g_required = 0, g_heap_bytes = 0;
static ComPtr<ID3D12Heap> g_color_heap, g_output_heap;
static ComPtr<ID3D12Resource> g_color_tex, g_output_tex;

static HMODULE g_cuda = nullptr;
static PfnCuInit pInit;
static PfnCuDeviceGetCount pCount;
static PfnCuDeviceGetName pName;
static PfnCuPrimaryCtxRetain pRetain;
static PfnCuPrimaryCtxRelease pRelease;
static PfnCuCtxSetCurrent pSetCur;
static PfnCuImportMem pImportMem;
static PfnCuGetMappedBuffer pGetMapped;
static PfnCuDestroyMem pDestroyMem;
static PfnCuModuleLoadData pModLoad;
static PfnCuModuleGetFunction pModFn;
static PfnCuStreamCreate pStreamCreate;
static PfnCuStreamSync pStreamSync;
static PfnCuMemAlloc pMemAlloc;
static PfnCuMemFree pMemFree;
static PfnCuMemcpyHtoD pHtoD;
static PfnCuMemcpyDtoH pDtoH;
static PfnCuLaunchKernel pLaunch;
static PfnCuImportSem pImportSem;
static PfnCuDestroySem pDestroySem;
static PfnCuSignalSem pSignalSem;
static PfnCuWaitSem pWaitSem;

static char g_adapter_name[256] = {};

static CUcontext g_ctx = nullptr;
static unsigned g_dev_index = 0xFFFFFFFFu;
static CUexternalMemory g_color_mem = nullptr, g_output_mem = nullptr;
static void* g_color_ptr = nullptr;
static void* g_output_ptr = nullptr;
static CUmodule g_module = nullptr;
static CUfunction g_fn_in = nullptr, g_fn_out = nullptr;
static CUstream g_stream = nullptr;
static CUexternalSemaphore g_sem = nullptr;
static CUdeviceptr g_src_dev = 0, g_dst_dev = 0;
static float* g_host = nullptr;

// --- SEH guard ----------------------------------------------------------------
// One shared wrapper: each step is a plain function (POD-only locals), the
// wrapper catches any native access violation and reports which step faulted.
typedef int (*StepFn)();
static const char* g_step_name = "";
static int g_av_step_id = -1;

static int RunGuarded(int id, const char* name, StepFn fn) {
    g_step_name = name;
    printf("STEP %2d %-46s ... ", id, name);
    fflush(stdout);
    __try {
        int r = fn();
        printf("%s\n", r ? "OK" : "FAIL");
        return r;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // No GetExceptionInformation() here: under /EHsc it is the
        // _exception_info intrinsic, which MSVC rejects in a handler body
        // (C2707). The step identity is the diagnostic payload; the faulting
        // address (read of 0x8) is already known from the field log.
        printf("ACCESS VIOLATION\n");
        g_av_step_id = id;
        return 0;
    }
}

static void CuFail(const char* what, CUresult r) {
    printf("    %s failed: 0x%08X\n", what, (unsigned)r);
}

// Same name matching the bridge uses to pair a D3D12 adapter with a CUDA device:
// exact or case-insensitive substring either way.
static int NamesMatch(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) return 0;
    size_t la = strlen(a), lb = strlen(b);
    if (la < lb) { const char* t = a; a = b; b = t; size_t tt = la; la = lb; lb = tt; }
    for (size_t i = 0; i + lb <= la; ++i) {
        int eq = 1;
        for (size_t j = 0; j < lb; ++j)
            if (tolower((unsigned char)a[i + j]) != tolower((unsigned char)b[j])) { eq = 0; break; }
        if (eq) return 1;
    }
    return 0;
}

// --- steps ---------------------------------------------------------------------

static int StepFence() {
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_fence)))) return 0;
    HRESULT hr = g_device->CreateSharedHandle(g_fence.Get(), nullptr, GENERIC_ALL, nullptr, &g_fence_nt);
    if (FAILED(hr) || !g_fence_nt) { printf("    CreateSharedHandle(fence) FAIL 0x%08X\n", (unsigned)hr); return 0; }
    return 1;
}

static int StepHeaps() {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g_w; d.Height = g_h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    UINT nr_rows = 0;
    UINT64 row_pitch = 0, required = 0;
    g_device->GetCopyableFootprints(&d, 0, 1, 0, &g_fp, &nr_rows, &row_pitch, &required);

    // The bridge's exact heap-size formula (dlss5nr_bridge.cpp AllocateFrameResources).
    D3D12_RESOURCE_ALLOCATION_INFO alloc = g_device->GetResourceAllocationInfo(1, 0, &d);
    const UINT h_placement = ((g_h + 127) / 128) * 128;
    const UINT64 placement_bytes = row_pitch * static_cast<UINT64>(h_placement);
    UINT64 heap_bytes = required;
    const UINT64 data_end = g_fp.Offset + row_pitch * static_cast<UINT64>(g_h);
    if (data_end > heap_bytes) heap_bytes = data_end;
    if (placement_bytes > heap_bytes) heap_bytes = placement_bytes;
    UINT64 alloc_size = alloc.SizeInBytes;
    if (alloc.Alignment > 1) alloc_size = (alloc_size + alloc.Alignment - 1) & ~(alloc.Alignment - 1);
    if (alloc_size > heap_bytes) heap_bytes = alloc_size;
    heap_bytes = (heap_bytes + 0xFFFFull) & ~0xFFFFull;

    ComPtr<ID3D12Heap> heaps[2];
    ComPtr<ID3D12Resource> texs[2];
    const D3D12_RESOURCE_STATES states[2] = {
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,  // color
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS            // output
    };
    for (int t = 0; t < 2 && heap_bytes > 0; ++t) {
        for (int attempt = 0;; ++attempt) {
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes = heap_bytes;
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Flags = D3D12_HEAP_FLAG_SHARED;
            HRESULT hr = g_device->CreateHeap(&hd, IID_PPV_ARGS(&heaps[t]));
            if (SUCCEEDED(hr)) {
                hr = g_device->CreatePlacedResource(heaps[t].Get(), g_fp.Offset, &d, states[t],
                                                    nullptr, IID_PPV_ARGS(&texs[t]));
            }
            if (SUCCEEDED(hr)) break;
            heaps[t].Reset();
            texs[t].Reset();
            if (attempt >= 31) { printf("    CreatePlacedResource FAIL 0x%08X\n", (unsigned)hr); heap_bytes = 0; break; }
            heap_bytes += 0x10000ull;
        }
    }
    if (!heaps[0] || !texs[0] || !heaps[1] || !texs[1]) return 0;
    g_color_heap = heaps[0]; g_color_tex = texs[0];
    g_output_heap = heaps[1]; g_output_tex = texs[1];
    g_row_pitch = static_cast<UINT64>(g_fp.Footprint.RowPitch);
    g_required = required;
    g_heap_bytes = heap_bytes;
    printf("    %ux%u pitch=%llu offset=%llu copyable=%llu alloc=%llu/%llu heap=%llu KB\n",
           g_w, g_h, (unsigned long long)g_row_pitch, (unsigned long long)g_fp.Offset,
           (unsigned long long)required, (unsigned long long)alloc.SizeInBytes,
           (unsigned long long)alloc.Alignment, (unsigned long long)(heap_bytes / 1024));
    return 1;
}

static int StepCudaInit() {
    g_cuda = LoadLibraryW(L"nvcuda.dll");
    if (!g_cuda) { printf("    nvcuda.dll not found\n"); return 0; }
    pInit = (PfnCuInit)GetProcAddress(g_cuda, "cuInit");
    pCount = (PfnCuDeviceGetCount)GetProcAddress(g_cuda, "cuDeviceGetCount");
    pName = (PfnCuDeviceGetName)GetProcAddress(g_cuda, "cuDeviceGetName");
    pRetain = (PfnCuPrimaryCtxRetain)GetProcAddress(g_cuda, "cuDevicePrimaryCtxRetain");
    pRelease = (PfnCuPrimaryCtxRelease)GetProcAddress(g_cuda, "cuDevicePrimaryCtxRelease");
    pSetCur = (PfnCuCtxSetCurrent)GetProcAddress(g_cuda, "cuCtxSetCurrent");
    pImportMem = (PfnCuImportMem)GetProcAddress(g_cuda, "cuImportExternalMemory");
    pGetMapped = (PfnCuGetMappedBuffer)GetProcAddress(g_cuda, "cuExternalMemoryGetMappedBuffer");
    pDestroyMem = (PfnCuDestroyMem)GetProcAddress(g_cuda, "cuDestroyExternalMemory");
    pModLoad = (PfnCuModuleLoadData)GetProcAddress(g_cuda, "cuModuleLoadData");
    pModFn = (PfnCuModuleGetFunction)GetProcAddress(g_cuda, "cuModuleGetFunction");
    pStreamCreate = (PfnCuStreamCreate)GetProcAddress(g_cuda, "cuStreamCreate");
    pStreamSync = (PfnCuStreamSync)GetProcAddress(g_cuda, "cuStreamSynchronize");
    // The unversioned exports are legacy v1 stubs (different functions in
    // nvcuda.dll; cuda.h #defines cuMemAlloc -> cuMemAlloc_v2). Must use _v2.
    pMemAlloc = (PfnCuMemAlloc)GetProcAddress(g_cuda, "cuMemAlloc_v2");
    pMemFree = (PfnCuMemFree)GetProcAddress(g_cuda, "cuMemFree_v2");
    pHtoD = (PfnCuMemcpyHtoD)GetProcAddress(g_cuda, "cuMemcpyHtoD_v2");
    pDtoH = (PfnCuMemcpyDtoH)GetProcAddress(g_cuda, "cuMemcpyDtoH_v2");
    pLaunch = (PfnCuLaunchKernel)GetProcAddress(g_cuda, "cuLaunchKernel");
    pImportSem = (PfnCuImportSem)GetProcAddress(g_cuda, "cuImportExternalSemaphore");
    pDestroySem = (PfnCuDestroySem)GetProcAddress(g_cuda, "cuDestroyExternalSemaphore");
    pSignalSem = (PfnCuSignalSem)GetProcAddress(g_cuda, "cuSignalExternalSemaphoresAsync");
    pWaitSem = (PfnCuWaitSem)GetProcAddress(g_cuda, "cuWaitExternalSemaphoresAsync");
    if (!pInit || !pCount || !pName || !pRetain || !pRelease || !pSetCur || !pImportMem ||
        !pGetMapped || !pDestroyMem || !pModLoad || !pModFn || !pStreamCreate || !pStreamSync ||
        !pMemAlloc || !pMemFree || !pHtoD || !pDtoH || !pLaunch || !pImportSem || !pDestroySem ||
        !pSignalSem || !pWaitSem) { printf("    nvcuda.dll missing exports\n"); return 0; }

    CUresult r = pInit(0);
    if (r != 0) { CuFail("cuInit", r); return 0; }
    int n = 0;
    pCount(&n);
    for (int i = 0; i < n && g_dev_index == 0xFFFFFFFFu; ++i) {
        char name[256] = {};
        pName(name, sizeof(name), (unsigned)i);
        printf("    cuda device %d: %s%s\n", i, name,
               NamesMatch(g_adapter_name, name) ? "   <-- matches D3D12 adapter" : "");
        if (NamesMatch(g_adapter_name, name)) g_dev_index = (unsigned)i;
    }
    if (g_dev_index == 0xFFFFFFFFu) { printf("    no CUDA device matching the D3D12 adapter\n"); return 0; }
    r = pRetain(&g_ctx, g_dev_index);
    if (r != 0 || !g_ctx) { CuFail("cuDevicePrimaryCtxRetain", r); return 0; }
    r = pSetCur(g_ctx);
    if (r != 0) { CuFail("cuCtxSetCurrent", r); return 0; }
    return 1;
}

// Import one placed-texture heap exactly like the bridge's ImportTextureHeap.
static int ImportOneHeap(int which /*0 color, 1 output*/, CUexternalMemory* memOut, void** ptrOut) {
    ID3D12Heap* heap = which == 0 ? g_color_heap.Get() : g_output_heap.Get();
    HANDLE h = nullptr;
    HRESULT shr = g_device->CreateSharedHandle(heap, nullptr, GENERIC_ALL, nullptr, &h);
    if (FAILED(shr) || !h) { printf("    CreateSharedHandle(%s heap) FAIL 0x%08X\n", which ? "output" : "color", (unsigned)shr); return 0; }

    ProbeMemDesc md{};
    md.type = PROBE_EXT_MEM_TYPE_D3D12_HEAP;
    md.handle.win32.handle = h;
    md.size = g_heap_bytes;
    md.flags = PROBE_EXT_MEM_DEDICATED;

    CUresult r = pImportMem(memOut, &md);
    CloseHandle(h);
    if (r != 0 || !*memOut) { CuFail(which ? "cuImportExternalMemory(output)" : "cuImportExternalMemory(color)", r); return 0; }

    ProbeBufferDesc bd{};
    bd.offset = 0;
    bd.size = g_heap_bytes;
    unsigned long long dev_ptr = 0;
    r = pGetMapped(reinterpret_cast<void**>(&dev_ptr), *memOut, &bd);  // cuda.h: void** out-param
    if (r != 0 || !dev_ptr) { CuFail(which ? "cuExternalMemoryGetMappedBuffer(output)" : "cuExternalMemoryGetMappedBuffer(color)", r); return 0; }
    *ptrOut = (void*)(dev_ptr + g_fp.Offset);
    printf("    %s: mapped base=0x%llX ptr(+offset)=0x%llX\n", which ? "output" : "color",
           (unsigned long long)dev_ptr, (unsigned long long)(uintptr_t)*ptrOut);
    return 1;
}

static int StepImportColor() { return ImportOneHeap(0, &g_color_mem, &g_color_ptr); }
static int StepImportOutput() { return ImportOneHeap(1, &g_output_mem, &g_output_ptr); }

static int StepModule() {
    CUresult r = pModLoad(&g_module, kDlss5NrKernelPtx);
    if (r != 0) { CuFail("cuModuleLoadData(real bridge PTX)", r); return 0; }
    r = pModFn(&g_fn_in, g_module, "dlss5nr_rgb_to_rgba16f");
    if (r != 0) { CuFail("cuModuleGetFunction(rgb_to_rgba16f)", r); return 0; }
    r = pModFn(&g_fn_out, g_module, "dlss5nr_rgba16f_to_rgb");
    if (r != 0) { CuFail("cuModuleGetFunction(rgba16f_to_rgb)", r); return 0; }
    return 1;
}

static int StepStreamBuffers() {
    CUresult r = pStreamCreate(&g_stream, 1 /*CU_STREAM_NON_BLOCKING, like torch*/);
    if (r != 0) { CuFail("cuStreamCreate", r); return 0; }
    size_t bytes = (size_t)g_w * g_h * 3 * sizeof(float);
    r = pMemAlloc(&g_src_dev, bytes);
    if (r != 0) { CuFail("cuMemAlloc(src)", r); return 0; }
    r = pMemAlloc(&g_dst_dev, bytes);
    if (r != 0) { CuFail("cuMemAlloc(dst)", r); return 0; }
    size_t n = (size_t)g_w * g_h * 3;
    g_host = (float*)malloc(bytes);
    if (!g_host) { printf("    malloc failed\n"); return 0; }
    for (size_t i = 0; i < n; ++i) g_host[i] = (float)(((i * 7919u) % 1000u)) / 1000.0f;
    r = pHtoD(g_src_dev, g_host, bytes);
    if (r != 0) { CuFail("cuMemcpyHtoD", r); return 0; }
    printf("    stream=0x%llX buffers=%llu KB each\n", (unsigned long long)(uintptr_t)g_stream,
           (unsigned long long)(bytes / 1024));
    return 1;
}

static int LaunchOne(CUfunction fn, const void* src, void* dst) {
    uint64_t s = (uint64_t)(uintptr_t)src;
    uint64_t d = (uint64_t)(uintptr_t)dst;
    uint64_t p = (uint64_t)g_row_pitch;
    int wi = g_w, hi = g_h;
    void* args[] = { &s, &d, &p, &wi, &hi };
    const unsigned gx = (g_w + 31u) / 32u;
    const unsigned gy = (g_h + 7u) / 8u;
    CUresult r = pLaunch(fn, gx, gy, 1, 32, 8, 1, 0, g_stream, args, nullptr);
    if (r != 0) CuFail("cuLaunchKernel", r);
    return r == 0 ? 1 : 0;
}

static int StepLaunchIn() { return LaunchOne(g_fn_in, (const void*)g_src_dev, g_color_ptr); }
static int StepLaunchOut() { return LaunchOne(g_fn_out, g_color_ptr, (void*)g_dst_dev); }

static int StepImportSem() {
    ProbeSemDesc sd{};
    sd.type = PROBE_SEM_TYPE_D3D12_FENCE;
    sd.handle.win32.handle = g_fence_nt;
    CUresult r = pImportSem(&g_sem, &sd);
    if (r != 0 || !g_sem) { CuFail("cuImportExternalSemaphore", r); return 0; }
    return 1;
}

static int StepHandshake() {
    // CUDA signals fence value 1 on the stream (after the input kernel).
    ProbeFenceParams sp{};
    sp.params.fence.value = 1;
    CUresult r = pSignalSem(&g_sem, &sp, 1, g_stream);
    if (r != 0) { CuFail("cuSignalExternalSemaphoresAsync(v=1)", r); return 0; }

    // D3D12 queue blocks until that signal, submits an empty list, signals v=2.
    HRESULT hr = g_queue->Wait(g_fence.Get(), 1);
    if (FAILED(hr)) { printf("    Queue::Wait(1) FAIL 0x%08X\n", (unsigned)hr); return 0; }
    hr = g_cmd->Close();
    if (FAILED(hr)) { printf("    CommandList::Close FAIL 0x%08X\n", (unsigned)hr); return 0; }
    ID3D12CommandList* lists[] = { g_cmd.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    ++g_fence_value;  // -> 2
    hr = g_queue->Signal(g_fence.Get(), g_fence_value);
    if (FAILED(hr)) { printf("    Queue::Signal(2) FAIL 0x%08X\n", (unsigned)hr); return 0; }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) { printf("    CreateEvent failed\n"); return 0; }
    g_fence->SetEventOnCompletion(g_fence_value, ev);
    DWORD w = WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
    if (w != WAIT_OBJECT_0) { printf("    CPU wait for fence v=2 timed out (%lu)\n", w); return 0; }
    g_cmd_alloc->Reset();
    g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
    return 1;
}

static int StepWaitSem() {
    ProbeFenceParams wp{};
    wp.params.fence.value = g_fence_value;  // 2
    CUresult r = pWaitSem(&g_sem, &wp, 1, g_stream);
    if (r != 0) { CuFail("cuWaitExternalSemaphoresAsync(v=2)", r); return 0; }
    return 1;
}

static int StepCheck() {
    CUresult r = pStreamSync(g_stream);
    if (r != 0) { CuFail("cuStreamSynchronize", r); return 0; }
    size_t n = (size_t)g_w * g_h * 3;
    r = pDtoH(g_host, g_dst_dev, n * sizeof(float));
    if (r != 0) { CuFail("cuMemcpyDtoH", r); return 0; }
    float maxdiff = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float d = g_host[i] - ((float)(((i * 7919u) % 1000u)) / 1000.0f);
        if (d < 0) d = -d;
        if (d > maxdiff) maxdiff = d;
    }
    printf("    round-trip max|diff| = %.6f (f16 tolerance ~5e-4)\n", maxdiff);
    if (maxdiff > 5e-3f) { printf("    VALUE CHECK FAILED\n"); return 0; }
    return 1;
}

static void Cleanup() {
    if (g_stream) pStreamSync(g_stream);
    if (g_sem) pDestroySem(g_sem);
    if (g_src_dev) pMemFree(g_src_dev);
    if (g_dst_dev) pMemFree(g_dst_dev);
    free(g_host);
    if (g_module) { typedef CUresult (WINAPI *PfnModUnl)(CUmodule); PfnModUnl u = (PfnModUnl)GetProcAddress(g_cuda, "cuModuleUnload"); if (u) u(g_module); }
    if (g_stream) { typedef CUresult (WINAPI *PfnStrmDst)(CUstream); PfnStrmDst s = (PfnStrmDst)GetProcAddress(g_cuda, "cuStreamDestroy"); if (s) s(g_stream); }
    if (g_color_mem) pDestroyMem(g_color_mem);
    if (g_output_mem) pDestroyMem(g_output_mem);
    if (g_ctx) pRelease(g_dev_index);
    if (g_cuda) FreeLibrary(g_cuda);
    if (g_fence_nt != INVALID_HANDLE_VALUE) CloseHandle(g_fence_nt);

    g_cmd.Reset(); g_cmd_alloc.Reset(); g_fence.Reset();
    g_color_heap.Reset(); g_output_heap.Reset();
    g_color_tex.Reset(); g_output_tex.Reset();
    g_queue.Reset(); g_device.Reset();
}

// After a driver AV the context may be corrupted; cleanup calls can fault too.
static void CleanupGuarded() {
    __try { Cleanup(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("note: cleanup faulted, ignoring\n"); }
}

// --- main -----------------------------------------------------------------------

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 3) { g_w = atoi(argv[1]); g_h = atoi(argv[2]); }
    if (g_w < 8 || g_h < 8 || g_w > 4096 || g_h > 4096) { printf("bad size\n"); return 1; }

    printf("probe_handshake: full CUDA/D3D12 zero-copy handshake at %ux%u\n", g_w, g_h);

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { printf("CreateDXGIFactory1 failed\n"); return 1; }
    int ok = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, 255, NULL, NULL);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE) { adapter->Release(); continue; }

        HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device));
        if (FAILED(hr)) { printf("D3D12CreateDevice FAIL 0x%08X\n", (unsigned)hr); adapter->Release(); continue; }
        strncpy(g_adapter_name, name, sizeof(g_adapter_name) - 1);
        g_adapter_name[sizeof(g_adapter_name) - 1] = '\0';
        printf("adapter: %s\n", name);

        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&g_queue)))) { adapter->Release(); continue; }
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_alloc)))) { adapter->Release(); continue; }
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&g_cmd)))) { adapter->Release(); continue; }

        ok = 1;
        ok &= RunGuarded(1, "fence(SHARED) + CreateSharedHandle(GENERIC_ALL)", StepFence);
        if (ok) ok &= RunGuarded(2, "placed texture heaps (bridge layout)", StepHeaps);
        if (ok) ok &= RunGuarded(3, "cuInit + primary ctx", StepCudaInit);
        if (ok) ok &= RunGuarded(4, "import color heap -> mapped buffer", StepImportColor);
        if (ok) ok &= RunGuarded(5, "import output heap -> mapped buffer", StepImportOutput);
        if (ok) ok &= RunGuarded(6, "load bridge PTX module + functions", StepModule);
        if (ok) ok &= RunGuarded(7, "stream + buffers + HtoD pattern", StepStreamBuffers);
        if (ok) ok &= RunGuarded(8, "launch rgb->rgba16f into imported heap", StepLaunchIn);
        if (ok) ok &= RunGuarded(9, "import fence semaphore", StepImportSem);
        if (ok) ok &= RunGuarded(10, "signal v1 -> D3D12 wait/submit/signal v2", StepHandshake);
        if (ok) ok &= RunGuarded(11, "cuda wait fence v2 on stream", StepWaitSem);
        if (ok) ok &= RunGuarded(12, "launch rgba16f->rgb from imported heap", StepLaunchOut);
        if (ok) ok &= RunGuarded(13, "sync + readback value check", StepCheck);

        printf("\nresult: %s%s\n", ok ? "ALL STEPS PASSED" : "FAILED at step ", ok ? "" : "");
        if (!ok && g_av_step_id > 0) printf("access violation in step %d\n", g_av_step_id);

        CleanupGuarded();
        adapter->Release();
        break;  // first NVIDIA adapter is enough
    }
    factory->Release();
    return ok ? 0 : 2;
}
