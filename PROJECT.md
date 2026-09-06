# ComfyUI-DLSS5 — Project Orientation Document

> **For AI assistants and new contributors: read this file first.** It is the single
> source of orientation for the project. Everything below was verified against the code
> as of v0.9.28 (2026-09-05).

## 1. What this project is

An **unofficial, experimental** ComfyUI custom node that runs **DLSS 5 Neural Rendering**
(NVIDIA NGX feature id **18**) *in-process* on Windows. There is no external executable and
no image files are written — the frame goes through D3D12/NGX inside the ComfyUI Python
process and comes back as a normal `IMAGE` tensor.

- Node category: `experimental/DLSS 5 NR`
- Node (class keys are **stable** — saved workflows reference them; do not rename):
  - `DLSS5NeuralRendering` → display name `🕹️DLSS 5 (OreX)` — the main processing node.
- **Neural Rendering only.** At 1x the output resolution equals the input resolution. This
  bridge has **no DLSS Super Resolution models**; real motion-vector input (`DLSSNR.MVec`)
  is available since v0.9.0 via `temporal_mode = optical_flow`.
- Not supported or endorsed by NVIDIA or Comfy Org. The integration relies on observed,
  undocumented runtime behavior (see §8).

## 2. Hard rules — do not break these

1. **Work only inside this folder.** The user mirrors changed files manually into their
   ComfyUI install (`custom_nodes/ComfyUI-DLSS5`) after each round; the project folder is
   the single source of truth. Do not inspect or modify the user's ComfyUI installation.
2. **Never commit or ship NVIDIA binaries**: `_nvngx.dll`, `nvngx_dlssnr.dll`, any other
   `nvngx_*` runtime, and NGX SDK headers. `.gitignore` enforces this for git; users supply
   their own legally obtained `runtime/nvngx_dlssnr.dll`. `_nvngx.dll` is discovered from
   the installed NVIDIA driver DriverStore (optional local override: `runtime/_nvngx.dll`).
3. Keep all project material described as **unofficial/experimental**; never imply NVIDIA
   endorsement, never link to leaked or modified NVIDIA binaries.
4. **Windows-only** (D3D12/NGX). Python dependencies are only `numpy` plus `torch`
   (provided by ComfyUI itself); everything else is stdlib (`ctypes`, `hashlib`, …).
5. The dev machine's GPU is occupied by the LLM that writes this code — **live NR runs are
   impossible there**. Verify changes with syntax checks only; the user tests on their own
   machine (RTX 5090, driver 616.56) and reports back.

## 3. File map (current tree)

| Path | Role |
|---|---|
| `__init__.py` | ComfyUI entry point: `NODE_CLASS_MAPPINGS`, `NODE_DISPLAY_NAME_MAPPINGS`, `WEB_DIRECTORY = "./js"`, and the frame-preview API (v0.9.27): `POST /dlss5/preview-arm` (arm single-frame capture for the next run of the node), `POST /dlss5/preview-disarm`, `GET /dlss5/preview-status` (idle/armed/done/error), `GET /dlss5/preview-result` (PNG, consumed on fetch). |
| `OreX_DLSS5.py` | All Python: constants, ctypes loading/ABI, both node classes. `__version__` lives here. |
| `js/OreX_DLSS5.js` | Frontend extension: OreX-style canvas tooltips (EN + RU) for every widget of both nodes; suppresses the built-in tooltip. Since v0.9.26 also draws the Preview/Hide buttons (on canvas, under `change_frame`) and the preview image, with a per-type `computeSize` patch reserving their height. Since v0.9.28 Preview arms the server (`POST /dlss5/preview-arm`), builds the prompt with `app.graphToPrompt()` and prunes it to this node + its upstream (the rgthree-comfy "Queue Selected Output Nodes" pattern, `_dlss5AddUpstream()`), queues only that subgraph via `app.api.queuePrompt()` — downstream nodes are absent from the prompt, so no outputs are written — then polls `/dlss5/preview-status` until the PNG is ready (early exit on the `execution_error` websocket event). |
| `bridge/bin/dlss5nr_bridge.dll` | **Project-owned** prebuilt Windows x64 bridge (D3D12 + NGX + CUDA/D3D12 interop). The only native binary that ships. Folder renamed from `native/` in v0.9.4. |
| `bridge/dlss5nr_bridge.cpp` | Bridge source (restored to the tree in v0.7.0): D3D12/NGX session, CPU staging path, and the CUDA interop path — dynamic `nvcuda.dll` loading, external-memory import of the shared-heap frame textures (`CreateSharedHandle` → `cuImportExternalMemory`), GPU-side fence handoff via an external semaphore, embedded-PTX conversion kernels launched on the caller's stream. Since v0.9.0 also owns the motion-vector channel: a full-res R16G16_FLOAT committed texture fed to `DLSSNR.MVec`, populated from NVOF flow by nearest upsample of normalized UVs. |
| `bridge/nvof_flow.h` / `bridge/nvof_flow.cpp` | NVIDIA Optical Flow integration (v0.9.0, ported from lisitskyaa/ComfyUI-DLSS5-NR, MIT — see THIRD_PARTY_NOTICES.md): loads `nvofapi64.dll` from the display driver, runs NVOF on a private D3D11 device created on the same adapter as the bridge's D3D12 device (luma-only input, grid 2, perf level FAST), ping-pong frame registration, S10.5 flow extraction into `NvofFlowFrame`. All calls SEH-guarded; session cached per size+adapter LUID. |
| `bridge/caller_shim.cpp` | Source of the caller-forwarding helper. |
| `bridge/build_bridge.ps1` | Build script (was `native/build_native.ps1` until v0.9.4): auto-finds MSVC `cl.exe` + Windows SDK; builds the shim and the bridge (`/O2`, two sources since v0.9.0: `dlss5nr_bridge.cpp` + `nvof_flow.cpp`; links `d3d12.lib d3d11.lib dxgi.lib ole32.lib`). No CUDA toolkit or NGX headers required. |
| `bridge/cuda_interop.cu` | Source of truth for the two conversion kernels (not compiled into the DLL); documents how to regenerate the PTX with nvcc. |
| `bridge/cuda_kernels_ptx.h` | Embedded PTX (`.version 8.7`, compute_75) loaded at runtime via `cuModuleLoadData`; the bridge has no build-time CUDA dependency. |
| `runtime/nvngx_dlssnr.dll` | **User-supplied** NVIDIA Neural Rendering runtime (~158 MiB). Never committed; required at run time. Dev-machine deployment (verified 2026-09-04): build **310.8.SF-v2**, SHA-256 `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927`, source: RankFTW/rhi-repo release `dlssnr-310.8.SF-v2`. Contains native CUBIN for sm_75/sm_86/sm_89/sm_120 + PTX sm_120 — RTX 20/30/40/50 all covered, so no build swap is needed for older cards. |
| `runtime/caller/nvngx.dll_comfy.dll` | **Project-owned** thin caller-forwarding helper (~104 KiB); its module name satisfies the observed Neural Rendering caller check. |
| `runtime/README.txt` | Explains to end users which DLLs they must provide themselves. |
| `PROJECT.md` | This file. |
| `README.md` | User-facing install guide + parameter table. |
| `CHANGELOG.md` | Version history (user-facing). |
| `LICENSE` | MIT — covers project-owned code only, never NVIDIA material. |
| `THIRD_PARTY_NOTICES.md` | Legal separation of project code vs NVIDIA/reference-repo material. |
| `requirements.txt` | `numpy` (torch comes from ComfyUI). |
| `.gitignore` | Ignores build outputs and all NVIDIA runtimes. |
| `.comfyignore` | Excludes developer-only files from ComfyUI-Manager packages. |

### Files intentionally removed from the working tree (v0.5.1 cleanup)

All of these are **recoverable from this repository's git history** unless noted:

- GitHub publishing: `.github/workflows/ci.yml`, `.github/workflows/release.yml`,
  `tools/package_release.ps1`, `tools/check_no_proprietary_binaries.py`,
  `pyproject.toml.example`, `CONTRIBUTING.md`, `SECURITY.md`.
- Diagnostics: `diagnose_runtime.bat`, `diagnose_runtime.ps1`,
  `tools/check_channel_order.py` (the last one was **untracked** — its deletion is irreversible).
- Old docs folder `docs/` (`ARCHITECTURE.md`, `LEGAL_NOTES.md`, `PUBLISHING.md`,
  `RELEASE_NOTES_v0.2.0.md`) — useful content was folded into this file.

## 4. Architecture and data path

```text
ComfyUI IMAGE [B,H,W,C] (torch)
    |
    | on the source device (v0.6.0): >1x bicubic resample, scene-change luma + downsample
    v
ctypes (OreX_DLSS5.py)
    |
    |  v0.8.0: if the input tensor is on the bridge's own GPU and CUDA interop is
    |  live -> dlss5nr_process_cuda(raw pointers + caller stream handle): pixels
    |  stay GPU-resident end-to-end (zero PCIe). Otherwise, or after any CUDA-path
    |  failure (whole batch retried with forced resets) -> CPU staging path.
    v
dlss5nr_bridge.dll   (project-owned, D3D12 + CUDA interop)
    +-- DXGI NVIDIA adapter selection by gpu_index
    +-- D3D12 device / command queue / fence
    +-- NGX core discovery & init (uses _nvngx.dll from driver DriverStore or runtime/ override)
    +-- loads user-supplied nvngx_dlssnr.dll via the caller shim
    +-- Neural Rendering feature 18, RGBA16F input/output textures
    +-- CUDA interop (v0.8.0): nvcuda.dll loaded dynamically; frame textures live on
        D3D12_HEAP_FLAG_SHARED heaps and are imported as CUDA external memory
        (CreateSharedHandle -> cuImportExternalMemory, persistent mapped pointers);
        the D3D12 fence wrapped as an external semaphore hands work between the GPU
        queues without CPU round-trips; embedded PTX kernels convert RGB(f32) <->
        RGBA16F on the caller's stream around the NGX evaluation
    |
    v
channel-order auto-detection (RGB vs swapped R/B) — on-GPU in the CUDA path,
CPU numpy in the staging path
    |
    | (staging path only: batch download -> pinned host buffer -> upload back)
    v
ComfyUI IMAGE
```

Key implementation facts:

- The D3D12/NGX session and feature are **persistent** inside the ComfyUI Python process and
  reused when compatible with the next image's dimensions/settings; switching `gpu_index`
  triggers `dlss5nr_shutdown()` + re-init. All entry points are guarded by a module-level
  `threading.RLock`.
- `os.add_dll_directory` handles for `bridge/bin`, `runtime`, `runtime/caller` are kept in a
  module list for the process lifetime (CPython removes a directory when its handle is GC'd).
- **ctypes API surface** of `dlss5nr_bridge.dll`:
  - `int dlss5nr_init(int gpu_index, const wchar_t* runtime_dir, char* err_buf, int err_len)`
    — called with `(gpu_index, str(runtime/), 4096-byte buffer, len)`.
  - `int dlss5nr_process(float* in_rgb, float* out_rgb, int w, int h,
    int style, int preset, float intensity, float tone, float structure, float skin,
    int automask, int reset, char* err_buf, int err_len)`
    — CPU float32 in/out; `reset=1` clears temporal history; error text comes back in
    `err_buf`. (The 4096-byte buffer is allocated per call on the Python side.)
  - `int dlss5nr_process_cuda(uint64_t src_dev, uint64_t dst_dev, int w, int h,
    uint64_t cuda_stream, int style, int preset, float intensity, float tone,
    float structure, float skin, int automask, int reset, char* err_buf, int err_len)`
    — v0.7.0 zero-PCIe path; `src_dev`/`dst_dev` are raw torch tensor device
    pointers (float32 RGB, contiguous), `cuda_stream` is the caller's stream handle
    (`torch.cuda.current_stream(device).cuda_stream`).
  - `int dlss5nr_cuda_supported(void)` — 1 when interop is live (requires init).
  - `int dlss5nr_cuda_status(char* buf, int cap)` — 1 + "ok", or 0 + the reason
    interop is unavailable.
  - `void dlss5nr_shutdown(void)`
  - `const char* dlss5nr_version(void)`, `const char* dlss5nr_gpu_name(void)`
- **v0.8.0 CUDA interop path** (the zero-PCIe design, external memory):
  - GPU matching is by **name, not index**: the DXGI adapter description is compared
    against each CUDA device name (case-insensitive equality or substring either way)
    and `cuDevicePrimaryCtxRetain` takes that device's primary context — the same one
    PyTorch uses by default, so raw tensor pointers are directly addressable. Python
    additionally requires `rgb.device.index == gpu_index`.
  - Setup (once per size): frame textures are placed on explicit
    `D3D12_HEAP_FLAG_SHARED` heaps (`CreateHeap` + `CreatePlacedResource` at the
    footprint offset; the heap size = max(copyable total, data end, driver placement
    estimate `pitch × ceil(h/128) × 128` — on this GPU/driver the placement height is
    rounded to a multiple of 128 rows and `GetResourceAllocationInfo` returns {0, 1} —
    plus its `SizeInBytes` when non-zero) rounded **up** to 64 KB. If the driver still
    rejects the placement, the heap grows in 64 KB steps and is retried (up to 31 times),
    so allocation succeeds for any resolution; a hard failure reports all of
    w/h/pitch/offset/copyable/alloc/align/heap); each heap is exported with
    `ID3D12Device::CreateSharedHandle` and imported into this process's CUDA context
    via `cuImportExternalMemory` (type D3D12_HEAP, NT handle, dedicated flag). The
    `cuExternalMemoryGetMappedBuffer` pointer is **persistent** — no per-frame
    map/unmap. Row pitch = footprint RowPitch; validated ≥ width·8 and 8-byte aligned
    at import. Note: `cuGraphicsD3D12RegisterResource` does not exist in the CUDA
    driver API (v0.7.0 was built on it, so that path never activated — fixed here).
  - Per frame: `cuCtxSetCurrent(torch ctx)` → PTX kernel RGB(f32)→RGBA16F writes into
    the mapped D3D12 memory → `cuSignalExternalSemaphoresAsync` on the fence-wrapped
    external semaphore (value v_in) → `ID3D12CommandQueue::Wait` on v_in (**GPU-side**
    ordering — no CPU round trip) → set params + NGX eval via the shim
    (`ExecuteAndWait`, CPU blocks during inference, signals v_out) →
    `cuWaitExternalSemaphoresAsync` on v_out → PTX kernel RGBA16F→RGB(f32) into the
    torch output slot, **left async** on the caller's stream (every later PyTorch op
    on that stream is ordered after it; the CPU returns while the GPU finishes).
  - No barriers in this path: `g_color` stays `NON_PIXEL_SHADER_RESOURCE` and
    `g_output` stays `UNORDERED_ACCESS` permanently — both are also the terminal
    states of the CPU path, so the two paths can be mixed freely across frames.
  - Error invariants: if `cuSignalExternalSemaphoresAsync` fails the bridge bails
    **before** enqueuing the D3D12 fence wait (waiting on a value that will never be
    signaled would hang the queue). An NGX eval failure after the wait was enqueued is
    drained with `ExecuteAndWait` (the input kernel signals v_in regardless) and does
    not disable CUDA. An output-side `cuWait` failure leaves dst unwritten, so Python
    retries the batch on the CPU path.
  - The async output tail is tracked in `g_last_cuda_stream`;
    `ReleaseCudaExternal()` (size change / shutdown) synchronizes that stream first,
    then destroys the external semaphore and both external memories — destroying them
    releases the driver's reference to the shared heaps, so the D3D12 ComPtrs are
    reset only afterwards.
  - Kernels: two PTX functions (compute_75, `.version 8.7`) taking
    `(u64 src, u64 dst, u64 pitch_bytes, u32 w, u32 h)`, grid (ceil(w/32), ceil(h/8)),
    block (32,8), `uint2` (8-byte/pixel) accesses. The PTX is embedded in the DLL
    (`cuda_kernels_ptx.h`) and loaded with `cuModuleLoadData`; `nvcuda.dll` is
    resolved via `GetProcAddress`, so building needs only MSVC + Windows SDK.
  - Any failure during interop init (no nvcuda, no matching GPU, module load error)
    is non-fatal: the CPU staging path stays fully functional and
    `dlss5nr_cuda_supported()` reports 0 with the reason in `dlss5nr_cuda_status`.
- Python-side processing:
  - `upscaling_mode` > 1x resamples the source (bicubic, antialiased) **before** the neural
    pass; 1x runs at native resolution. Output dimensions are rounded to even numbers (min 2).
  - `temporal_mode`: `none` resets only the first frame; `auto`/`optical_flow` reset when the mean
    luminance difference between consecutive frames exceeds `scene_change_threshold`. Since
    v0.9.0, `optical_flow` additionally runs NVIDIA Optical Flow (driver-supplied
    `nvofapi64.dll`, see `bridge/nvof_flow.*`) on each raw input frame and feeds the resulting
    S10.5 flow — nearest-upscaled to full-res normalized UVs in an R16G16_FLOAT texture — into
    `DLSSNR.MVec` (with `MVecScaleX/Y = width/height`). It forces the CPU staging path for
    sequences of 2+ frames; the first frame and every scene-cut frame carry zero vectors. If
    the driver lacks the NVOF API the node prints a warning and falls back to `auto`.
  - `warmup_frames`: after each history reset the frame is re-evaluated N extra times (only
    the first evaluation carries `reset=1`) so temporal accumulation settles before its output
    is kept.
  - Limits mirror the reference enhancer: long edge ≤ 7680, short edge ≤ 4320.
  - **v0.6.0 performance:** >1x resampling and scene-change luma run on the source
    device before the single batch download (previously they ran per-frame on CPU
    copies). Both batch copies use cached page-locked staging buffers
    (`_pinned_buffer`, cap 4 GiB, per-shape LRU eviction) so CUDA DMA runs at full
    bandwidth; on allocation failure or cap overflow the code falls back to plain
    pageable copies. The output array aliases the pinned input buffer — safe because
    frame *i* is fully consumed before slot *i* is overwritten and in/out shapes match.

## 5. Node parameters (`DLSS5NeuralRendering`)

| Widget | Type / range | Default | NGX parameter | Notes |
|---|---|---|---|---|
| `upscaling_mode` | combo | `1.5x (Quality)` | — | `1x`, `1.5x (Quality)`, `1.724x (Balanced)`, `2x (Performance)`, `3x (Ultra Performance)`. No SR models: >1x = resample + neural pass at target size. Default since v0.9.8. |
| `dlss_model_preset` | combo | `M` | — (**reserved**) | `Default/J/K/L/M` mirror the reference enhancer, but the feature-18 runtime exposes **no** model-preset parameter (verified against the DLL string table: 61 `DLSSNR.*` parameters, none for model selection). Validated, not forwarded; does not change output. |
| `image` | IMAGE | — | — | Batch order = temporal order. |
| `style` | combo | `natural` | style int | `default`=0, `natural`=1, `cinematic`=2 (raw ints from old workflows accepted). |
| `intensity` | FLOAT 0–2, step .05 | 1.0 | intensity | **Measured: values above 1.0 have no further effect**; below 1.0 blends toward the source. |
| `tone` | FLOAT 0–2, step .05 | 2.0 | `DLSSNR.LocalToneStrength` | Local tone/contrast enhancement. Default raised to 2.0 in v0.9.26. |
| `structure` | FLOAT 0–2, step .05 | 2.0 | `DLSSNR.LocalStructureStrength` | Texture structure reconstruction; ~1.5 is a good balance for AI-generated footage. Default raised to 2.0 in v0.9.26. |
| `skin` | FLOAT −1–2, step .05 | 2.0 | `DLSSNR.SkinStructureStrength` | Skin/pore reconstruction. **Requires `auto_mask`** — with the mask off it does nothing. Default raised to 2.0 in v0.9.26 (−1.00 = native default, model decides). |
| `auto_mask` | BOOLEAN | False | automask | Experimental runtime-generated mask; may cause flicker/inconsistency. Also the gate for `skin`. |
| `temporal_mode` | combo | `auto` | reset logic + `DLSSNR.MVec` | `auto` (scene-change resets only), `optical_flow` (same resets **plus** real NVOF motion vectors into `DLSSNR.MVec`; CPU staging path; falls back to `auto` if the driver has no `nvofapi64.dll`), `none`. |
| `scene_change_threshold` | FLOAT 0.01–1, step .01 | 0.24 | reset logic | Mean luminance change above which history resets. |
| `warmup_frames` | INT 0–16 | 0 | — | Extra evaluations after each reset. |
| `gpu_mode` | BOOLEAN | True | init + VRAM move | On = 🟢 GPU ON: the bridge runs on the first NVIDIA GPU (nvidia-smi order) and the node moves a CPU input batch into its VRAM when it fits. Off = 🔴 GPU OFF: frames stay where they are, the bridge still runs on the first GPU. Replaced the old `gpu_index` INT in v0.9.15. |
| `chunk_frames` | INT 0–256 | 64 | — | v0.9.23 memory-saving mode: 0 = off (whole batch at once), >0 = that many frames per chunk, assembled into one full-length tensor; temporal history carries across chunk boundaries. Default raised to 64 in v0.9.26. |
| `change_frame` | INT 0–100000 | 0 | — | v0.9.27 (reworded v0.9.28): frame index for the canvas-drawn Preview button below (0 = first frame). Not used by a full run; Preview queues only the subgraph feeding this node and it processes just this one frame of its input batch during that run (temporal history starts fresh at it); an index beyond the end of the sequence is clamped to the last frame. Downstream nodes are not in the pruned prompt, so a preview writes no outputs. |

Widget order in the UI is exactly the dict order above (v0.3.0 moved `upscaling_mode` first
and replaced the old `batch_mode` with `motion`, renamed to `temporal_mode` in v0.9.5;
v0.4.0 inserted `dlss_model_preset`; v0.9.22 removed `preset`; v0.9.23 added `chunk_frames`
under `gpu_mode`; v0.9.26 added `change_frame` last, with the canvas-drawn Preview/Hide
buttons directly under it). Saved workflows from older versions may need widget values
re-checked.

**There are deliberately no `"tooltip"` fields in `INPUT_TYPES`** (since v0.5.1) — see §6.

## 6. Frontend: OreX-style tooltips (`js/OreX_DLSS5.js`)

ComfyUI renders widget fields as **DOM elements above the canvas**, so a canvas-level
`onMouseMove` hit-test never fires over them. The extension therefore uses two trigger paths:

1. **Primary (works for all standard widgets):** in `onDrawForeground`, for every widget whose
   `w.inputEl` exists, bind one-time `pointerenter`/`pointerleave` listeners (flag
   `el._dlss5TooltipBound`). On enter: match the widget against `DLSS5_HELP_DESCRIPTIONS`
   by normalized name/label, start an 800 ms hover timer, then set
   `this.dlss5ActiveTooltip` + `setDirtyCanvas(true)`. On leave: clear state.
2. **Fallback:** a patched `onMouseMove` hit-tests `w.last_y` / `w.computeSize(...)` for
   areas that have no DOM element (same mechanism as the user's OreX camera/crop nodes).

**Built-in tooltip suppression (v0.5.1):** the plain ComfyUI tooltip used to render on top of
the custom box. It is now killed at the source — `INPUT_TYPES` contains no `"tooltip"` keys,
so ComfyUI has nothing to display. The JS additionally keeps a defensive sweep in
`onDrawForeground` (`w.tooltip = null`, clear `el.title`) in case a future frontend re-adds
tooltips from somewhere else.

**Style constants** (the "OreX look"): box at `x = nodeWidth + 25`, vertically centered on the
widget; margin 12; rounded rect r=6 with a left arrow (8×6); fill `rgba(18,18,18,0.98)`;
shadow `rgba(0,0,0,0.8)` blur 10 offsetY 4 (cleared to transparent before stroking);
**stroke `rgba(0,150,255,0.7)` lineWidth 1.5** (the signature blue border); title
`bold 13px #ffffff` = `${icon} ${name}`; EN line `11px #cccccc` = `EN: …`; RU line
`11px #999999` = `RU: …`; text word-wraps at 420 px.

**Help table:** `DLSS5_HELP_DESCRIPTIONS` holds 14 entries `{name, icon, desc, ru_desc}` —
one per widget (icons: 🔍 🧠 🎨 🎚️ 🌗 🧵 🧑 🎭 🎞️ ✂️ ⏳ 🎮 🧩 🎯). The texts were supplemented
from the reference DLSS 5 Visual Enhancer web interface. **If you add/rename a widget in
Python, add/update its entry here or it will have no tooltip.**

**Preview/Hide controls (v0.9.26, reworked v0.9.27/v0.9.28):** two buttons drawn ON CANVAS
in one row under the `change_frame` widget (`_dlss5SetupNodeControls`, no DOM element — DOM
buttons did not render in all builds). Preview arms the server (`POST /dlss5/preview-arm`)
and then queues ONLY THE SUBGRAPH FEEDING THIS NODE — the rgthree-comfy "Queue Selected
Output Nodes" pattern: `app.graphToPrompt()` builds the full prompt (new frontends return a
`{workflow, output}` wrapper; classic builds return the bare node dict), `_dlss5AddUpstream()`
prunes it to this node + everything that feeds it (linked inputs are `[upstreamId, slot]`
arrays in a prompt dict; widget values are scalars or `{__value__: [...]}` wrappers, so a
plain array marks a link), and `app.api.queuePrompt(0, payload)` queues only that (the `app`
global is captured into module-level `_dlss5App`). Everything upstream runs exactly like a
full run; downstream nodes are absent from the prompt entirely — so a preview writes no
outputs (the v0.9.27 one-frame-video side effect is gone). When this node's `process()`
executes it sees the fresh arm (module-level `_preview_arm_ts` in `OreX_DLSS5.py`, TTL 600 s
so a stale arm cannot hijack a later ordinary run) and processes only the `change_frame`
slice of its input batch, stashing the rendered PNG in `_preview_result` (or the failure
text in `_preview_error`). The JS polls `/dlss5/preview-status` every 700 ms (10-minute
deadline, early exit on the `execution_error` websocket event — both `execution_error` and
the legacy `exec_error` name are listened, matched by `prompt_id` when available), fetches
the PNG from `/dlss5/preview-result` and draws it under the buttons (signature blue border,
"frame N" chip); Hide removes it. On any failure the JS POSTs `/dlss5/preview-disarm`.
Clicks are hit-tested in a wrapped `onMouseDown` (node-local coords, 2 px padding) and
consumed; hover sets a pointer cursor + OreX-blue border. A per-type `computeSize` patch
(`_dlss5PreviewExtraHeight`) reserves the button row + image height so nothing is clipped;
`refreshSize()` resizes the node on show/hide. No input batch is kept in RAM between runs
(the v0.9.26 `_preview_source` cache is gone). The node class carries `OUTPUT_NODE = True`
(v0.9.28) because ComfyUI accepts only output nodes as execution roots of a prompt and a
pruned subgraph contains no other one; in full runs it is inert (its upstream is already
pulled in by the real outputs downstream, and it returns no ui data), and it also makes the
node selectable in the frontend's "Queue Selected Output Nodes" menu.

**Registration hardening:** `_dlss5Register(app)` uses `beforeRegisterNodeDef` (patches the
node type prototype before widgets exist) plus a fallback patch of already-registered types
via `LiteGraph.registered_node_types`, and a top-level retry loop (50 ms × 200) if the `app`
global is not ready when the script loads. Success marker in the browser console:
`[OreX_DLSS5] extension registered (tooltips + Preview/Hide controls)`, plus one line per
node: `[OreX_DLSS5] Preview/Hide buttons ready (canvas-drawn) on node #N`.

## 7. Version history (condensed; full text in CHANGELOG.md)

- **0.1.x** — first in-process D3D12/NGX Neural Rendering node, still-image + temporal modes.
- **0.2.0** — first GitHub-release-oriented build: prebuilt bridge/helper via CI, automatic
  `_nvngx.dll` discovery from DriverStore, runtime diagnostics (GPU name, DLL SHA-256), CI
  guard against bundling NVIDIA runtimes.
- **0.3.0** — widget rework: `upscaling_mode` first, `batch_mode` replaced by `motion`.
- **0.4.0** — CPU float32 staging path; `dlss_model_preset` added (reserved); `preset` became
  a combo list; canvas tooltips introduced.
- **0.5.0** — node renamed to the "OreX" display names, blue-border tooltip style, tooltip
  texts supplemented from the reference enhancer UI, mappings moved to `__init__.py`.
- **0.5.1** — built-in plain tooltip removed (tooltips live only in JS); project cleanup
  (dev/publishing/diagnostic files and `docs/` deleted, see §3); this orientation document added.
- **0.6.0** — Python-side performance: on-device resampling + scene luma before the batch
  download, cached pinned staging buffers for both batch copies.
- **0.7.0** — new bridge with a CUDA/D3D12 interop path: zero-PCIe pixel path when
  the input is already on the bridge GPU (embedded PTX + dynamic `nvcuda.dll`, no CUDA
  toolkit needed), automatic fallback to CPU staging, on-GPU channel auto-detection,
  `prev_gray` NameError fix; bridge sources restored to the working tree. (The mechanism
  relied on `cuGraphicsD3D12RegisterResource`, which does not exist in the CUDA driver
  API — the path never activated and every frame silently used the CPU fallback.)
- **0.8.0** — interop rewritten on the real external-memory mechanism: shared-heap frame
  textures imported via `CreateSharedHandle` → `cuImportExternalMemory` (persistent mapped
  pointers), GPU-side fence handoff through an external semaphore (one fewer CPU↔GPU sync
  per frame), error invariants documented above; bridge version `0.4.0-external-memory`.
- **0.8.1** — fixed a size-dependent `CreatePlacedResource` failure: shared-heap sizes must be
  multiples of 64 KB, but `GetCopyableFootprints` totals are unaligned for many resolutions
  (544×704 failed while 768×768 passed); the heap size is now rounded up to 64 KB before
  `CreateHeap`, and allocation failures log the HRESULT; bridge version
  `0.4.1-external-memory`.
- **0.8.2** — heap sizing hardened for any resolution: sized from `GetResourceAllocationInfo`
  (the driver's true placement footprint) rather than the copyable total alone; distinct
  per-build bridge version strings (`0.4.2-external-memory`) so the Runtime Info node shows
  exactly which DLL is loaded. Field data confirmed the need: with a 0.8.1-era build, every
  exact 64 KB multiple worked and all half-integers failed, while 768×1344 (exactly 126 × 64 KB)
  failed where 896×1152 with the identical total passed — placement footprint exceeding the
  copyable total for some shapes.
- **0.8.3** — allocation made self-healing and fully diagnosable: field data showed
  896×1152 passing one run and failing the next, which is impossible under 0.8.2's sizing
  if that build were loaded — so either an old DLL was still imported or the driver's
  reported footprints are not a reliable upper bound on this GPU/driver. Now: the heap
  grows in 64 KB steps (up to 7 retries) when `CreatePlacedResource` rejects the placement;
  sizing also accounts for `GetResourceAllocationInfo` Alignment; failure messages carry
  all allocation numbers (w/h/pitch/offset/copyable/alloc/align/heap); bridge version
  `0.4.3-external-memory`.
- **0.8.4** — root cause found and fixed: the 0.8.3 diagnostics showed
  `alloc=0 align=1` — on this GPU/driver (RTX 5090, 616.56) `GetResourceAllocationInfo`
  returns {0, 1}, so fit-based sizing was inert; and the true placement rule, verified
  against all 17 observed sizes from every round, is that the driver rounds the height up
  to a multiple of 128 rows (every failure had h % 128 ≠ 0, every success h % 128 == 0).
  This also explains "896×1152 failing": at `upscaling_mode = 1.5x` the bridge allocates
  at the target 1344×1728 (h = 1728, not a multiple of 128). The placement is now
  estimated directly (`pitch × ceil(h/128) × 128`); the retry ladder (extended to 31
  steps) remains as insurance; bridge version `0.4.4-external-memory`.
- **0.9.0** — real `motion=optical_flow`: NVIDIA Optical Flow (`nvofapi64.dll` from the
  display driver, no user file) computes S10.5 flow on a private D3D11 device on the same
  adapter; the bridge nearest-upsamples it to full-res normalized UVs in an R16G16_FLOAT
  committed texture and feeds `DLSSNR.MVec` (scale = width/height, full-frame subrect);
  OF mode forces the CPU staging path with temporal=1; graceful fallback to `auto` when
  the driver lacks NVOF; Runtime Info reports NVOF availability; new exports
  `dlss5nr_nvof_available/grid/perf`; bridge version `0.5.0-external-memory-nvof`.

## 8. Legal notes (condensed from the former docs/LEGAL_NOTES.md)

- Project-owned material (Python node, D3D12/staging code, build scripts, caller shim) is MIT.
- **Never bundle** `_nvngx.dll`, `nvngx_dlssnr.dll`, other `nvngx_*` binaries or NGX SDK
  headers — in git history or release assets. Users supply their own runtime.
- The integration depends on observed **undocumented** behavior: NGX feature id 18, Neural
  Rendering parameter names, application/project identifiers used at NGX init, and the caller
  shim whose module name satisfies an observed caller check. These may be governed by NVIDIA
  license/contract restrictions — the main legal-review item before any production use or
  curated-registry publication.
- `Zonnery/dlss5-nr-player` is cited as a research/reference source; it had no visible
  open-source LICENSE at v0.2.0 preparation time — do not vendor its code.
- Distribution posture: publish source + project-owned prebuilt bridge/helper only, mark
  everything experimental, require users to provide their own NVIDIA runtime.

## 9. Development workflow and verification

- Syntax checks (the only automated verification possible on the dev machine):
  - `C:/Python312/python.exe -m py_compile __init__.py OreX_DLSS5.py`
  - `node --check js/OreX_DLSS5.js`
- After any change, tell the user **exactly which files to copy** into their ComfyUI
  `custom_nodes/ComfyUI-DLSS5`, then restart ComfyUI (Python changes) and hard-reload the
  browser (Ctrl+F5, JS changes).
- Rebuilding the bridge needs **MSVC + Windows SDK only** (no CUDA toolkit, no NGX headers —
  the conversion PTX is embedded and `nvcuda.dll` is loaded at runtime): sources live in
  `bridge/`, run `powershell -ExecutionPolicy Bypass -File bridge\build_bridge.ps1`. Normal
  users always use the prebuilt `bridge/bin/dlss5nr_bridge.dll`. Regenerating the PTX header
  (only if the kernels change) needs nvcc; see the comment at the top of `bridge/cuda_interop.cu`
  (64-bit `cl.exe` must be first in PATH for the device pass).
- Git note: the working tree contains uncommitted renames (old `nodes.py` → `OreX_DLSS5.py`,
  old `build_native.bat` files deleted) and the v0.5.x changes; commit when the user asks.

## 10. Roadmap / next optimizations

The structural win that v0.6.0 left as a plan — pixels crossing PCIe four times per frame —
was shipped in v0.8.0: CUDA/D3D12 external-memory interop keeps ComfyUI tensors GPU-resident
end-to-end (see §4). Remaining ideas, all **unverified** (formats/ABI not confirmed, and none
of it is testable on the dev machine whose GPU is occupied by the LLM):

- ~~**`DLSSNR.MVec` (+ `DLSSNR.MVecSubrect`)** — real motion-vector input~~ — **done in v0.9.0**:
  NVOF (`nvofapi64.dll` from the display driver, design ported from lisitskyaa/ComfyUI-DLSS5-NR)
  computes S10.5 flow at a 2× grid; the bridge nearest-upsamples it to full-res R16G16_FLOAT
  normalized UVs and feeds `DLSSNR.MVec` with `MVecScaleX/Y = (width, height)` (the pair is now
  parameterized — width/height in OF mode, 1.0f without). Open follow-up: the MVec channel is
  exercised only on the CPU staging path; extending it to the CUDA zero-PCIe path would need a
  GPU-side upsample kernel (or NVOF running on the D3D12 device) — see `bridge/nvof_flow.cpp`.
- **Frame pipelining** — reference measurements (Kim2091/vapourkit) show the blocking CPU
  wait on each evaluation is the dominant per-frame cost (~21 ms at 4K on an RTX 5080 Laptop);
  overlapping frame N+1's input conversion with frame N's evaluation cut the blocking wait to
  ~0.2 ms. Our `ExecuteAndWait` blocks the CPU during inference; pipelining would need two
  in-flight evaluations (double-buffered textures + a second feature instance, or an async
  eval API) — whether NGX allows concurrent evaluations of one feature is unverified.
- **Quality presets** — perseval-BLR/DLSS5-Video-Converter ships named (intensity, tone,
  structure, skin) profiles: Faithful 0.70/0.75/0.75/−1, Natural 1.0×3/−1,
  Strong-Cinematic 1.65/1.40/1.50/1.0, Extreme-Overdrive 2.50/2.00/2.00/1.5, plus a
  120-frame warmup and a scene-cut detector. A `quality_preset` combo mapping to those
  tuples would be a cheap node-level feature (no bridge change).
- **`DLSSNR.ControlMask` (+Subrect)** — user-supplied region mask for what the model may
  touch; could expose a "protect this area" input. Format unverified.
- **`DLSSNR.Depth` (+Subrect)** — depth-guided enhancement (e.g. from a monodepth estimate).
  Format unverified.
- **`DLSSNR.BidirectionalDistortionField` (+Subrect)** — temporal-distortion hint; niche,
  lowest priority.
- **Pure CUDA backend** — the runtime DLL exports the official NGX CUDA API family
  (`NVSDK_NGX_CUDA_Init*`, `NVSDK_NGX_CUDA_CreateFeature1(NVSDK_NGX_CUDADevice*)` where
  `CUDADevice = {void* cudaContext; void* cudaStream}`, textures passed as device pointers,
  internal formats RGBA16F/RG16F/R32F). A CUDA-native bridge could drop D3D12 entirely and
  remove the interop hop; deferred because the exact ABI is unverified against this runtime
  build and the external-memory path already works.
- `DLSSNR.UI` / `DLSSNR.UIAlpha` are game-HUD protection inputs — not relevant to ComfyUI.

Everything above can only be tested on a machine with a free NVIDIA GPU (the user's RTX 5090).
