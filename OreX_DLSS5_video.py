# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ComfyUI-DLSS5-Ox contributors

"""🕹️DLSS 5 Video (OreX) — video-stream variant of the DLSS 5 Neural
Rendering node.

A visual copy of the image-based 🕹️DLSS 5 (OreX) node
(OreX_DLSS5.py / js/OreX_DLSS5.js): the same widgets, the same OreX-style
EN/RU tooltips and the same canvas-drawn Preview/Hide controls. The only
difference is the wire type — it takes a ComfyUI VIDEO (a VideoInput object)
on the input and emits a ComfyUI VIDEO on the output instead of an IMAGE
tensor batch.

The per-frame neural pipeline is NOT re-implemented here. For a normal run,
process() decodes a bounded frame chunk, delegates it to the unchanged
DLSS5NeuralRendering pipeline, writes the completed part to disk and finally
concatenates the parts into a file-backed VideoInput. This keeps both input and
output memory bounded for long videos and permits resume after interruption.
"""

import gc
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import urllib.error
from fractions import Fraction
from pathlib import Path

import folder_paths
import torch
import comfy.model_management
import comfy.utils

from .OreX_DLSS5 import (
    DLSS5NeuralRendering,
    _UPSCALING_MODES,
    _DLSS_MODEL_PRESETS,
    _TEMPORAL_MODES,
)

def _ensure_dll_exists():
    current_dir = os.path.dirname(os.path.abspath(__file__))
    runtime_dir = os.path.join(current_dir, "runtime")
    dll_path = os.path.join(runtime_dir, "nvngx_dlssnr.dll")
    tmp_path = dll_path + ".tmp"
    
    # Если итоговый файл уже есть, ничего не делаем
    if os.path.exists(dll_path):
        return
        
    os.makedirs(runtime_dir, exist_ok=True)
    dll_url = "https://github.com/orex2121/ComfyUI-DLSS5-orex/releases/download/nvngx-v1/nvngx_dlssnr.dll"
    
    # 1. Проверяем наличие временного файла для докачки
    existing_size = 0
    if os.path.exists(tmp_path):
        existing_size = os.path.getsize(tmp_path)
        
    headers = {'User-Agent': 'Mozilla/5.0'}
    
    # 2. Если файл уже частично скачан, запрашиваем сервер отдать остаток
    if existing_size > 0:
        headers['Range'] = f"bytes={existing_size}-"
        print(f"[DLSS5-Ox] Resuming download of nvngx_dlssnr.dll from {existing_size / (1024*1024):.1f} MB...")
    else:
        print(f"[DLSS5-Ox] Missing nvngx_dlssnr.dll. Starting automatic download...")
        
    print(f"[DLSS5-Ox] URL: {dll_url}")
    
    try:
        req = urllib.request.Request(dll_url, headers=headers)
        with urllib.request.urlopen(req, timeout=15) as response:
            
            # Код 206 означает, что сервер понял Range и отдает часть файла
            is_partial = response.getcode() == 206
            mode = 'ab' if is_partial else 'wb'
            
            if not is_partial:
                existing_size = 0  # Сервер не поддерживает докачку, пишем с нуля
                
            total_size = int(response.info().get('Content-Length', 0)) + existing_size
            downloaded = existing_size
            chunk_size = 1024 * 1024  
            
            with open(tmp_path, mode) as out_file:
                while True:
                    chunk = response.read(chunk_size)
                    if not chunk:
                        break
                    
                    out_file.write(chunk)
                    downloaded += len(chunk)
                    
                    if total_size > 0:
                        percent = min(100, int(downloaded * 100 / total_size))
                        sys.stdout.write(f"\r[DLSS5-Ox] Downloading... {percent}% ({downloaded / (1024*1024):.1f} MB / {total_size / (1024*1024):.1f} MB)")
                        sys.stdout.flush()
                        
        if total_size > 0 and downloaded < total_size:
            raise Exception(f"Download incomplete: got {downloaded} out of {total_size} bytes")
            
        print("\n[DLSS5-Ox] Download complete! Proceeding with initialization.")
        
        # 3. Безопасное переименование после 100% скачивания
        if os.path.exists(dll_path):
            os.remove(dll_path)
        os.rename(tmp_path, dll_path)
        
    except Exception as e:
        sys.stdout.write("\n")
        sys.stdout.flush()
        print(f"[ERROR] [DLSS5-Ox] Failed to download nvngx_dlssnr.dll: {e}")
        
        # 4. ВАЖНО: Мы больше НЕ удаляем tmp_path здесь. 
        # Прогресс сохраняется на диске для следующей попытки.
        raise RuntimeError(
            f"Missing {dll_path} and failed to download it automatically.\n"
            f"Error: {e}\n"
            f"Please check your internet connection or proxy settings, or manually download the file from {dll_url} and place it in the runtime folder."
        )


# ComfyUI's native VIDEO wire type carries a VideoInput object (lazy,
# file-backed or in-memory). Import the builders defensively so the node
# degrades gracefully when the comfy_api layer is unavailable.[cite: 2]
def _import_video_classes():
    found = {}
    try:
        from comfy_api.latest import VideoFromComponents
        found["VideoFromComponents"] = VideoFromComponents
    except Exception:
        pass
    try:
        from comfy_api.latest import VideoComponents
        found["VideoComponents"] = VideoComponents
    except Exception:
        pass
    try:
        from comfy_api.latest import VideoFromFile
        found["VideoFromFile"] = VideoFromFile
    except Exception:
        pass
    return found

_VCLASSES = _import_video_classes()
VideoFromComponents = _VCLASSES.get("VideoFromComponents")
VideoComponents = _VCLASSES.get("VideoComponents")
VideoFromFile = _VCLASSES.get("VideoFromFile")
_HAS_VIDEO_BUILDERS = VideoFromComponents is not None and VideoComponents is not None


def _source_path(video):
    if hasattr(video, "get_stream_source"):
        source = video.get_stream_source()
        if isinstance(source, (str, os.PathLike)) and Path(source).is_file():
            return Path(source).resolve()
    return None


def _write_manifest(path, manifest):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


def _clear_directory(path, keep=None):
    if not path.is_dir():
        return
    for child in path.iterdir():
        if keep is not None and child == keep:
            continue
        if child.is_dir():
            shutil.rmtree(child, ignore_errors=True)
        else:
            child.unlink(missing_ok=True)


def _resume_config(video, source, total_frames, fps, chunk_frames, settings):
    stat = source.stat()
    trim = video.get_active_trim_window() if hasattr(video, "get_active_trim_window") else (0.0, 0.0)
    return {
        "version": 1,
        "source": {"path": str(source), "size": stat.st_size, "mtime_ns": stat.st_mtime_ns},
        "trim": [float(trim[0]), float(trim[1])],
        "total_frames": total_frames,
        "fps": f"{fps.numerator}/{fps.denominator}",
        "chunk_frames": chunk_frames,
        "settings": settings,
    }


def _valid_part(path, expected_frames, fps):
    if VideoFromFile is None or not path.is_file() or path.stat().st_size == 0:
        return False
    try:
        part = VideoFromFile(str(path))
        return part.get_frame_count() == expected_frames and Fraction(part.get_frame_rate()) == fps
    except Exception:
        return False


def _write_part(images, fps, path):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("DLSS 5 Video disk chunks require FFmpeg in PATH.")
    height, width = images.shape[1:3]
    path.unlink(missing_ok=True)
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-video_size", f"{width}x{height}",
        "-framerate", f"{fps.numerator}/{fps.denominator}", "-i", "-", "-an",
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-pix_fmt", "yuv420p",
        "-movflags", "+faststart", str(path),
    ]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        for frame in images:
            data = (frame.detach()[..., :3] * 255).clamp(0, 255).byte().cpu().contiguous().numpy()
            view = memoryview(data).cast("B")
            for offset in range(0, len(view), 1024 * 1024):
                process.stdin.write(view[offset:offset + 1024 * 1024])
        process.stdin.close()
        error = process.stderr.read().decode("utf-8", errors="replace")
        return_code = process.wait()
    except BaseException:
        process.kill()
        process.wait()
        path.unlink(missing_ok=True)
        raise
    if return_code:
        path.unlink(missing_ok=True)
        raise RuntimeError("FFmpeg failed to encode a DLSS chunk:\n" + error[-4000:])


def _ffconcat_path(path):
    return str(path.resolve()).replace("\\", "/").replace("'", "'\\''")


def _as_video_components(video):
    """Return the frame components (a VideoComponents object) for a VIDEO input.

    Accepts a VideoInput (the native VIDEO wire type) or a plain file path /
    stream (some loaders emit a path string for the VIDEO slot). The whole
    stream is decoded here into one [B,H,W,C] tensor — the same thing the
    separate video→sequence node did, but now inside this node and in memory.[cite: 2]
    """
    if isinstance(video, torch.Tensor):
        class DummyComps:
            images = video
            frame_rate = None
            audio = None
            metadata = None
        return DummyComps()

    if hasattr(video, "get_components"):
        return video.get_components()
    if VideoFromFile is None:
        raise RuntimeError(
            "This VIDEO input is a file path, but the comfy_api VideoFromFile "
            "loader is unavailable — connect a loader that emits a VideoInput "
            "object instead.")
    return VideoFromFile(video).get_components()


def _build_video(images, frame_rate, audio, metadata):
    """Re-wrap an enhanced [B,H,W,C] float 0..1 tensor into a ComfyUI VIDEO.

    Uses the core's in-memory VideoFromComponents so this node does NOT
    re-encode inside itself — the single encode happens in the downstream
    save node, exactly like the image node hands its tensor to a save node.
    Falls back to the bare tensor if the comfy_api layer is missing.[cite: 2]
    """
    if _HAS_VIDEO_BUILDERS:
        if frame_rate is None:
            frame_rate = Fraction(1)
        comps = VideoComponents(
            images=images, frame_rate=frame_rate, audio=audio, metadata=metadata)
        return VideoFromComponents(comps)
    return images


class DLSS5NeuralRenderingVideo:
    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                # Identical widget set and order to the image-based
                # DLSS5NeuralRendering node (🕹️DLSS 5 (OreX)); the only
                # replacement is the input wire type — a VIDEO socket in place
                # of the IMAGE socket. No "tooltip" fields on purpose: the
                # OreX-style EN/RU tooltips are drawn by js/OreX_DLSS5_video.js
                # (it matches by widget name, so the list must stay in sync
                # with the image node's DLSS5_HELP_DESCRIPTIONS).[cite: 2]
                "upscaling_mode": (list(_UPSCALING_MODES), {
                    "default": "1.5x (Quality)",
                }),
                "dlss_model_preset": (list(_DLSS_MODEL_PRESETS), {
                    "default": "M",
                }),
                "video": ("VIDEO",),
                "style": (["natural", "cinematic", "default"], {
                    "default": "natural",
                }),
                "intensity": ("FLOAT", {
                    "default": 1.0,
                    "min": 0.0,
                    "max": 2.0,
                    "step": 0.05,
                }),
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
                "gpu_mode": ("BOOLEAN", {
                    "default": True,
                    "label_on": "🟢 GPU ON",
                    "label_off": "🔴 GPU OFF",
                }),
                "chunk_frames": ("INT", {
                    "default": 32,
                    "min": 0,
                    "max": 256,
                    "step": 1,
                }),
                "continue_if_stopped": ("BOOLEAN", {
                    "default": False,
                    "label_on": "ON",
                    "label_off": "OFF",
                }),
                "change_frame": ("INT", {
                    "default": 0,
                    "min": 0,
                    "max": 100000,
                    "step": 1,
                }),
            }
        }

    RETURN_TYPES = ("VIDEO",)
    RETURN_NAMES = ("video",)
    # Execution root so the Preview's pruned prompt (only this node + its
    # upstream, built by js/OreX_DLSS5_video.js) passes server validation —
    # the same reason the image node sets this. In a full run it is inert: the
    # upstream is pulled in by the real downstream outputs and this node
    # returns no ui data, so nothing extra is sent over the websocket.[cite: 2]
    OUTPUT_NODE = True
    FUNCTION = "process"
    CATEGORY = "experimental/DLSS 5 NR"
    DESCRIPTION = (
        "Unofficial experimental in-process DLSS 5 Neural Rendering (NGX feature 18) "
        "for video streams. Takes a VIDEO in and emits an enhanced VIDEO out; the "
        "per-frame pipeline is the same as the image-based 🕹️DLSS 5 (OreX) node."
    )

    @classmethod
    def IS_CHANGED(cls, **kwargs):
        """Инвалидируем кэш ComfyUI при переходе между Preview и обычным Run[cite: 2]"""
        import time
        try:
            from . import OreX_DLSS5
            armed = bool(OreX_DLSS5._preview_arm_ts and (time.time() - OreX_DLSS5._preview_arm_ts) < OreX_DLSS5._PREVIEW_ARM_TTL)
            if armed:
                # В режиме Preview всегда возвращаем уникальное значение, чтобы кадры предпросмотра не кэшировались как финал[cite: 2]
                return f"preview_{time.time()}"
        except Exception:
            pass
        # В обычном режиме возвращаем статику, чтобы ComfyUI кэшировал полное видео как обычно[cite: 2]
        return "normal_run"

    def process(
        self,
        video,
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
        chunk_frames=32,
        continue_if_stopped=False,
        change_frame=0,
    ):
        """ComfyUI entry point. Unwrap the input VIDEO into a [B,H,W,C] frame
        tensor, run the (unchanged) image-based DLSS5 pipeline on it, and
        re-wrap the enhanced tensor back into a VIDEO. The frames are one
        in-memory tensor from decode to re-wrap — no intermediate sequence.[cite: 2]
        """
        import time
        import torch
        from . import OreX_DLSS5

        armed = bool(OreX_DLSS5._preview_arm_ts and (time.time() - OreX_DLSS5._preview_arm_ts) < OreX_DLSS5._PREVIEW_ARM_TTL)
        pv_idx = int(change_frame)

        fast_tensor = None
        
        # --- БЫСТРЫЙ ПРЕДПРОСМОТР (Прямое чтение 1 кадра через OpenCV) ---
        if armed:
            if isinstance(video, torch.Tensor):
                pv_idx = max(0, min(pv_idx, video.shape[0] - 1))
                fast_tensor = video[pv_idx : pv_idx + 1]
            else:
                try:
                    import folder_paths
                    
                    # Глубокий рекурсивный поиск файла в объекте[cite: 2]
                    def find_path(obj, depth=0):
                        if depth > 3: return None
                        
                        if isinstance(obj, str):
                            if os.path.isfile(obj): return obj
                            try:
                                p = folder_paths.get_annotated_filepath(obj)
                                if os.path.isfile(p): return p
                            except: pass
                            p = os.path.join(folder_paths.get_input_directory(), os.path.basename(obj))
                            if os.path.isfile(p): return p
                            return None
                        
                        if isinstance(obj, dict):
                            for v in obj.values():
                                p = find_path(v, depth+1)
                                if p: return p
                            return None
                            
                        for attr in dir(obj):
                            if attr.startswith('__') or attr in ['metadata', 'audio', 'frame_rate']: 
                                continue
                            try:
                                val = getattr(obj, attr)
                                if isinstance(val, str) and val.lower().endswith(('.mp4', '.mov', '.avi', '.webm', '.mkv')):
                                    p = find_path(val, depth+1)
                                    if p: return p
                            except: pass
                        return None
                        
                    v_path = find_path(video)
                    
                    if v_path and os.path.isfile(v_path):
                        import cv2
                        cap = cv2.VideoCapture(v_path)
                        total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
                        target = max(0, min(pv_idx, total - 1)) if total > 0 else pv_idx
                        
                        cap.set(cv2.CAP_PROP_POS_FRAMES, target)
                        ret, frame = cap.read()
                        cap.release()
                        
                        if ret:
                            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                            fast_tensor = torch.from_numpy(frame).float().div_(255.0).unsqueeze(0)
                            print(f"[DLSS5-Ox] Fast preview: extracted frame {target} directly from {v_path} via OpenCV")
                        else:
                            print(f"[DLSS5-Ox] Fast preview: OpenCV failed to read frame {target} from {v_path}")
                    else:
                        print(f"[DLSS5-Ox] Fast preview: Could not resolve absolute path. Fallback to full decode.")
                except Exception as e:
                    print(f"[DLSS5-Ox] Fast preview extraction failed: {e}")

        source = _source_path(video)
        if (not armed and int(chunk_frames) > 0 and source is not None
                and hasattr(video, "as_trimmed") and hasattr(video, "get_frame_count")
                and hasattr(video, "get_frame_rate")):
            return self._process_disk_chunks(
                video, source, upscaling_mode, dlss_model_preset, style, preset,
                intensity, tone, structure, skin, auto_mask, temporal_mode,
                scene_change_threshold, warmup_frames, gpu_mode, int(chunk_frames),
                bool(continue_if_stopped), change_frame)

        # --- Декодирование (мгновенное для Preview или полное для штатного запуска) ---
        if fast_tensor is not None:
            # OpenCV успешно достал 1 кадр. Пропускаем тяжелую распаковку ComfyUI.[cite: 2]
            images = fast_tensor
            frame_rate, audio, metadata = None, None, None
        else:
            comps = _as_video_components(video)
            images = comps.images            
            frame_rate = getattr(comps, "frame_rate", None)
            audio = getattr(comps, "audio", None)
            metadata = getattr(comps, "metadata", None)
            
            if armed and images.shape[0] > 0:
                safe_idx = max(0, min(pv_idx, images.shape[0] - 1))
                images = images[safe_idx : safe_idx + 1]

        if not isinstance(images, torch.Tensor) or images.ndim != 4:
            raise RuntimeError(
                f"Expected a ComfyUI VIDEO with a [B,H,W,C] frame tensor, got "
                f"{type(images).__name__} with shape "
                f"{tuple(images.shape) if hasattr(images, 'shape') else 'n/a'}.")

        # Проверка и скачивание DLL перед вызовом основного обработчика
        _ensure_dll_exists()

        impl = OreX_DLSS5.DLSS5NeuralRendering()
        result = impl.process(
            images, upscaling_mode=upscaling_mode, dlss_model_preset=dlss_model_preset,
            style=style, preset=preset, intensity=intensity, tone=tone, structure=structure,
            skin=skin, auto_mask=auto_mask, temporal_mode=temporal_mode,
            scene_change_threshold=scene_change_threshold, warmup_frames=warmup_frames,
            gpu_mode=gpu_mode, chunk_frames=chunk_frames, change_frame=change_frame,
        )
        
        # Корректировка номера кадра в UI для Preview
        if armed and OreX_DLSS5._preview_result is not None:
            OreX_DLSS5._preview_result["frame"] = pv_idx

        result_tensor = result[0]        
        del images                       

        out = _build_video(result_tensor, frame_rate, audio, metadata)
        return (out,)

    def _process_disk_chunks(
        self, video, source, upscaling_mode, dlss_model_preset, style, preset,
        intensity, tone, structure, skin, auto_mask, temporal_mode,
        scene_change_threshold, warmup_frames, gpu_mode, chunk_frames,
        continue_if_stopped, change_frame,
    ):
        total_frames = int(video.get_frame_count())
        fps = Fraction(video.get_frame_rate())
        if total_frames < 1:
            raise RuntimeError("The input video contains no frames.")

        settings = {
            "upscaling_mode": upscaling_mode,
            "dlss_model_preset": dlss_model_preset,
            "style": style,
            "preset": preset,
            "intensity": intensity,
            "tone": tone,
            "structure": structure,
            "skin": skin,
            "auto_mask": auto_mask,
            "temporal_mode": temporal_mode,
            "scene_change_threshold": scene_change_threshold,
            "warmup_frames": warmup_frames,
            "gpu_mode": gpu_mode,
        }
        config = _resume_config(video, source, total_frames, fps, chunk_frames, settings)
        resume_id = hashlib.sha256(json.dumps(config, sort_keys=True).encode("utf-8")).hexdigest()[:16]
        resume_root = Path(folder_paths.get_output_directory()) / "dlss5_resume"

        if continue_if_stopped:
            temp_root = resume_root / resume_id
            resume_root.mkdir(parents=True, exist_ok=True)
            _clear_directory(resume_root, keep=temp_root)
        else:
            shutil.rmtree(resume_root, ignore_errors=True)
            temp_root = Path(tempfile.mkdtemp(prefix="dlss5_video_", dir=folder_paths.get_temp_directory()))
        temp_root.mkdir(parents=True, exist_ok=True)

        plan = [(start, min(start + chunk_frames, total_frames))
                for start in range(0, total_frames, chunk_frames)]
        parts = [temp_root / f"part_{index:05d}.mp4" for index in range(len(plan))]
        manifest_path = temp_root / "manifest.json"
        manifest = {"config": config, "completed_parts": []}
        if continue_if_stopped and manifest_path.is_file():
            try:
                existing = json.loads(manifest_path.read_text(encoding="utf-8"))
                if existing.get("config") == config:
                    manifest = existing
            except Exception:
                pass
        _write_manifest(manifest_path, manifest)

        _ensure_dll_exists()
        progress = comfy.utils.ProgressBar(len(plan))
        success = False
        try:
            for index, ((start, end), part_path) in enumerate(zip(plan, parts)):
                comfy.model_management.throw_exception_if_processing_interrupted()
                count = end - start
                if continue_if_stopped and _valid_part(part_path, count, fps):
                    print(f"[DLSS5-Ox] Resume: chunk {index + 1}/{len(plan)} is already complete")
                    progress.update(1)
                    continue

                sliced = video.as_trimmed(
                    float(Fraction(start, 1) / fps),
                    float(Fraction(count, 1) / fps),
                    strict_duration=False,
                )
                if sliced is None:
                    raise RuntimeError(f"Could not read DLSS chunk {index + 1}: frames {start}..{end - 1}")
                components = sliced.get_components()
                images = components.images
                if images.shape[0] != count:
                    raise RuntimeError(
                        f"DLSS chunk {index + 1}: expected {count} frames, decoded {images.shape[0]}")

                impl = DLSS5NeuralRendering()
                result = impl.process(
                    images, upscaling_mode=upscaling_mode,
                    dlss_model_preset=dlss_model_preset, style=style, preset=preset,
                    intensity=intensity, tone=tone, structure=structure, skin=skin,
                    auto_mask=auto_mask, temporal_mode=temporal_mode,
                    scene_change_threshold=scene_change_threshold,
                    warmup_frames=warmup_frames, gpu_mode=gpu_mode,
                    chunk_frames=0, change_frame=change_frame,
                )[0]
                _write_part(result, fps, part_path)

                completed = set(manifest.get("completed_parts", []))
                completed.add(index)
                manifest["completed_parts"] = sorted(completed)
                _write_manifest(manifest_path, manifest)
                progress.update(1)

                del result, images, components, sliced, impl
                gc.collect()
                comfy.model_management.soft_empty_cache()

            for (start, end), part_path in zip(plan, parts):
                if not _valid_part(part_path, end - start, fps):
                    raise RuntimeError(f"Invalid or incomplete DLSS chunk: {part_path.name}")

            first = VideoFromFile(str(parts[0]))
            width, height = first.get_dimensions()
            output_folder, filename, counter, subfolder, _ = folder_paths.get_save_image_path(
                "video/DLSS5_video", folder_paths.get_output_directory(), width, height)
            output_file = f"{filename}_{counter:05}_.mp4"
            final_path = Path(output_folder) / output_file
            concat_path = temp_root / "parts.ffconcat"
            concat_path.write_text(
                "ffconcat version 1.0\n" + "".join(
                    f"file '{_ffconcat_path(path)}'\n" for path in parts),
                encoding="utf-8",
            )

            ffmpeg = shutil.which("ffmpeg")
            trim_start, trim_duration = (video.get_active_trim_window()
                                         if hasattr(video, "get_active_trim_window") else (0.0, 0.0))
            duration = float(Fraction(total_frames, 1) / fps)
            audio_input = []
            if trim_start > 0:
                audio_input += ["-ss", f"{trim_start:.9f}"]
            audio_input += ["-t", f"{min(trim_duration, duration) if trim_duration > 0 else duration:.9f}"]
            command = [
                ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
                "-f", "concat", "-safe", "0", "-i", str(concat_path),
                *audio_input, "-i", str(source),
                "-map", "0:v:0", "-map", "1:a:0?", "-c:v", "copy",
                "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart",
                str(final_path),
            ]
            assembled = subprocess.run(command, capture_output=True, text=True)
            if assembled.returncode:
                final_path.unlink(missing_ok=True)
                raise RuntimeError("FFmpeg failed to assemble DLSS chunks:\n" + assembled.stderr[-4000:])

            success = True
            result_video = VideoFromFile(str(final_path))
            ui = {
                "images": [{"filename": output_file, "subfolder": subfolder, "type": "output"}],
                "animated": (True,),
            }
            return {"ui": ui, "result": (result_video,)}
        except Exception:
            if continue_if_stopped:
                print(f"[DLSS5-Ox] Resume data saved: {temp_root}")
            raise
        finally:
            if success or not continue_if_stopped:
                shutil.rmtree(temp_root, ignore_errors=True)
            if resume_root.is_dir():
                try:
                    resume_root.rmdir()
                except OSError:
                    pass
