// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ComfyUI-DLSS5-NR contributors
//
// Conversion kernels for the CUDA/D3D12 interop path of dlss5nr_bridge.dll.
// This file is NOT compiled into the DLL directly. It is the source of truth
// for bridge/cuda_kernels_ptx.h, which is generated with:
//
//   nvcc -arch=compute_75 -ptx cuda_interop.cu -o cuda_interop.ptx
//   (then wrap the .ptx text into a C++ raw string literal)
//
// The bridge loads the PTX at runtime through cuModuleLoadData and launches
// these kernels with the CUDA driver API, so the DLL has no build-time or
// link-time dependency on the CUDA toolkit. compute_75 (Turing) is the floor:
// DLSS requires Turing+ anyway, and any reasonably recent NVIDIA driver JITs
// this PTX for the actual GPU.
//
// NOTE for regenerating: nvcc must find a 64-bit cl.exe first in PATH
// (...\VC\Tools\MSVC\<ver>\bin\Hostx64\x64). If a Hostx64\x86 cl is found
// instead, the device pass loses _WIN64 and fails with a bogus
// "invalid redeclaration of type name size_t" from vcruntime.h.
//
// Layouts:
//   src/dst float32 RGB: tightly packed, 3 floats per pixel, row-major.
//   texture RGBA16F:     4 halfs per pixel (R,G,B,A), rows padded to a
//                        driver-chosen pitch in bytes (multiple of 256).

#include <cuda_fp16.h>
#include <cstddef>

extern "C" __global__ void dlss5nr_rgb_to_rgba16f(
    const float* __restrict__ src,
    unsigned short* __restrict__ dst,
    size_t pitch_bytes,
    int w,
    int h) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    const float* p = src + static_cast<size_t>(y) * w * 3 + x * 3;
    // Scalar loads: the source row stride (w*3 floats) is not guaranteed to
    // be 16-byte aligned, so a vectorized float3 load would be unsafe.
    const float r = fminf(fmaxf(p[0], 0.0f), 1.0f);
    const float g = fminf(fmaxf(p[1], 0.0f), 1.0f);
    const float b = fminf(fmaxf(p[2], 0.0f), 1.0f);

    // One RGBA16F pixel is exactly 8 bytes: pack RG and BA into two u32s and
    // store them as a uint2. The bridge validates at registration that the
    // mapped base pointer and row pitch are 8-byte aligned, so this store is
    // always aligned. (Do NOT use uint4 here — it would advance x by 16 bytes
    // per pixel and only every second pixel would be written.)
    const unsigned short hr = __half_as_ushort(__float2half_rn(r));
    const unsigned short hg = __half_as_ushort(__float2half_rn(g));
    const unsigned short hb = __half_as_ushort(__float2half_rn(b));
    const unsigned short ha = __half_as_ushort(__float2half_rn(1.0f));

    uint2 v;
    v.x = (static_cast<unsigned int>(hg) << 16) | hr;
    v.y = (ha << 16) | hb;
    reinterpret_cast<uint2*>(dst + static_cast<size_t>(y) * (pitch_bytes / 2))[x] = v;
}

extern "C" __global__ void dlss5nr_rgba16f_to_rgb(
    const unsigned short* __restrict__ src,
    float* __restrict__ dst,
    size_t pitch_bytes,
    int w,
    int h) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    // One RGBA16F pixel is exactly 8 bytes (uint2), not 16.
    const uint2 v = reinterpret_cast<const uint2*>(
        src + static_cast<size_t>(y) * (pitch_bytes / 2))[x];
    const unsigned short hr = v.x & 0xffffu;
    const unsigned short hg = v.x >> 16;
    const unsigned short hb = v.y & 0xffffu;

    float* p = dst + static_cast<size_t>(y) * w * 3 + x * 3;
    // Clamp like the CPU staging path so both paths return identical ranges.
    p[0] = fminf(fmaxf(__half2float(__ushort_as_half(hr)), 0.0f), 1.0f);
    p[1] = fminf(fmaxf(__half2float(__ushort_as_half(hg)), 0.0f), 1.0f);
    p[2] = fminf(fmaxf(__half2float(__ushort_as_half(hb)), 0.0f), 1.0f);
}
