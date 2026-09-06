# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ComfyUI-DLSS5-Ox contributors

import ctypes
import io
import math
import os
import platform
import threading
import time
from pathlib import Path

import numpy as np
import torch
from PIL import Image as _PILImage

# Импорты для работы API эндпоинтов Preview
import server
from aiohttp import web

_ROOT = Path(__file__).resolve().parent
_BRIDGE = _ROOT / "bridge" / "bin" / "dlss5nr_bridge.dll"
_RUNTIME = _ROOT / "runtime"

_lock = threading.RLock()
_lib = None
_initialized_gpu = None
_dll_directory_handles = []
# v0.9.1: bridge-side optical-flow fallback reasons already printed, so a
# session-latched latch is reported once per reason instead of every run.
_motion_warnings = set()

# v0.9.27 (reworded v0.9.28): preview-run state. The Preview button
# (js/OreX_DLSS5.js) POSTs /dlss5/preview-arm and then queues ONLY the
# subgraph feeding this node — the prompt is pruned client-side to this node +
# its upstream (the rgthree-comfy "Queue Selected Output Nodes" pattern), so
# downstream nodes are not in the prompt at all and no outputs are written.
# Everything upstream runs exactly as in a full run, and when this node's
# process() executes it sees _preview_arm_ts is fresh and processes ONLY the
# change_frame slice of its input batch. The wrapper
# stashes the rendered PNG in _preview_result (or the failure text in
# _preview_error); the JS polls /dlss5/preview-status and fetches
# /dlss5/preview-result. An arm older than _PREVIEW_ARM_TTL is stale (e.g. the
# queued run died before reaching this node) and is ignored, so it cannot turn
# a later ordinary run into a preview. The v0.9.26 _preview_source batch cache
# is gone — no input tensor is kept in RAM between runs anymore.
_preview_arm_ts = 0.0        # time.time() when armed; 0.0 = not armed
_PREVIEW_ARM_TTL = 600.0     # seconds an arm stays valid (10 min)
_preview_result = None       # {"png": bytes, "frame": int} after an armed run
_preview_error = None        # str set when the armed run's process() failed

# ==============================================================================
# HTTP ROUTES FOR PREVIEW SUPPORT
# ==============================================================================

@server.PromptServer.instance.routes.post("/dlss5/preview-arm")
async def dlss5_preview_arm(request):
    global _preview_arm_ts, _preview_result, _preview_error
    _preview_arm_ts = time.time()
    _preview_result = None
    _preview_error = None
    return web.json_response({"status": "armed"})

@server.PromptServer.instance.routes.get("/dlss5/preview-status")
async def dlss5_preview_status(request):
    global _preview_arm_ts, _preview_result, _preview_error
    if _preview_error:
        return web.json_response({"state": "error", "error": _preview_error})
    if _preview_result:
        return web.json_response({"state": "done"})
    if _preview_arm_ts > 0.0 and (time.time() - _preview_arm_ts) < _PREVIEW_ARM_TTL:
        return web.json_response({"state": "armed"})
    return web.json_response({"state": "idle"})

@server.PromptServer.instance.routes.get("/dlss5/preview-result")
async def dlss5_preview_result(request):
    global _preview_result
    if not _preview_result:
        return web.json_response({"error": "no result"}, status=404)
    return web.Response(
        body=_preview_result["png"],
        content_type="image/png",
        headers={"X-DLSS5-Frame": str(_preview_result["frame"])}
    )

@server.PromptServer.instance.routes.post("/dlss5/preview-disarm")
async def dlss5_preview_disarm(request):
    global _preview_arm_ts, _preview_result, _preview_error
    _preview_arm_ts = 0.0
    _preview_result = None
    _preview_error = None
    return web.json_response({"status": "disarmed"})

# ==============================================================================

# v0.9.21: gpu_mode is styled the way the OreX LM Studio nodes are — a native
# BOOLEAN widget with label_on/label_off ("🟢 GPU ON" / "🔴 GPU OFF"), no DOM
# overlay over its row (the v0.9.19 JS switch hid the row and did not render
# in all builds, so it was removed in v0.9.20).
# v0.9.22: the preset parameter (DLSSNR.Hint.Render.Preset combo) and the whole
# save/apply-preset feature (canvas Save button + /dlss5/presets API +
# presets.json) were removed — the user does without presets; process() still
# accepts a `preset` kwarg (default "Default") so old saved workflows keep
# working: ComfyUI drops unknown prompt inputs server-side, and js/OreX_DLSS5.js
# strips the stale leading widget value when loading old graphs.
# v0.9.23: chunk_frames INT widget (0 = off) — long sequences are processed in
# chunks of N frames so peak RAM/VRAM is O(chunk) + the output tensor instead of
# input batch + output batch at once. Frames keep their order on the same bridge
# session and scene-detect state carries across chunk boundaries, so the result
# matches an unchunked run (no boundary resets, no quality loss at the seams).
# v0.9.24: fix — chunked CPU staging crashed on frame 0 with "can't assign a
# numpy.ndarray to a torch.FloatTensor": PyTorch __setitem__ does not convert
# numpy values, so each result is now written via from_numpy + copy_.
# v0.9.25: chunked mode now uses VRAM — the CUDA/CPU path is chosen per chunk:
# a chunk that fits the free VRAM is moved to the bridge GPU and runs zero-copy
# (its result is copied back into the output tensor), so a sequence whose whole
# batch does not fit still gets VRAM-speed inference at O(chunk) peak VRAM.
# Also: log prefix renamed [DLSS5-Ox] -> [DLSS5-Ox].
# v0.9.26: presets.json is gone for good — the auto-creation at server start and
# the /dlss5/presets API that wrote it were removed (the preset feature left the
# node in v0.9.22; only dead backend code remained). New defaults: chunk_frames
# 64, tone/structure/skin 2.0. New change_frame INT widget under chunk_frames +
# canvas-drawn Preview/Hide buttons in one row below it: Preview runs the same
# pipeline on that one frame of the input batch the node kept from its last run
# (clamped to the last frame when out of range) via POST /dlss5/preview and
# draws the result under the buttons; Hide removes it. process() keeps the
# input batch referenced in _preview_source for this — replaced on every run,
# freed on restart.
# v0.9.27: Preview redesigned after field feedback — the v0.9.26 button relied
# on the input batch cached from a PREVIOUS full run, so it did nothing until
# the workflow had been queued at least once (and again after every restart).
# Now Preview arms the server (POST /dlss5/preview-arm) and queues the real
# graph via api.queuePrompt: upstream nodes execute normally ("run the scheme
# up to this point"), and process() — seeing the fresh arm — processes only the
# change_frame slice of its input (clamped to the last frame when out of range),
# encodes it to PNG and hands it back through /dlss5/preview-status +
# /dlss5/preview-result. The _preview_source cache, render_preview_frame and
# POST /dlss5/preview are gone: no batch is kept in RAM between runs, and the
# preview always uses the CURRENT inputs of the queued graph. Side effect by
# design: a preview run produces normal downstream outputs (e.g. a one-frame
# video) and shows progress in the UI like any other run.
# v0.9.28: Preview fixed after field testing — it now queues ONLY the subgraph
# feeding this node instead of the whole graph (the rgthree-comfy "Queue
# Selected Output Nodes" pattern, adopted from that node's recursiveAddNodes):
# js/OreX_DLSS5.js takes the prompt from app.graphToPrompt(), keeps just this
# node and everything linked into it, and queues that pruned dict. Downstream
# nodes are not in the prompt at all, so a preview run computes exactly one
# image and writes no outputs (the v0.9.27 one-frame-video side effect is gone).
# For the pruned prompt to pass server validation this node had to become an
# execution root: OUTPUT_NODE = True (inert in full runs — its upstream is
# already pulled in by the real outputs, and it returns no ui data, so nothing
# extra goes over the websocket; bonus: the node is now selectable in the
# frontend's "Queue Selected Output Nodes" menu). The JS also listens for the
# execution_error websocket event (v0.9.27 used the old exec_error name) and
# matches it against the queued prompt_id.
__version__ = "0.9.28"


class DLSS5NRError(RuntimeError):
    pass


# Output size relative to the source, mirroring the public DLSS mode labels.
_UPSCALING_MODES = {
    "1x (DLAA / native)": 1.0,
    "1.5x (Quality)": 1.5,
    "1.724x (Balanced)": 1.724,
    "2x (Performance)": 2.0,
    "3x (Ultra Performance)": 3.0,
}

# Neural rendering preset inside the DLSS model (DLSSNR.Hint.Render.Preset).
_NR_PRESETS = {"Default": 0, "Preset #1": 1, "Preset #2": 2, "Preset #3": 3}

# DLSS model selection as exposed by the reference DLSS 5 enhancer. The current
# feature-18 runtime (nvngx_dlssnr.dll) exposes no model-preset parameter in its
# NGX parameter table (verified against the DLL string table: 61 DLSSNR.*
# parameters, none for model selection), so this control is reserved: it is
# validated but does not change the output until a runtime build exposes it.
_DLSS_MODEL_PRESETS = {"Default": 0, "J": 10, "K": 11, "L": 12, "M": 13}

# optical_flow was removed from the options in v0.9.15 (the NVOF input path is
# not stable on this runtime and always errored); old workflows that saved it
# are clamped to "auto" in process(). The NVOF code stays dormant behind it.
_TEMPORAL_MODES = ["auto", "none"]

# v0.9.15: the gpu_index INT widget was replaced by a two-state toggle;
# v0.9.16 made it a real BOOLEAN switch widget (on = GPU ON 🟢, off = GPU OFF 🔴).
# On  == old 0 (bridge on GPU 0 + batch moved to cuda:0 if it fits);
# Off == old -1 (frames stay where they are, bridge still on GPU 0).
def _gpu_on(gpu_mode) -> bool:
    # Normalizes every shape this key has ever taken: the new boolean, the
    # v0.9.15 saved strings "GPU ON 🟢"/"GPU OFF 🔴", and anything truthy.
    if isinstance(gpu_mode, str):
        return "off" not in gpu_mode.lower()
    return bool(gpu_mode)

# Same supported boundary as the reference DLSS 5 enhancer: 8K long edge.
_MAX_LONG_EDGE = 7680
_MAX_SHORT_EDGE = 4320

# Page-locked staging for the batch download/upload around the native bridge.
# Pinned host memory lets CUDA DMA at full bandwidth instead of torch's chunked
# pageable path. Buffers are cached per shape and capped in total bytes so runs
# with varying resolutions cannot exhaust the host DMA budget; on allocation
# failure or cap overflow the caller falls back to plain pageable copies.
_PINNED_CACHE_CAP_BYTES = 4 * 1024 ** 3
_pinned_cache = {}
_pinned_lock = threading.Lock()


def _pinned_buffer(shape):
    key = tuple(int(s) for s in shape)
    with _pinned_lock:
        buf = _pinned_cache.get(key)
        if buf is not None:
            return buf
        size = int(np.prod(key)) * 4
        total = sum(b.numel() * b.element_size() for b in _pinned_cache.values())
        while _pinned_cache and total + size > _PINNED_CACHE_CAP_BYTES:
            oldest = next(iter(_pinned_cache))
            total -= _pinned_cache.pop(oldest).numel() * 4
        if total + size > _PINNED_CACHE_CAP_BYTES:
            return None
        try:
            buf = torch.empty(key, dtype=torch.float32, pin_memory=True)
        except (RuntimeError, MemoryError):
            return None
        _pinned_cache[key] = buf
        return buf


def _decode_error(buf: ctypes.Array) -> str:
    try:
        return buf.value.decode("utf-8", errors="replace")
    except Exception:
        return "Unknown native DLSS5 NR error"


def _load_library():
    global _lib
    if _lib is not None:
        return _lib

    if platform.system() != "Windows":
        raise DLSS5NRError("DLSS 5 Neural Rendering node is Windows-only (D3D12/NGX).")
    if not _BRIDGE.exists():
        raise DLSS5NRError(
            f"Native bridge is missing: {_BRIDGE}\n"
                "Normal users: install the prebuilt Windows ZIP from GitHub Releases (not the GitHub Source code ZIP).\n"
                "Developers: rebuild dlss5nr_bridge.dll from the bridge sources kept in this repository's git history (bridge/*.cpp)."
        )

    # Keep AddDllDirectory handles alive for the lifetime of the process.
    # CPython removes a directory when the returned handle is closed/GC'd.
    for dll_dir in (_ROOT / "bridge" / "bin", _RUNTIME, _RUNTIME / "caller"):
        if dll_dir.exists():
            _dll_directory_handles.append(os.add_dll_directory(str(dll_dir)))

    lib = ctypes.WinDLL(str(_BRIDGE))

    lib.dlss5nr_init.argtypes = [
        ctypes.c_int,
        ctypes.c_wchar_p,
        ctypes.c_char_p,
        ctypes.c_int,
    ]
    lib.dlss5nr_init.restype = ctypes.c_int

    lib.dlss5nr_process.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,   # style
        ctypes.c_int,   # preset
        ctypes.c_float, # intensity
        ctypes.c_float, # tone
        ctypes.c_float, # structure
        ctypes.c_float, # skin
        ctypes.c_int,   # automask
        ctypes.c_int,   # reset
        ctypes.c_int,   # temporal (1 = feed NVIDIA Optical Flow motion vectors)
        ctypes.c_char_p,
        ctypes.c_int,
    ]
    lib.dlss5nr_process.restype = ctypes.c_int

    # v0.7.0 CUDA interop path: raw device pointers + caller stream handle.
    lib.dlss5nr_process_cuda.argtypes = [
        ctypes.c_uint64, # src device pointer (torch tensor)
        ctypes.c_uint64, # dst device pointer (torch tensor)
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint64, # caller CUDA stream handle
        ctypes.c_int,   # style
        ctypes.c_int,   # preset
        ctypes.c_float, # intensity
        ctypes.c_float, # tone
        ctypes.c_float, # structure
        ctypes.c_float, # skin
        ctypes.c_int,   # automask
        ctypes.c_int,   # reset
        ctypes.c_char_p,
        ctypes.c_int,
    ]
    lib.dlss5nr_process_cuda.restype = ctypes.c_int

    lib.dlss5nr_cuda_supported.argtypes = []
    lib.dlss5nr_cuda_supported.restype = ctypes.c_int

    lib.dlss5nr_cuda_status.argtypes = [ctypes.c_char_p, ctypes.c_int]
    lib.dlss5nr_cuda_status.restype = ctypes.c_int

    lib.dlss5nr_shutdown.argtypes = []
    lib.dlss5nr_shutdown.restype = None

    lib.dlss5nr_version.argtypes = []
    lib.dlss5nr_version.restype = ctypes.c_char_p

    lib.dlss5nr_gpu_name.argtypes = []
    lib.dlss5nr_gpu_name.restype = ctypes.c_char_p

    # v0.9.0 NVIDIA Optical Flow (driver-supplied nvofapi64.dll). The host uses
    # these to decide whether temporal_mode=optical_flow can be honored and to report
    # the fixed estimator settings in Runtime Info.
    lib.dlss5nr_nvof_available.argtypes = []
    lib.dlss5nr_nvof_available.restype = ctypes.c_int

    lib.dlss5nr_nvof_grid.argtypes = []
    lib.dlss5nr_nvof_grid.restype = ctypes.c_int

    lib.dlss5nr_nvof_perf.argtypes = []
    lib.dlss5nr_nvof_perf.restype = ctypes.c_int

    # v0.9.1: 1 while optical flow is active for this session, 0 once the
    # bridge latched it off after a frame error (the buffer then carries the
    # human-readable reason; "ok" while active).
    lib.dlss5nr_motion_status.argtypes = [ctypes.c_char_p, ctypes.c_int]
    lib.dlss5nr_motion_status.restype = ctypes.c_int

    _lib = lib
    return lib


def _ensure_initialized(gpu_index: int):
    global _initialized_gpu
    lib = _load_library()
    if _initialized_gpu == gpu_index:
        return lib

    if _initialized_gpu is not None:
        lib.dlss5nr_shutdown()
        _initialized_gpu = None

    nr_dll = _RUNTIME / "nvngx_dlssnr.dll"
    shim = _RUNTIME / "caller" / "nvngx.dll_comfy.dll"
    if not shim.exists():
        shim = _RUNTIME / "caller" / "nvngx.dll"  # legacy fallback
    if not nr_dll.exists():
        raise DLSS5NRError(
            f"Missing {nr_dll}. Place your legally obtained nvngx_dlssnr.dll in the runtime folder."
        )
    if not shim.exists():
        raise DLSS5NRError(
            f"Missing caller shim {shim}. Normal users should install the prebuilt Windows release ZIP. "
            "Developers can rebuild it from the bridge sources kept in this repository's git history (bridge/*.cpp)."
        )

    err = ctypes.create_string_buffer(4096)
    ok = lib.dlss5nr_init(gpu_index, str(_RUNTIME), err, len(err))
    if not ok:
        raise DLSS5NRError(_decode_error(err))
    _initialized_gpu = gpu_index
    return lib


def _style_to_int(style: str) -> int:
    named = {"default": 0, "natural": 1, "cinematic": 2}
    if style in named:
        return named[style]
    return int(style)


def _preset_to_int(preset) -> int:
    if isinstance(preset, str):
        try:
            return _NR_PRESETS[preset]
        except KeyError:
            choices = ", ".join(_NR_PRESETS)
            raise DLSS5NRError(f"Unknown preset {preset!r}. Choose one of: {choices}.") from None
    # Old saved workflows store the raw integer 0..3.
    value = int(preset)
    if not 0 <= value <= 3:
        choices = ", ".join(_NR_PRESETS)
        raise DLSS5NRError(f"Unknown preset {preset!r}. Choose one of: {choices}.")
    return value


def _model_preset_to_int(model) -> int:
    try:
        return _DLSS_MODEL_PRESETS[model]
    except (KeyError, TypeError):
        choices = ", ".join(_DLSS_MODEL_PRESETS)
        raise DLSS5NRError(f"Unknown dlss_model_preset {model!r}. Choose one of: {choices}.") from None


def _upscaling_factor(mode: str) -> float:
    try:
        return _UPSCALING_MODES[mode]
    except KeyError:
        choices = ", ".join(_UPSCALING_MODES)
        raise DLSS5NRError(f"Unknown upscaling mode {mode!r}. Choose one of: {choices}.") from None


def _even(value: float) -> int:
    """Round to the nearest even number, minimum 2."""
    return max(2, int(math.floor(value / 2.0 + 0.5)) * 2)


# v0.9.11: watchdog for native bridge calls. A wedged D3D12 session (observed
# after an AV-recovery rebuild: a thread blocked on a GPU fence with zero CPU/GPU
# activity) makes ComfyUI "pretend to work" forever. A blocked ctypes call cannot
# be preempted from Python, so each native evaluate runs in a daemon worker and
# the caller joins with a timeout; on timeout we fail loudly with the only
# correct remedy (restart ComfyUI) instead of hanging silently.
_NATIVE_CALL_TIMEOUT_S = 300


def _native_call_with_watchdog(fn, what):
    box = {}

    def worker():
        try:
            box["result"] = fn()
        except BaseException as exc:  # re-raised in the caller thread below
            box["error"] = exc

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(_NATIVE_CALL_TIMEOUT_S)
    if t.is_alive():
        raise DLSS5NRError(
            f"{what}: the bridge call did not return within "
            f"{_NATIVE_CALL_TIMEOUT_S // 60} min — the native session is wedged "
            "(a thread is blocked waiting on a GPU fence). Stop this run and "
            "RESTART ComfyUI before trying again.")
    if "error" in box:
        raise box["error"]
    return box.get("result")


def _is_native_crash_error(exc) -> bool:
    """True when the failure means the native bridge session in this process is
    corrupted: a caught or escaped access violation, or a call that never
    returned because a thread blocked on a GPU fence. Retrying such a session —
    even via CPU staging — has been observed to wedge (v0.9.11, v0.9.16 field
    runs), so these must fail the run with restart advice instead of falling
    back."""
    msg = str(exc).lower()
    return "access violation" in msg or "fatal exception" in msg or "wedged" in msg


def _scene_reset(j, is_first, temporal_mode, detect_scenes, grays, prev_gray, threshold):
    """Per-frame history-reset decision shared by all processing loops (v0.9.23).

    `j` indexes `grays` locally for the current call; `is_first` marks the very
    first frame of the whole sequence (global index), so a chunked run resets
    exactly like an unchunked one: at frame 0, and on detected scene changes —
    never merely because a chunk boundary was crossed. Returns
    (reset, next_prev_gray).
    """
    if temporal_mode == "none":
        return (1 if is_first else 0), prev_gray
    if not detect_scenes:
        return 1, prev_gray
    gray = grays[j]
    if prev_gray is None:
        reset = 1
    else:
        scene_score = float(np.mean(np.abs(gray - prev_gray)))
        reset = 1 if scene_score > threshold else 0
    return reset, gray


def _scene_grays(rgb, out_w, out_h):
    """Downsampled luma for scene-change detection (long edge <= 256 px), computed
    on the tensor's own device; returns a small CPU float32 numpy array
    [N, gh, gw]. Only these tiny arrays cross to the host for thresholding."""
    scale = min(1.0, 256.0 / max(out_w, out_h))
    gray_w = _even(out_w * scale)
    gray_h = _even(out_h * scale)
    g = 0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]
    if (g.shape[1], g.shape[2]) != (gray_h, gray_w):
        g = torch.nn.functional.interpolate(g[:, None], size=(gray_h, gray_w), mode="area")
    return g.squeeze(1).to(device="cpu", dtype=torch.float32).contiguous().numpy()


def _cuda_blockers(rgb, bridge_gpu, lib, use_of):
    """Reasons the zero-PCIe CUDA interop path cannot run for this batch (v0.9.23:
    shared by the unchunked and chunked paths so both report identically)."""
    blockers = []
    if not rgb.is_cuda:
        blockers.append(
            "input tensor is on CPU (upstream node or ComfyUI "
            "'RAM pressure cache' keeps frames in system memory)")
    elif rgb.device.index != bridge_gpu:
        blockers.append(f"input on GPU {rgb.device.index}, bridge on GPU {bridge_gpu}")
    if use_of:
        blockers.append("optical_flow needs CPU frames (NVOF)")
    if not lib.dlss5nr_cuda_supported():
        # v0.9.12: surface the bridge's own reason (g_cuda_note) instead of a
        # static string — "unavailable" never told us which init step failed
        # (driver missing, missing exports, cuInit error, GPU name mismatch, ...).
        cuda_buf = ctypes.create_string_buffer(1024)
        lib.dlss5nr_cuda_status(cuda_buf, len(cuda_buf))
        note = cuda_buf.value.decode("utf-8", errors="replace") or "unavailable"
        blockers.append(f"CUDA interop unavailable in bridge ({note})")
    return blockers


class DLSS5NeuralRendering:
    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                # No "tooltip" fields on purpose (v0.5.1): ComfyUI's built-in plain tooltip
                # rendered on top of the OreX-style canvas tooltip, so widget descriptions
                # now live only in js/OreX_DLSS5.js (DLSS5_HELP_DESCRIPTIONS), which matches
                # widgets by name and draws the bordered EN/RU box itself.
                # v0.9.22: the preset combo (DLSSNR.Hint.Render.Preset) was
                # removed — all values produce identical output on current
                # builds anyway; process() still takes preset="Default" so old
                # saved workflows keep working (unknown prompt inputs are
                # dropped server-side).
                "upscaling_mode": (list(_UPSCALING_MODES), {
                    # v0.9.8: 1.5x is the verified mode for optical_flow on current
                    # runtime builds (1x native crashes its first frame).
                    "default": "1.5x (Quality)",
                }),
                "dlss_model_preset": (list(_DLSS_MODEL_PRESETS), {
                    "default": "M",
                }),
                "image": ("IMAGE",),
                "style": (["natural", "cinematic", "default"], {
                    "default": "natural",
                }),
                "intensity": ("FLOAT", {
                    "default": 1.0,
                    "min": 0.0,
                    "max": 2.0,
                    "step": 0.05,
                }),
                # v0.9.26: defaults raised to 2.0 (tone/structure/skin).
                "tone": ("FLOAT", {
                    "default": 2.0,
                    "min": 0.0,
                    "max": 2.0,
                    "step": 0.05,
                }),
                "structure": ("FLOAT", {
                    "default": 2.0,
                    "min": 0.0,
                    "max": 2.0,
                    "step": 0.05,
                }),
                "skin": ("FLOAT", {
                    "default": 2.0,
                    "min": -1.0,
                    "max": 2.0,
                    "step": 0.05,
                }),
                "auto_mask": ("BOOLEAN", {
                    "default": False,
                }),
                "temporal_mode": (_TEMPORAL_MODES, {
                    "default": "auto",
                }),
                "scene_change_threshold": ("FLOAT", {
                    "default": 0.24,
                    "min": 0.01,
                    "max": 1.0,
                    "step": 0.01,
                }),
                "warmup_frames": ("INT", {
                    "default": 0,
                    "min": 0,
                    "max": 16,
                    "step": 1,
                }),
                # v0.9.16: real BOOLEAN switch widget (on = GPU ON 🟢, off =
                # GPU OFF 🔴). Old workflows that saved the v0.9.15 strings are
                # normalized in process() via _gpu_on(); values from the
                # gpu_index era do not match this key and fall back to on.
                # v0.9.20: styled exactly like the OreX LM Studio nodes —
                # native widget + label_on/label_off with the 🟢/🔴 icons
                # (no JS overlay over the row).
                "gpu_mode": ("BOOLEAN", {
                    "default": True,
                    "label_on": "🟢 GPU ON",
                    "label_off": "🔴 GPU OFF",
                }),
                # v0.9.23: chunked processing for long sequences. 0 = off (whole
                # batch in one go — the pre-0.9.23 behavior, old workflows are
                # unaffected); >0 = process that many frames per chunk and
                # assemble the result into a single full-batch tensor. Frames
                # stay in order on the same bridge session, so temporal history
                # carries across chunk boundaries exactly as in an unchunked run.
                # Peak RAM/VRAM drops from O(batch) to O(chunk) + the output
                # tensor (which must exist anyway — it is what we return).
                # v0.9.26: default 64 — a sensible chunk for 1080p-class footage
                # (64 × out_h × out_w × 12 bytes ≈ 1–2 GiB at 1.5x); 0 still = off.
                "chunk_frames": ("INT", {
                    "default": 32,
                    "min": 0,
                    "max": 256,
                    "step": 1,
                }),
                # v0.9.27 (reworded v0.9.28): frame index for the Preview
                # button below (0 = first frame). Not used by a full run —
                # after the JS arms /dlss5/preview-arm it queues only the
                # subgraph feeding this node (rgthree-comfy "Queue Selected
                # Output Nodes" pattern), and process() clamps an out-of-range
                # index to the last frame of its own input batch, processing
                # only that one frame during the run. Downstream nodes are not
                # in the pruned prompt, so a preview run writes no outputs.
                "change_frame": ("INT", {
                    "default": 0,
                    "min": 0,
                    "max": 100000,
                    "step": 1,
                }),
                # Widget order is intentional: upscaling_mode on top since
                # v0.9.22 (preset was removed; it sat on top from v0.9.7);
                # batch_mode was replaced by motion in v0.3.0, renamed to
                # temporal_mode in v0.9.5; dlss_model_preset was inserted below
                # upscaling_mode in v0.4.0; change_frame sits last (after
                # chunk_frames) since v0.9.26, with the canvas-drawn Preview/Hide
                # buttons directly under it. ComfyUI matches prompt keys by name,
                # so reordering is safe for saved workflows (values may need a
                # re-check after loading). channel_order was a widget until
                # v0.9.4 (auto/RGBA/BGRA); the node now always runs the
                # auto-detection, so old saved values are simply ignored.
            }
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("image",)
    # v0.9.28: required so the Preview's PRUNED prompt (only this node and
    # its upstream, built by js/OreX_DLSS5.js) passes server validation —
    # ComfyUI only accepts OUTPUT_NODE=True nodes as execution roots, and a
    # pruned subgraph contains no other output node. In a normal full run
    # this is inert: the node's upstream is already pulled in by the real
    # outputs downstream, and it returns no ui data, so nothing extra is
    # sent over the websocket. Bonus: the node becomes selectable in the
    # frontend's "Queue Selected Output Nodes" menu (rgthree-comfy style).

    # v0.9.28: required so the Preview's PRUNED prompt (only this node and
    # its upstream, built by js/OreX_DLSS5.js) passes server validation —
    # ComfyUI only accepts OUTPUT_NODE=True nodes as execution roots, and a
    # pruned subgraph contains no other output node. In a normal full run
    # this is inert: the node's upstream is already pulled in by the real
    # outputs downstream, and it returns no ui data, so nothing extra is
    # sent over the websocket. Bonus: the node becomes selectable in the
    # frontend's "Queue Selected Output Nodes" menu (rgthree-comfy style).
    OUTPUT_NODE = True
    FUNCTION = "process"
    CATEGORY = "experimental/DLSS 5 NR"
    DESCRIPTION = (
        "Unofficial experimental in-process DLSS 5 Neural Rendering (NGX feature 18). "
        "1x enhances at native resolution; larger upscaling modes resample then enhance. "
        "No external executable and no image files are written."
    )

    @classmethod
    def IS_CHANGED(cls, **kwargs):
        import time
        global _preview_arm_ts, _PREVIEW_ARM_TTL
        try:
            armed = bool(_preview_arm_ts and (time.time() - _preview_arm_ts) < _PREVIEW_ARM_TTL)
            if armed:
                return f"preview_{time.time()}"
        except Exception:
            pass
        return "normal_run"


    @classmethod
    def IS_CHANGED(cls, **kwargs):
        import time
        global _preview_arm_ts, _PREVIEW_ARM_TTL
        try:
            armed = bool(_preview_arm_ts and (time.time() - _preview_arm_ts) < _PREVIEW_ARM_TTL)
            if armed:
                return f"preview_{time.time()}"
        except Exception:
            pass
        return "normal_run"

    def process(
        self,
        image,
        upscaling_mode="1.5x (Quality)",
        dlss_model_preset="M",
        style="natural",
        preset="Default",
        intensity=1.0,
        tone=2.0,
        structure=2.0,
        skin=2.0,
        auto_mask=False,
        temporal_mode="auto",
        scene_change_threshold=0.24,
        warmup_frames=0,
        gpu_mode=True,
        chunk_frames=64,
        change_frame=0,  # v0.9.27: frame index used when this run is an armed preview
    ):
        """ComfyUI entry point (v0.9.27). Wraps _process_run with the single-frame
        preview capture: when the Preview button armed this run (see _preview_arm_ts),
        only the change_frame slice of `image` is processed and its PNG is stashed in
        _preview_result for /dlss5/preview-result. Ordinary runs pass through untouched."""
        global _preview_arm_ts, _preview_result, _preview_error
        pv_idx = None
        armed = bool(_preview_arm_ts and (time.time() - _preview_arm_ts) < _PREVIEW_ARM_TTL)
        if armed:
            # Consume the arm immediately — even if the run below fails, a stale arm
            # must not turn some later ordinary run into a preview.
            _preview_arm_ts = 0.0
            _preview_result = None
            _preview_error = None
            if isinstance(image, torch.Tensor) and image.ndim == 4:
                pv_idx = max(0, min(int(change_frame), int(image.shape[0]) - 1))
        try:
            result = self._process_run(
                image, upscaling_mode=upscaling_mode, dlss_model_preset=dlss_model_preset,
                style=style, preset=preset, intensity=intensity, tone=tone, structure=structure,
                skin=skin, auto_mask=auto_mask, temporal_mode=temporal_mode,
                scene_change_threshold=scene_change_threshold, warmup_frames=warmup_frames,
                gpu_mode=gpu_mode, chunk_frames=chunk_frames, change_frame=change_frame,
                _pv_idx=pv_idx)
        except Exception as exc:
            if armed:
                _preview_error = f"{type(exc).__name__}: {exc}"
            raise
        if pv_idx is not None:
            try:
                frame = (result[0][0].detach().to(device="cpu", dtype=torch.float32)
                         .clamp_(0.0, 1.0).contiguous())
                rgb = frame[..., :3].contiguous()
                arr = (rgb.numpy() * 255.0 + 0.5).astype(np.uint8)
                buf = io.BytesIO()
                _PILImage.fromarray(arr).save(buf, format="PNG")
                _preview_result = {"png": buf.getvalue(), "frame": pv_idx}
                print(f"[DLSS5-Ox] preview: frame {pv_idx} rendered — the UI fetches it from /dlss5/preview-result")
            except Exception as exc:
                _preview_error = f"preview encode failed: {exc}"
        return result

    def _process_run(
        self,
        image,
        upscaling_mode="1.5x (Quality)",
        dlss_model_preset="M",
        style="natural",
        preset="Default",
        intensity=1.0,
        tone=2.0,
        structure=2.0,
        skin=2.0,
        auto_mask=False,
        temporal_mode="auto",
        scene_change_threshold=0.24,
        warmup_frames=0,
        gpu_mode=True,
        chunk_frames=64,
        change_frame=0,
        _pv_idx=None,  # v0.9.27: armed preview — process only this frame index
    ):
        if not isinstance(image, torch.Tensor) or image.ndim != 4:
            raise DLSS5NRError("Expected ComfyUI IMAGE tensor [B,H,W,C].")
        if image.shape[-1] < 3:
            raise DLSS5NRError("DLSS5 NR requires at least RGB input.")

        # v0.9.27: armed preview run — process ONLY the selected frame. count
        # becomes 1, so chunking and scene detection are both off below and the
        # temporal history starts fresh at this frame (a fast parameter check,
        # not a slice of a finished video). The process() wrapper captures the
        # PNG; an out-of-range index was already clamped to the last frame there.
        if _pv_idx is not None:
            total_in = int(image.shape[0])
            # The lazy slice optimization is applied upstream in OreX_DLSS5_video for the VIDEO input type.
            # For the IMAGE input type, the upstream LoadImageSequence or LoadVideo has already 
            # returned a fully decoded tensor. We slice the tensor here to process only 1 frame.
            safe_idx = max(0, min(_pv_idx, total_in - 1))
            image = image[safe_idx:safe_idx + 1]
            print(f"[DLSS5-Ox] preview run: processing only frame {safe_idx} of {total_in} "
                  "(temporal history starts fresh at this frame)")

        factor = _upscaling_factor(upscaling_mode)
        style_i = _style_to_int(style)
        preset_i = _preset_to_int(preset)
        # Validated for UI parity with the reference enhancer; reserved on this
        # runtime path, see _DLSS_MODEL_PRESETS.
        _model_preset_to_int(dlss_model_preset)
        threshold = float(scene_change_threshold)
        if not 0.01 <= threshold <= 1.0:
            raise DLSS5NRError("scene_change_threshold must be between 0.01 and 1.0.")
        warmup = max(0, min(16, int(warmup_frames)))
        # v0.9.15: optical_flow is no longer offered; old workflows may still
        # carry it — clamp to auto so they keep working instead of re-enabling
        # the crashing NVOF path.
        if temporal_mode not in _TEMPORAL_MODES:
            print(f"[DLSS5-Ox] temporal_mode={temporal_mode!r} is no longer available (v0.9.15); using auto")
            temporal_mode = "auto"

        count, in_h, in_w, _c = image.shape
        # v0.9.23: chunked processing for long sequences. 0 keeps the legacy
        # whole-batch behavior bit-for-bit; a chunk >= count is a single chunk,
        # which is the unchunked path anyway, so only 0 < chunk < count chunks.
        chunk = max(0, min(256, int(chunk_frames)))
        use_chunks = chunk > 0 and count > 1 and chunk < count
        if factor == 1.0:
            out_w, out_h = in_w, in_h
        else:
            out_w, out_h = _even(in_w * factor), _even(in_h * factor)
        if max(out_w, out_h) > _MAX_LONG_EDGE or min(out_w, out_h) > _MAX_SHORT_EDGE:
            raise DLSS5NRError(
                f"Output {out_w}x{out_h} exceeds the supported "
                f"{_MAX_LONG_EDGE}x{_MAX_SHORT_EDGE} boundary. Pick a lower upscaling mode for this source."
            )

        # v0.9.13: GPU ON 🟢 also moves a CPU (or other-GPU) input batch into the
        # first GPU's VRAM before processing — ComfyUI's RAM-pressure cache and video
        # loaders keep long batches in system memory, and while frames sit in RAM the
        # zero-copy path is impossible. GPU OFF 🔴 opts out of the move (frames stay
        # put). Both modes run the bridge on GPU 0; v0.9.15 replaced the gpu_index INT
        # with this toggle — a real BOOLEAN switch since v0.9.16. The move
        # is refused when batch + output would not fit the free VRAM: peak usage holds
        # both the input-res and the output-res batch at once (resampling allocates the
        # second one, and the final upload / zero-copy output needs it too), so a blind
        # .to() would OOM long sequences.
        gpu = 0 if _gpu_on(gpu_mode) else -1
        bridge_gpu = max(0, gpu)
        if gpu >= 0:
            device = torch.device(f"cuda:{gpu}")
            if image.device != device and gpu < torch.cuda.device_count():
                needed_in = image.element_size() * image.numel()
                needed_out = count * out_h * out_w * 3 * 4
                try:
                    free, _total = torch.cuda.mem_get_info(device)
                except Exception:
                    free = None
                if free is not None and needed_in + needed_out + (2 << 30) > free:
                    if use_chunks:
                        # v0.9.23: the whole batch no longer needs to fit — it is
                        # processed in chunks, so staying on the current device is a
                        # feature here, not a failure.
                        print(f"[DLSS5-Ox] keeping frames on {image.device}: whole batch + output need "
                              f"{(needed_in + needed_out) / 2**30:.1f} GiB but only {free / 2**30:.1f} GiB "
                              f"is free on {device} — processing in chunks of {chunk} frames instead")
                    else:
                        print(f"[DLSS5-Ox] keeping frames on {image.device}: batch + output need "
                              f"{(needed_in + needed_out) / 2**30:.1f} GiB but only {free / 2**30:.1f} GiB "
                              f"is free on {device} — split the sequence into shorter runs or use a GPU with more VRAM")
                else:
                    print(f"[DLSS5-Ox] moving {count} frames ({needed_in / 2**30:.1f} GiB) from "
                          f"{image.device} to {device}")
                    image = image.to(device)
            elif gpu >= torch.cuda.device_count():
                print(f"[DLSS5-Ox] GPU ON but CUDA sees {torch.cuda.device_count()} device(s); "
                      f"keeping frames on {image.device}")

        # CPU staging around the native bridge is kept (v0.1 design): the bridge
        # consumes plain float32 RGB arrays. Since v0.6.0 everything that used to
        # run on full-resolution host copies happens on the source device first:
        # >1x resampling and scene-change luma are computed there before the
        # single batch download, and both batch copies use pinned staging.
        # .contiguous() is required for the v0.7.0 CUDA path (raw pointers);
        # it is a no-op when the slice already is contiguous.
        if use_chunks:
            # v0.9.23: keep the input as a plain view of `image` (original dtype);
            # each chunk is converted and resampled separately in _run_chunked, so
            # peak memory is one chunk instead of the whole batch.
            rgb = image[..., :3].detach()
        else:
            rgb = image[..., :3].detach().to(dtype=torch.float32).contiguous()

        if not use_chunks and (out_w, out_h) != (in_w, in_h):
            # This bridge has no DLSS Super Resolution models: larger modes
            # resample first (bicubic with antialiasing), then the neural pass
            # runs at the target size.
            t = rgb.permute(0, 3, 1, 2)
            t = torch.nn.functional.interpolate(
                t, size=(out_h, out_w), mode="bicubic", align_corners=False, antialias=True)
            rgb = t.permute(0, 2, 3, 1).clamp_(0.0, 1.0).contiguous()

        # Scene-change detection state for temporal_mode auto/optical_flow sequences.
        detect_scenes = temporal_mode in ("auto", "optical_flow") and count > 1
        grays = None
        if detect_scenes and not use_chunks:
            # v0.9.23: chunked runs compute the tiny luma arrays per chunk inside
            # _run_chunked (the whole-batch array would defeat the point).
            grays = _scene_grays(rgb, out_w, out_h)

        with _lock:
            lib = _ensure_initialized(bridge_gpu)

            # v0.9.0 real optical flow: temporal_mode=optical_flow on a sequence feeds
            # genuine NVIDIA Optical Flow vectors into DLSSNR.MVec. The NVOF
            # estimator consumes a CPU RGB pointer, so this mode always runs on
            # the CPU staging path (the CUDA zero-copy path carries no MV). If
            # the driver does not ship nvofapi64.dll we fall back to auto
            # behavior instead of failing the whole run.
            use_of = temporal_mode == "optical_flow" and count > 1
            if use_of and not lib.dlss5nr_nvof_available():
                print("[DLSS5-Ox] NVIDIA Optical Flow (nvofapi64.dll) was not found in the "
                      "display driver; falling back to temporal_mode=auto behavior for this run.")
                use_of = False

            # v0.7.0 CUDA interop path (zero PCIe pixel traffic): available only
            # when the input already sits on the bridge's own GPU and the bridge
            # reports live interop. A non-native runtime failure there disables
            # CUDA in the bridge for this process; we then retry the whole batch
            # on the CPU staging path below with fresh history (NGX temporal
            # state is unknown after a failed frame, so force resets). v0.9.17:
            # a native crash (access violation) does NOT fall back — the session
            # in this process is corrupted and any retry wedges on the rebuild,
            # so it fails the run with restart advice instead.
            # v0.9.9: the chosen path is printed every run. Previously a gate
            # miss or a runtime fallback was completely silent, so all three
            # temporal modes looked identical in RAM/VRAM usage and it was
            # impossible to tell from the UI which path actually ran.
            # v0.9.23: chunked path — same per-frame core as the unchunked runs,
            # but the full output is pre-allocated once and filled chunk by chunk,
            # so peak RAM/VRAM is O(chunk) + that tensor instead of input batch +
            # output batch at once. The bridge session persists across chunks and
            # frames keep their order, so temporal history (and scene-detect state)
            # carries across boundaries exactly as in an unchunked run.
            if use_chunks:
                # v0.9.25: the CUDA/CPU decision moved into _run_chunked and is made
                # PER CHUNK — a chunk that fits the free VRAM is moved to the bridge
                # GPU and runs zero-copy even when the whole batch does not fit (the
                # case that used to force CPU staging for the entire run). Peak VRAM
                # stays O(chunk): the input chunk + its temp output, copied back D2H.
                # gpu_device=None (GPU OFF) keeps every chunk on CPU staging.
                gpu_device = torch.device(f"cuda:{bridge_gpu}") if gpu >= 0 else None
                try:
                    return self._run_chunked(
                        lib, image, rgb, count, in_w, in_h, out_w, out_h, style_i, preset_i,
                        intensity, tone, structure, skin, auto_mask, temporal_mode,
                        threshold, warmup, detect_scenes, chunk, use_of, gpu_device)
                except (DLSS5NRError, OSError) as exc:
                    if _is_native_crash_error(exc):
                        print(f"[DLSS5-Ox] native crash ({exc}). The bridge session in this "
                              "process is corrupted — RESTART ComfyUI before the next run; a retry "
                              "(even on another path) would wedge on the session rebuild.")
                    raise

            cuda_blockers = _cuda_blockers(rgb, bridge_gpu, lib, use_of)

            if not cuda_blockers:
                try:
                    print("[DLSS5-Ox] path: CUDA zero-copy (pixels stay in VRAM, no PCIe traffic)")
                    return self._run_cuda_batch(
                        lib, image, rgb, count, out_w, out_h, style_i, preset_i,
                        intensity, tone, structure, skin, auto_mask, temporal_mode,
                        threshold, warmup, detect_scenes, grays)
                # v0.9.16: catch OSError as well — CPython converts a native
                # access violation in the ctypes call into "Windows fatal
                # exception: access violation" + OSError, and the watchdog
                # re-raises it verbatim. The bridge's SEH trampoline contains
                # such AVs (stage-tagged error -> DLSS5NRError).
                # v0.9.17: after a native crash the session in this process is
                # corrupted and the in-process rebuild that used to follow has
                # been observed twice to wedge — so an AV (or a wedged call)
                # fails the run with restart advice instead of falling back to
                # CPU staging, where it would just wedge again. Non-native CUDA
                # failures (stream mismatch, size change, ...) leave the session
                # healthy and still fall back.
                except (DLSS5NRError, OSError) as exc:
                    if _is_native_crash_error(exc):
                        print(f"[DLSS5-Ox] path: CUDA zero-copy hit a native crash ({exc}). "
                              "The bridge session in this process is corrupted — RESTART ComfyUI "
                              "before the next run (a CPU staging retry would wedge on the rebuild).")
                        raise
                    print(f"[DLSS5-Ox] path: CUDA zero-copy failed ({exc}); "
                          "retrying this batch on CPU staging")
                    detect_scenes = False
            else:
                print("[DLSS5-Ox] path: CPU staging — " + "; ".join(cuda_blockers))

            # --- CPU staging path (v0.1 design) ---
            try:
                return self._run_cpu_staging(
                    lib, rgb, image, count, out_w, out_h, style_i, preset_i,
                    intensity, tone, structure, skin, auto_mask, temporal_mode,
                    threshold, warmup, detect_scenes, grays, use_of)
            except DLSS5NRError as exc:
                # v0.9.11: a native crash leaves the bridge session corrupted,
                # and the in-process rebuild that used to follow has been
                # observed to wedge (blocked on a GPU fence, zero CPU/GPU load) —
                # confirmed again by the v0.9.16 field run after a CUDA-path AV.
                # Fail loudly with restart advice; the next run in this process
                # would trigger the same unreliable rebuild on its first frame.
                if _is_native_crash_error(exc):
                    print(f"[DLSS5-Ox] native crash ({exc}). The bridge session in this "
                          "process is corrupted — RESTART ComfyUI before the next run; a retry "
                          "(even on another path) would wedge on the session rebuild.")
                raise

    def _run_cpu_staging(
        self, lib, rgb, image, count, out_w, out_h, style_i, preset_i,
        intensity, tone, structure, skin, auto_mask, temporal_mode, threshold,
        warmup, detect_scenes, grays, use_of, start=0, fill_out=None,
        scene_state=None, total=None,
    ):
        """CPU staging path (v0.1 design), extracted in v0.9.10 so the caller can
        retry the whole batch with auto behavior after a native optical_flow crash.

        Called with _lock held and the bridge initialized. Page-locked staging for
        the batch download/upload (see _pinned_buffer); `dst` deliberately aliases
        the same buffer: input and output share one shape, and frame i is fully
        consumed before slot i gets overwritten.

        v0.9.23 chunk support: `start`/`total` re-base the frame numbering for a
        sub-range of the sequence (global index i = start + j), and `fill_out`
        writes each result into a caller-allocated tensor sized to this call's
        range (indexed locally by j) instead of assembling one here — that is how
        _run_chunked keeps peak memory at O(chunk) + the output tensor.
        scene_state carries prev_gray across calls, so a chunked run resets
        exactly like an unchunked one.
        """
        # Chunked mode always stages through pinned memory (even for CPU input):
        # the bridge's internal H2D/D2H copies then run at full DMA bandwidth.
        stage = (_pinned_buffer((count, out_h, out_w, 3))
                 if (fill_out is not None or rgb.is_cuda) else None)
        if stage is not None:
            stage.copy_(rgb)
            src = stage.numpy()
        elif rgb.is_cuda:
            src = rgb.to(device="cpu").numpy()
        else:
            src = rgb.contiguous().numpy()

        dst = None if fill_out is not None else (
            src if stage is not None else np.empty((count, out_h, out_w, 3), dtype=np.float32))

        state = scene_state if scene_state is not None else {"prev": None}
        prev_gray = state["prev"]
        total_n = total if total is not None else count
        for j in range(count):
            i = start + j
            # v0.9.11: heartbeat so a future stall can be localized to a frame
            # instead of looking like a dead node (ComfyUI's progress bar is
            # per-node, not per-frame).
            if total_n >= 64 and i % 64 == 0:
                print(f"[DLSS5-Ox] staging: frame {i}/{total_n}")
            frame_in = np.ascontiguousarray(src[j], dtype=np.float32)
            fh, fw, _ = frame_in.shape

            reset, prev_gray = _scene_reset(
                j, i == 0, temporal_mode, detect_scenes, grays, prev_gray, threshold)
            state["prev"] = prev_gray

            # After a history reset the temporal state is cold; extra
            # evaluations let accumulation settle before we keep this frame.
            # Only the first evaluation carries reset=1, the rest build on it.
            evaluations = 1 + (warmup if reset else 0)
            for eval_i in range(evaluations):
                frame_out = np.empty_like(frame_in, dtype=np.float32)
                err = ctypes.create_string_buffer(4096)

                def _call(_err=err):
                    return lib.dlss5nr_process(
                        frame_in.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                        frame_out.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                        int(fw), int(fh),
                        int(style_i), int(preset_i),
                        ctypes.c_float(float(intensity)),
                        ctypes.c_float(float(tone)),
                        ctypes.c_float(float(structure)),
                        ctypes.c_float(float(skin)),
                        1 if auto_mask else 0,
                        int(reset) if eval_i == 0 else 0,
                        1 if use_of else 0,
                        _err, len(_err),
                    )

                ok = _native_call_with_watchdog(_call, f"Frame {i}")
                if not ok:
                    raise DLSS5NRError(f"Frame {i}: {_decode_error(err)}")

            # The leaked stock/reference runtime has been observed to expose
            # BGRA-like channel order, while some Ada-patched builds expose
            # ordinary RGBA. The bridge deliberately returns raw RGB slots;
            # DLSS NR may relight/reconstruct the image, but it should not
            # globally swap red and blue. Pick the interpretation whose
            # low-frequency colour statistics are closer to the source —
            # combining per-pixel MAE and mean-colour distance makes this
            # robust without assuming a particular DLL build (the former
            # channel_order widget was always "auto" since v0.9.4).
            raw = frame_out
            swapped = frame_out[..., [2, 1, 0]]
            # Downsample by striding for cheap comparison on large images.
            step_y = max(1, fh // 128)
            step_x = max(1, fw // 128)
            ref_s = frame_in[::step_y, ::step_x]
            raw_s = raw[::step_y, ::step_x]
            swp_s = swapped[::step_y, ::step_x]
            raw_score = float(np.mean(np.abs(raw_s - ref_s))) + float(np.mean(np.abs(raw_s.mean(axis=(0,1)) - ref_s.mean(axis=(0,1)))))
            swp_score = float(np.mean(np.abs(swp_s - ref_s))) + float(np.mean(np.abs(swp_s.mean(axis=(0,1)) - ref_s.mean(axis=(0,1)))))
            corrected = raw if raw_score <= swp_score else swapped

            if dst is not None:
                dst[j] = corrected
            else:
                # v0.9.24: PyTorch __setitem__ does NOT convert numpy values
                # ("can't assign a numpy.ndarray to a torch.FloatTensor") — go
                # through a zero-copy view + device-correct copy instead.
                # v0.9.25: fill_out is sized to this call's range, so index it
                # locally by j (the caller passes the matching slice).
                fill_out[j].copy_(torch.from_numpy(corrected))

        if fill_out is not None:
            # Chunked mode: the caller assembles and returns the full tensor; the
            # motion-status note and completion print happen once there.
            return

        # v0.9.1: the bridge may have latched optical flow off mid-run when this
        # runtime rejected the MVec contract (the video then completed in auto
        # mode). Surface that exactly once per reason.
        if use_of:
            motion_buf = ctypes.create_string_buffer(1024)
            if not lib.dlss5nr_motion_status(motion_buf, len(motion_buf)):
                note = motion_buf.value.decode("utf-8", errors="replace") or "disabled"
                if note not in _motion_warnings:
                    _motion_warnings.add(note)
                    print(f"[DLSS5-Ox] {note}")

        print(f"[DLSS5-Ox] staging: all {total_n} frames done, returning result to ComfyUI")

        # Return to the same device/dtype convention ComfyUI supplied. When the
        # pinned staging buffer was used, the upload is a full-bandwidth DMA from
        # it; otherwise fall back to the plain pageable path.
        if stage is not None:
            result = torch.empty_like(stage, device=image.device)
            result.copy_(stage)
            result = result.to(dtype=image.dtype)
        else:
            result = torch.from_numpy(dst).to(device=image.device, dtype=image.dtype)
        return (result.clamp_(0.0, 1.0),)

    def _run_cuda_batch(
        self, lib, image, rgb, count, out_w, out_h, style_i, preset_i,
        intensity, tone, structure, skin, auto_mask, temporal_mode, threshold,
        warmup, detect_scenes, grays, start=0, fill_out=None, scene_state=None,
        total=None,
    ):
        """v0.7.0 zero-PCIe CUDA interop path.

        Called with _lock held and the bridge initialized. The bridge maps its
        D3D12 frame textures into this process's CUDA context, converts
        RGB(f32) <-> RGBA16F with embedded PTX kernels on the caller's stream,
        runs NGX inference in between, and writes each frame directly into a
        slot of the output tensor. Pixels never cross PCIe; only the tiny
        scene-detection luma arrays (already computed by the caller) live on
        the host.

        v0.9.23 chunk support: `start`/`total` re-base the frame numbering for a
        sub-range, and `fill_out` writes into a caller-allocated tensor sized to
        this call's range (indexed locally by j; same contract as
        _run_cpu_staging); scene_state carries prev_gray across chunks so resets
        match an unchunked run.
        """
        device = rgb.device
        stream_handle = torch.cuda.current_stream(device).cuda_stream
        external = fill_out is not None
        out = (fill_out if external
               else torch.empty((count, out_h, out_w, 3), dtype=torch.float32, device=device))

        state = scene_state if scene_state is not None else {"prev": None}
        prev_gray = state["prev"]
        total_n = total if total is not None else count
        for j in range(count):
            i = start + j
            if total_n >= 64 and i % 64 == 0:
                print(f"[DLSS5-Ox] cuda zero-copy: frame {i}/{total_n}")
            frame_in = rgb[j]  # contiguous [H, W, 3]; the chunk was made contiguous
            fh, fw, _ = frame_in.shape

            reset, prev_gray = _scene_reset(
                j, i == 0, temporal_mode, detect_scenes, grays, prev_gray, threshold)
            state["prev"] = prev_gray

            # After a history reset the temporal state is cold; extra
            # evaluations let accumulation settle before we keep this frame.
            # Only the first evaluation carries reset=1, the rest build on it.
            evaluations = 1 + (warmup if reset else 0)
            for eval_i in range(evaluations):
                err = ctypes.create_string_buffer(4096)

                def _call(_err=err):
                    return lib.dlss5nr_process_cuda(
                        frame_in.data_ptr(), out[j].data_ptr(), int(fw), int(fh),
                        stream_handle,
                        int(style_i), int(preset_i),
                        float(intensity), float(tone), float(structure), float(skin),
                        1 if auto_mask else 0,
                        int(reset) if eval_i == 0 else 0,
                        _err, len(_err),
                    )

                ok = _native_call_with_watchdog(_call, f"Frame {i} (CUDA path)")
                if not ok:
                    raise DLSS5NRError(f"Frame {i} (CUDA path): {_decode_error(err)}")

            # Channel-order interpretation, same policy as the CPU path but
            # computed fully on-GPU; every op below is queued on the caller's
            # stream and therefore ordered after the bridge's conversion
            # kernel, so no synchronization is needed until the final clamp.
            raw = out[j]
            swapped = raw[..., [2, 1, 0]]
            # Downsample by striding for cheap comparison on large images.
            step_y = max(1, fh // 128)
            step_x = max(1, fw // 128)
            ref_s = frame_in[::step_y, ::step_x]
            raw_s = raw[::step_y, ::step_x]
            swp_s = swapped[::step_y, ::step_x]
            raw_score = (float((raw_s - ref_s).abs().mean())
                         + float((raw_s.mean(dim=(0, 1)) - ref_s.mean(dim=(0, 1))).abs().mean()))
            swp_score = (float((swp_s - ref_s).abs().mean())
                         + float((swp_s.mean(dim=(0, 1)) - ref_s.mean(dim=(0, 1))).abs().mean()))
            if swp_score < raw_score:
                out[j] = swapped

        if external:
            # Chunked mode: the caller converts dtype and clamps the full tensor.
            return
        result = out.to(dtype=image.dtype)
        return (result.clamp_(0.0, 1.0),)

    def _run_chunked(
        self, lib, image, rgb, count, in_w, in_h, out_w, out_h, style_i, preset_i,
        intensity, tone, structure, skin, auto_mask, temporal_mode, threshold,
        warmup, detect_scenes, chunk, use_of, gpu_device=None,
    ):
        """v0.9.23: long-sequence path for chunk_frames > 0.

        Called with _lock held and the bridge initialized. The full output is
        pre-allocated once on the input device and filled chunk by chunk, so peak
        RAM/VRAM is O(chunk) + that tensor instead of input batch + output batch
        at once. Frames stay in order on the same bridge session, so temporal
        history carries across chunk boundaries exactly as in an unchunked run
        (scene_state threads prev_gray; no boundary resets), and the result
        matches an unchunked run. Per chunk: convert dtype + resample only that
        slice, compute its tiny luma array, then run the existing per-frame core.

        v0.9.25: the path is chosen PER CHUNK, not once for the whole run. With
        GPU ON a CPU chunk that fits the free VRAM (input + temp output + 1 GiB)
        is moved to the bridge GPU and runs zero-copy; its result is copied back
        into out_full right after, so peak VRAM stays O(chunk) even when the
        whole batch does not fit. A chunk that does not fit — or any chunk after
        a non-native CUDA failure, which disables interop for the process — runs
        on pinned CPU staging; the failed chunk is retried there with fresh
        history (per-frame resets), exactly like the unchunked retry, and earlier
        chunks keep their results.
        """
        resample = (out_w, out_h) != (in_w, in_h)
        out_full = torch.empty(
            (count, out_h, out_w, 3), dtype=torch.float32, device=image.device)
        scene_state = {"prev": None}
        # v0.9.25: interop gate for the whole run; a non-native CUDA failure
        # inside a chunk latches it off (the bridge disables CUDA in-process).
        cuda_enabled = (gpu_device is not None and not use_of
                        and lib.dlss5nr_cuda_supported())
        if not cuda_enabled:
            if gpu_device is None:
                cuda_off_reason = "GPU OFF"
            elif use_of:
                cuda_off_reason = "optical_flow needs CPU frames (NVOF)"
            else:
                cuda_buf = ctypes.create_string_buffer(1024)
                lib.dlss5nr_cuda_status(cuda_buf, len(cuda_buf))
                note = cuda_buf.value.decode("utf-8", errors="replace") or "unavailable"
                cuda_off_reason = f"CUDA interop unavailable in bridge ({note})"
        else:
            cuda_off_reason = None
        logged_path = None
        used_cuda = False
        for s in range(0, count, chunk):
            e = min(s + chunk, count)
            n_c = e - s
            print(f"[DLSS5-Ox] chunk {s + 1}-{e}/{count}")
            c_in = rgb[s:e].detach().to(dtype=torch.float32).contiguous()
            if resample:
                t = c_in.permute(0, 3, 1, 2)
                t = torch.nn.functional.interpolate(
                    t, size=(out_h, out_w), mode="bicubic", align_corners=False, antialias=True)
                c_rgb = t.permute(0, 2, 3, 1).clamp_(0.0, 1.0).contiguous()
            else:
                c_rgb = c_in
            grays_c = _scene_grays(c_rgb, out_w, out_h) if detect_scenes else None

            # v0.9.25: per-chunk path decision. A chunk already on the bridge GPU
            # (whole batch fit and was moved there) runs zero-copy straight into
            # out_full; a CPU chunk is moved first when it fits, with its result
            # copied back D2H afterwards.
            path = "cpu"
            if cuda_enabled:
                if c_rgb.device == gpu_device:
                    path = "cuda"
                else:
                    needed = 2 * n_c * out_h * out_w * 3 * 4 + (1 << 30)
                    try:
                        free, _total = torch.cuda.mem_get_info(gpu_device)
                    except Exception:
                        free = None
                    if free is not None and needed <= free:
                        c_rgb = c_rgb.to(gpu_device)
                        path = "cuda"
                    else:
                        why = (f"needs {needed / 2**30:.1f} GiB but only "
                               f"{free / 2**30:.1f} GiB is free on {gpu_device}"
                               if free is not None else f"VRAM query failed on {gpu_device}")
                        print(f"[DLSS5-Ox] chunk {s + 1}-{e}/{count}: CPU staging — {why}")

            if path != logged_path:
                if path == "cuda":
                    used_cuda = True
                    how = ("input already on the bridge GPU"
                           if image.device == gpu_device
                           else "chunks moved to the bridge GPU as they fit")
                    print(f"[DLSS5-Ox] path: CUDA zero-copy, chunked ({chunk} frames/chunk; {how})")
                else:
                    why = "" if used_cuda else (f" — {cuda_off_reason}" if cuda_off_reason else "")
                    print(f"[DLSS5-Ox] path: CPU staging (chunked){why}")
                logged_path = path

            if path == "cuda":
                try:
                    if out_full.device == gpu_device:
                        self._run_cuda_batch(
                            lib, image, c_rgb, n_c, out_w, out_h, style_i, preset_i,
                            intensity, tone, structure, skin, auto_mask, temporal_mode,
                            threshold, warmup, detect_scenes, grays_c, start=s,
                            fill_out=out_full[s:e], scene_state=scene_state, total=count)
                    else:
                        out_c = torch.empty(
                            (n_c, out_h, out_w, 3), dtype=torch.float32, device=gpu_device)
                        self._run_cuda_batch(
                            lib, image, c_rgb, n_c, out_w, out_h, style_i, preset_i,
                            intensity, tone, structure, skin, auto_mask, temporal_mode,
                            threshold, warmup, detect_scenes, grays_c, start=s,
                            fill_out=out_c, scene_state=scene_state, total=count)
                        out_full[s:e].copy_(out_c)
                        del out_c
                except (DLSS5NRError, OSError) as exc:
                    if _is_native_crash_error(exc):
                        raise
                    # Non-native CUDA failure: interop is disabled in the bridge
                    # for this process — latch off, retry THIS chunk on CPU
                    # staging with fresh history (per-frame resets), like the
                    # unchunked retry. Earlier chunks keep their results; the
                    # forced-reset chunk never updates prev_gray, so restore it
                    # from this chunk's luma to keep scene detection continuous.
                    cuda_enabled = False
                    logged_path = None
                    print(f"[DLSS5-Ox] path: CUDA zero-copy failed ({exc}); "
                          f"retrying chunk {s + 1}-{e}/{count} on CPU staging, "
                          "the rest of the run continues on CPU staging")
                    self._run_cpu_staging(
                        lib, c_rgb, image, n_c, out_w, out_h, style_i, preset_i,
                        intensity, tone, structure, skin, auto_mask, temporal_mode,
                        threshold, warmup, False, grays_c, use_of, start=s,
                        fill_out=out_full[s:e], scene_state=scene_state, total=count)
                    if detect_scenes and grays_c is not None:
                        scene_state["prev"] = grays_c[-1]
            else:
                self._run_cpu_staging(
                    lib, c_rgb, image, n_c, out_w, out_h, style_i, preset_i,
                    intensity, tone, structure, skin, auto_mask, temporal_mode,
                    threshold, warmup, detect_scenes, grays_c, use_of, start=s,
                    fill_out=out_full[s:e], scene_state=scene_state, total=count)
            del c_rgb, c_in
        print(f"[DLSS5-Ox] chunked: all {count} frames done in {(count + chunk - 1) // chunk} chunks")
        result = out_full.to(dtype=image.dtype)
        return (result.clamp_(0.0, 1.0),)


# Регистрация узлов (на случай, если она происходила в этом же файле)
NODE_CLASS_MAPPINGS = {
    "DLSS5NeuralRendering": DLSS5NeuralRendering
}

NODE_DISPLAY_NAME_MAPPINGS = {
    "DLSS5NeuralRendering": "DLSS 5 (OreX)"
}