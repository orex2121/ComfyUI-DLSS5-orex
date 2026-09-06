#pragma once

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ComfyUI-DLSS5-NR contributors
//
// Ported from lisitskyaa/ComfyUI-DLSS5-NR (MIT). The D3D11 function-table
// layout used here was independently exercised by the MIT-licensed
// NIGos/dlss5-bridge project. See THIRD_PARTY_NOTICES.md for attribution.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <string>
#include <vector>

// Coarse S10.5 optical-flow field produced by NVIDIA Optical Flow (NVOFA).
// Each cell stores signed X,Y displacement in units of 1/32 pixel.
struct NvofFlowFrame {
    bool has_flow = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t grid = 0;
    std::vector<int16_t> xy;
};

// Prepare the temporal optical-flow field for one raw RGB input frame.
// - reset=true starts a new sequence; this frame only primes the previous-frame slot.
// - on the first frame out.has_flow is false and the caller should supply zero MVs.
// - subsequent frames return current->previous flow, matching the DLSS reprojection direction.
bool NvofPrepareFrame(
    IDXGIAdapter1* adapter,
    const float* rgb,
    uint32_t width,
    uint32_t height,
    bool reset,
    NvofFlowFrame& out,
    std::string& error);

// Releases the active OFA session and its D3D11 textures, but keeps no contract state.
void NvofReleaseSession();

// Full teardown including nvofapi64.dll.
void NvofShutdown();

// Lightweight driver check used by Runtime Info. Does not create an OFA session.
bool NvofDriverApiAvailable();

// Fixed settings used by this release.
uint32_t NvofGridSize();
uint32_t NvofPerfLevel();
