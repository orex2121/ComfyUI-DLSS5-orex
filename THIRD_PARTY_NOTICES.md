# Third-Party Notices

This file distinguishes the project's MIT-licensed code from third-party technology and names referenced by the integration.

## NVIDIA

NVIDIA, DLSS, NGX, GeForce and related names/trademarks are property of NVIDIA Corporation and/or its affiliates.

This repository does **not** include NVIDIA proprietary runtime binaries or NVIDIA NGX SDK headers. In particular, it does not include `_nvngx.dll` or `nvngx_dlssnr.dll`.

The native bridge declares only the minimum ABI shapes/function signatures it needs to dynamically call an installed/user-supplied NGX runtime. Those interfaces and identifiers correspond to NVIDIA technology; this project's MIT license does not grant rights in NVIDIA software, APIs, trademarks or binaries.

NVIDIA's public DLSS repository marks NGX headers as NVIDIA proprietary. Users and redistributors should review the license terms applicable to the NVIDIA software they use.

Reference: https://github.com/NVIDIA/DLSS

## Zonnery/dlss5-nr-player

The project was informed by behavior documented/exposed by the experimental `Zonnery/dlss5-nr-player` repository, including use of NGX feature id 18 and Neural Rendering parameter names.

As of preparation of v0.2.0, that repository did not expose an explicit open-source LICENSE file in its repository root. This repository therefore does not vendor its source files or claim a license to redistribute them.

Reference: https://github.com/Zonnery/dlss5-nr-player

## lisitskyaa/ComfyUI-DLSS5-NR (MIT)

The NVIDIA Optical Flow integration added in v0.9.0 (`bridge/nvof_flow.h` / `bridge/nvof_flow.cpp`) is ported from the MIT-licensed `lisitskyaa/ComfyUI-DLSS5-NR` project: its D3D11-based NVOF session management (loading `nvofapi64.dll` from the display driver, luma-only input preparation, ping-pong frame registration, S10.5 flow extraction) is used here with modifications for this bridge's in-process D3D12 pipeline. The ported files carry their own SPDX MIT notices and attribution comments.

Reference: https://github.com/lisitskyaa/ComfyUI-DLSS5-NR

## NIGos/dlss5-bridge (MIT)

The D3D11 function-table layout exercised by the optical flow code (`NvOFAPICreateInstanceD3D11` export, slot indices for CreateSession/Init/Register/Unregister/Execute/Destroy/LastError) was independently validated against the MIT-licensed `NIGos/dlss5-bridge` project before being used here.

Reference: https://github.com/NIGos/dlss5-bridge

## ComfyUI

ComfyUI and Comfy Org are separate projects. This custom node is unofficial and is not endorsed by Comfy Org.

Reference: https://github.com/Comfy-Org/ComfyUI
