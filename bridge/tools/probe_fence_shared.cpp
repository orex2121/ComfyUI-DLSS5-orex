// probe_fence_shared.cpp — v0.9.15 diagnostic for the zero-copy interop failure.
//
// The bridge's v0.9.14 fence import failed inside ComfyUI with a FAILED HRESULT,
// but the error text printed GetLastError(), which is 0 for COM methods, so the
// real code was hidden. This probe reproduces the exact call sequence on the
// real GPU without allocating meaningful VRAM and without running any kernel,
// so it is safe while the card is busy:
//
//   1. ID3D12Device::CreateSharedHandle for fences (NONE / SHARED flag) and a
//      small heap, with GENERIC_ALL and GENERIC_READ|GENERIC_WRITE — prints the
//      actual HRESULT of every combination.
//   2. If a fence NT handle exists: cuImportExternalSemaphore with the same
//      hand-written ABI structs the bridge uses (validated against cuda.h
//      12.8/13.0), so a pass also validates the bridge's layout.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>
#include <cctype>
#include <string>

// --- cuda.h subset (layout verified against CUDA 12.8 and 13.0 headers) ----
typedef int CUresult;
typedef void* CUexternalSemaphore; // opaque driver handle
enum { PROBE_SEM_TYPE_D3D12_FENCE = 4 }; // CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE

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

typedef CUresult (WINAPI *PfnCuInit)(unsigned);
typedef CUresult (WINAPI *PfnCuDeviceGetCount)(int*);
typedef CUresult (WINAPI *PfnCuDeviceGetName)(char*, int, unsigned int);
typedef CUresult (WINAPI *PfnCuPrimaryCtxRetain)(void**, unsigned int);
typedef CUresult (WINAPI *PfnCuPrimaryCtxRelease)(unsigned int);
typedef CUresult (WINAPI *PfnCuCtxSetCurrent)(void*);
typedef CUresult (WINAPI *PfnCuImportSem)(CUexternalSemaphore*, const ProbeSemDesc*);
typedef CUresult (WINAPI *PfnCuDestroySem)(CUexternalSemaphore);

static void Hr(const char* what, HRESULT hr) {
    printf("  %-46s %s 0x%08X\n", what, SUCCEEDED(hr) ? "OK  " : "FAIL", (unsigned)hr);
}

static std::string Fold(const char* s) {
    std::string r(s ? s : "");
    for (char& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

static int NamesMatch(const char* a, const char* b) {
    std::string x = Fold(a), y = Fold(b);
    if (x.empty() || y.empty()) return 0;
    return x == y || x.find(y) != std::string::npos || y.find(x) != std::string::npos;
}

static void TryCudaImport(const char* adapter_name, HANDLE h) {
    HMODULE m = LoadLibraryW(L"nvcuda.dll");
    if (!m) { printf("  nvcuda.dll not found\n"); return; }
    PfnCuInit pInit = (PfnCuInit)GetProcAddress(m, "cuInit");
    PfnCuDeviceGetCount pCount = (PfnCuDeviceGetCount)GetProcAddress(m, "cuDeviceGetCount");
    PfnCuDeviceGetName pName = (PfnCuDeviceGetName)GetProcAddress(m, "cuDeviceGetName");
    PfnCuPrimaryCtxRetain pRetain = (PfnCuPrimaryCtxRetain)GetProcAddress(m, "cuDevicePrimaryCtxRetain");
    PfnCuPrimaryCtxRelease pRelease = (PfnCuPrimaryCtxRelease)GetProcAddress(m, "cuDevicePrimaryCtxRelease");
    PfnCuCtxSetCurrent pSetCur = (PfnCuCtxSetCurrent)GetProcAddress(m, "cuCtxSetCurrent");
    PfnCuImportSem pImport = (PfnCuImportSem)GetProcAddress(m, "cuImportExternalSemaphore");
    PfnCuDestroySem pDestroy = (PfnCuDestroySem)GetProcAddress(m, "cuDestroyExternalSemaphore");
    if (!pInit || !pCount || !pName || !pRetain || !pRelease || !pSetCur || !pImport || !pDestroy) {
        printf("  nvcuda.dll missing required exports\n");
        FreeLibrary(m);
        return;
    }
    CUresult r = pInit(0);
    if (r != 0) { printf("  cuInit failed: 0x%08X\n", (unsigned)r); FreeLibrary(m); return; }
    int n = 0;
    pCount(&n);
    unsigned match = 0xFFFFFFFFu;
    for (int i = 0; i < n; ++i) {
        char name[256] = {};
        pName(name, sizeof(name), (unsigned)i);
        printf("  cuda device %d: %s%s\n", i, name,
               NamesMatch(adapter_name, name) ? "   <-- matches D3D12 adapter" : "");
        if (match == 0xFFFFFFFFu && NamesMatch(adapter_name, name)) match = (unsigned)i;
    }
    if (match == 0xFFFFFFFFu) {
        printf("  no CUDA device matches this adapter; skipping import test\n");
        FreeLibrary(m);
        return;
    }
    void* ctx = nullptr;
    r = pRetain(&ctx, match);
    if (r != 0 || !ctx) {
        printf("  cuDevicePrimaryCtxRetain(%u) failed: 0x%08X\n", match, (unsigned)r);
        FreeLibrary(m);
        return;
    }
    pSetCur(ctx);
    ProbeSemDesc d{};
    d.type = PROBE_SEM_TYPE_D3D12_FENCE;
    d.handle.win32.handle = h;
    CUexternalSemaphore sem = nullptr;
    r = pImport(&sem, &d);
    printf("  cuImportExternalSemaphore(D3D12 fence) -> %s (0x%08X)\n",
           r == 0 ? "OK" : "FAIL", (unsigned)r);
    if (r == 0 && sem) pDestroy(sem);
    pRelease(match);
    FreeLibrary(m);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("probe_fence_shared: D3D12 CreateSharedHandle + CUDA fence import\n");

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        printf("CreateDXGIFactory1 failed\n");
        return 1;
    }

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, 255, NULL, NULL);
        printf("\nadapter %u: %s (vendor 0x%04X, VRAM %.0f MB, software=%d)\n", i, name,
               desc.VendorId, (double)desc.DedicatedVideoMemory / (1024.0 * 1024.0),
               (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE) {
            adapter->Release();
            continue;
        }

        ID3D12Device* device = nullptr;
        HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        if (FAILED(hr)) {
            printf("  D3D12CreateDevice FAIL 0x%08X\n", (unsigned)hr);
            adapter->Release();
            continue;
        }

        HANDLE h_fence_all = INVALID_HANDLE_VALUE;

        ID3D12Fence* fence = nullptr;
        if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            HRESULT h1 = device->CreateSharedHandle(fence, NULL, GENERIC_ALL, NULL, &h_fence_all);
            Hr("fence(NONE) + GENERIC_ALL", h1);
            if (FAILED(h1)) h_fence_all = INVALID_HANDLE_VALUE;
            HANDLE t2 = INVALID_HANDLE_VALUE;
            HRESULT h2 = device->CreateSharedHandle(fence, NULL, GENERIC_READ | GENERIC_WRITE, NULL, &t2);
            Hr("fence(NONE) + GENERIC_READ|GENERIC_WRITE", h2);
            if (SUCCEEDED(h2)) CloseHandle(t2);
        } else {
            printf("  CreateFence(NONE) failed\n");
        }

        ID3D12Fence* fence2 = nullptr;
        HANDLE h_fence_shared = INVALID_HANDLE_VALUE;
        if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence2)))) {
            HRESULT h3 = device->CreateSharedHandle(fence2, NULL, GENERIC_ALL, NULL, &h_fence_shared);
            Hr("fence(SHARED) + GENERIC_ALL", h3);
            if (FAILED(h3)) h_fence_shared = INVALID_HANDLE_VALUE;
        } else {
            printf("  CreateFence(SHARED) failed\n");
        }

        // The bridge creates its frame heaps with D3D12_HEAP_FLAG_SHARED; test
        // an unflagged heap as a control plus the shared-flag heap it uses.
        const struct { const char* label; D3D12_HEAP_FLAGS flags; } heap_cases[] = {
            {"heap NONE-flag", (D3D12_HEAP_FLAGS)0},
            {"heap SHARED-flag", D3D12_HEAP_FLAG_SHARED},
        };
        for (const auto& hc : heap_cases) {
            ID3D12Heap* h = nullptr;
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes = 65536;
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Flags = hc.flags;
            if (SUCCEEDED(device->CreateHeap(&hd, IID_PPV_ARGS(&h)))) {
                HANDLE t4 = INVALID_HANDLE_VALUE;
                HRESULT h4 = device->CreateSharedHandle(h, NULL, GENERIC_ALL, NULL, &t4);
                Hr((std::string(hc.label) + " + GENERIC_ALL").c_str(), h4);
                if (SUCCEEDED(h4)) CloseHandle(t4);
                HANDLE t5 = INVALID_HANDLE_VALUE;
                HRESULT h5 = device->CreateSharedHandle(h, NULL, GENERIC_READ | GENERIC_WRITE, NULL, &t5);
                Hr((std::string(hc.label) + " + GENERIC_READ|GENERIC_WRITE").c_str(), h5);
                if (SUCCEEDED(h5)) CloseHandle(t5);
                h->Release();
            } else {
                printf("  CreateHeap(%s) failed\n", hc.label);
            }
        }

        HANDLE cuda_test_handle = INVALID_HANDLE_VALUE;
        const char* cuda_test_src = "";
        if (h_fence_shared != INVALID_HANDLE_VALUE) { cuda_test_handle = h_fence_shared; cuda_test_src = "fence(SHARED) + GENERIC_ALL"; }
        else if (h_fence_all != INVALID_HANDLE_VALUE) { cuda_test_handle = h_fence_all; cuda_test_src = "fence(NONE) + GENERIC_ALL"; }
        if (cuda_test_handle != INVALID_HANDLE_VALUE) {
            printf("\n  CUDA import test (NT handle from %s):\n", cuda_test_src);
            TryCudaImport(name, cuda_test_handle);
        }
        if (h_fence_all != INVALID_HANDLE_VALUE) CloseHandle(h_fence_all);
        if (h_fence_shared != INVALID_HANDLE_VALUE) CloseHandle(h_fence_shared);

        if (fence2) fence2->Release();
        if (fence) fence->Release();
        device->Release();
        adapter->Release();
    }

    factory->Release();
    printf("\ndone.\n");
    return 0;
}
