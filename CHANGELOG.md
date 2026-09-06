# Changelog

## 0.9.28 - 2026-09-05

**Preview fixed: it now queues ONLY the subgraph feeding the DLSS5 node (the rgthree-comfy "Queue Selected Output Nodes" pattern) instead of the whole workflow.** In v0.9.27 Preview queued the FULL graph — if any downstream node failed server-side validation (missing model, broken link), the entire queue was rejected and nothing ran, so the button appeared dead. Now the JS builds the prompt with `app.graphToPrompt()`, prunes it to this node + everything that feeds it, and queues only that: upstream runs exactly like a full run, this node processes only the `change_frame` frame, and downstream nodes are not in the prompt at all — so a preview no longer writes any downstream outputs (the v0.9.27 one-frame-video side effect is gone).

- **Pruned queue** (`js/OreX_DLSS5.js`): new `_dlss5AddUpstream()` — rgthree-comfy's `recursiveAddNodes` pattern with defensive guards: keep the node, recursively add every linked input (in a prompt dict, links are `[upstreamId, slot]` arrays; widget values are scalars or `{__value__: [...]}` wrappers, so a plain array marks a link; the visited-set guard also cuts cycles and dedupes). Both frontend shapes are handled: new frontends' `graphToPrompt()` returns a `{workflow, output}` wrapper (the pruned `output` is queued, the original `workflow` kept as metadata), classic builds return the bare node dict. If the node is muted/bypassed it is absent from the prompt and Preview fails with a clear message instead of queueing something else.
- **`OUTPUT_NODE = True`** (`OreX_DLSS5.py`): ComfyUI accepts only `OUTPUT_NODE=True` nodes as execution roots, and a pruned subgraph contains no other output node — without this the server rejects the prompt with "Prompt has no outputs". In a normal full run this is inert: the node's upstream is already pulled in by the real outputs downstream, and it returns no ui data, so nothing extra goes over the websocket. Bonus: the node now also appears as a selectable target in the frontend's "Queue Selected Output Nodes" menu (rgthree-comfy style).
- **Websocket error event** (`js/OreX_DLSS5.js`): v0.9.27 listened for `exec_error`, but this server sends `execution_error` — an execution failure was never seen, so the button just polled to the 10-minute deadline. Now both names are listened (new frontends dispatch a CustomEvent with the payload in `.detail`; classic builds pass the payload object directly), matched by `prompt_id` when available.
- **Copy `OreX_DLSS5.py` and `js/OreX_DLSS5.js`, then RESTART ComfyUI** (node definitions are read at startup) and hard-refresh the browser (Ctrl+Shift+R). No bridge changes — no DLL to copy; `__init__.py` is unchanged.

## 0.9.27 - 2026-09-05

**Preview redesigned after field testing: it now runs the real graph instead of reusing a cached batch.** In v0.9.26 Preview could only work after a full run had already happened (it processed one frame of the input batch kept from that run) — on a fresh start, or right after a restart, the button did nothing. Now Preview arms the server and queues the actual workflow through ComfyUI's normal prompt path: everything upstream runs exactly like a full run, and when the DLSS5 node executes it processes only the `change_frame` frame of its input batch (an index beyond the end of the sequence takes the last frame) and shows the result under the buttons. No prior run is needed, and no batch is kept in RAM between runs anymore.

- **How Preview works now** (`js/OreX_DLSS5.js`, `OreX_DLSS5.py`): clicking Preview POSTs `/dlss5/preview-arm`, then builds the prompt with `app.graphToPrompt()` and queues it with `app.api.queuePrompt(0, prompt)` — a real run of the current graph. When this node's `process()` executes it sees the fresh arm, clamps `change_frame` to `[0, count-1]`, processes only that one frame (temporal history starts fresh at it), and encodes the result to PNG in module-level `_preview_result`. The JS polls `/dlss5/preview-status` every 700 ms (10-minute deadline) with an early exit on the `exec_error` websocket event, then fetches the PNG from `/dlss5/preview-result` (used index in the `X-DLSS5-Frame` header) and draws it under the buttons with the signature blue border and a "frame N" chip. On any failure the JS POSTs `/dlss5/preview-disarm`, flashes the button red, and logs to the console.
- **Arm TTL** (`OreX_DLSS5.py`): an arm older than `_PREVIEW_ARM_TTL` (600 s) is stale — e.g. the queued run died before reaching this node — and is ignored, so it cannot turn a later ordinary run into a preview. The arm is consumed immediately even if processing fails.
- **API reshuffle** (`__init__.py`): `POST /dlss5/preview` (v0.9.26) is replaced by four endpoints — `POST /dlss5/preview-arm`, `POST /dlss5/preview-disarm`, `GET /dlss5/preview-status` (idle/armed/done/error), `GET /dlss5/preview-result` (PNG, consumed on fetch; 409 JSON when there is none).
- **Removed** (`OreX_DLSS5.py`): the `_preview_source` input-batch cache, the `_preview_reentry` guard, `render_preview_frame()`, and `_PREVIEW_PARAM_KEYS` — nothing from a previous run is kept in RAM anymore.
- **Side effect (by design):** a preview is a real run, so it produces normal downstream outputs (e.g. a one-frame video) and shows progress in the UI like any other queue item. It is still a fast parameter check: only one frame goes through the bridge, with fresh temporal history at that frame — not a slice of the finished video.
- **Copy `OreX_DLSS5.py`, `__init__.py` and `js/OreX_DLSS5.js`, then RESTART ComfyUI** (the new server routes register at import time) and hard-refresh the browser (Ctrl+Shift+R). No bridge changes — no DLL to copy.

## 0.9.26 - 2026-09-05

**`presets.json` is gone for good, and the node gains a single-frame Preview: a `change_frame` INT widget under `chunk_frames` plus two canvas-drawn buttons in one row — Preview and Hide.** Preview runs the full pipeline on ONE frame of the input batch from your last run and shows the result right under the buttons; an index beyond the end of the sequence takes the last frame. New defaults: `chunk_frames` 64, `tone`/`structure`/`skin` 2.0 (`style` was already natural).

- **presets.json removed** (`__init__.py`): the leftover v0.9.19 presets API is deleted — the auto-creation of an empty `presets.json` at server start, `GET /dlss5/presets`, `POST /dlss5/presets/save`, and their helpers (`_load_presets`, `_save_presets`, `_clean_preset_params`). The preset feature left the node itself in v0.9.22; only dead backend code remained. The file is no longer created or read anywhere — delete any existing `presets.json` by hand if you want it gone from disk.
- **New `change_frame` INT widget** (`OreX_DLSS5.py`): default 0, range 0..100000, placed directly under `chunk_frames`. Not used by a full run — the JS reads it when Preview is clicked and sends it to the new API. OreX EN/RU tooltip added (🎯).
- **Preview/Hide buttons** (`js/OreX_DLSS5.js`): drawn ON CANVAS in one row under `change_frame` (no DOM element — the v0.9.19–v0.9.21 DOM button did not appear in all builds), each half the node width. Preview POSTs the current widget values plus the frame index to `POST /dlss5/preview`; the result PNG is drawn under the buttons with the signature blue border and a "frame N" label (N = the actually used index). Hide removes the image. Buttons flash green/red on success/failure, show "Preview…" while busy, get a pointer cursor + OreX-blue hover border; clicks are hit-tested in `onMouseDown` and consumed. The per-type `computeSize` patch reserves the button row + image height so nothing is clipped, and `refreshSize()` resizes the node when the preview appears/disappears.
- **New `POST /dlss5/preview` API** (`__init__.py`): body `{"frame": int, "params": {...}}`. The heavy work runs in an executor thread so the aiohttp event loop (and the UI) stays responsive; the native bridge is serialized by the same `_lock` a full run uses. Response: `image/png` bytes with the used index in the `X-DLSS5-Frame` header; 409 JSON when no sequence has been run yet, 500 on other failures.
- **How Preview works** (`OreX_DLSS5.py`): `process()` keeps a reference to the input batch it was called with (before the GPU move) in module-level `_preview_source`; `render_preview_frame(frame_index, params)` clamps the index to `[0, count-1]` — beyond the end of the sequence the last frame is taken — and runs `process()` on that one frame with the forwarded widget values. The inner call runs on a reentry thread so it does not clobber the cache with its 1-frame slice. Temporal history starts fresh at the chosen frame: this is a fast parameter check, not a slice of the finished video.
- **Memory note:** holding `_preview_source` keeps the last input batch in RAM between runs (ComfyUI would otherwise be free to evict it), so long sequences stay resident until the next run or a restart — the node prints one line per run (`preview: keeping N frame(s) on <device> for the Preview button…`). With several DLSS5 nodes, the last run's input wins.
- **New defaults:** `chunk_frames` 0 → **64** (a sensible chunk for 1080p-class footage; 0 still = off), `tone` 1.0 → **2.0**, `structure` 1.0 → **2.0**, `skin` −1.0 → **2.0** (min stays −1.0). `style` default was already `natural` — unchanged.
- **Removed from the JS:** all dead preset code (`DLSS5_PRESET_PARAMS`, `_dlss5PresetsCache`, `_dlss5LoadPresets`, `_dlss5ApplyPreset`, the Save-button flow, the `preset` tooltip entry).
- **Copy `OreX_DLSS5.py`, `__init__.py` and `js/OreX_DLSS5.js`, then RESTART ComfyUI** (the new API route registers at import time) and hard-refresh the browser (Ctrl+Shift+R). No bridge changes — no DLL to copy.

## 0.9.25 - 2026-09-04

**Chunked mode now uses VRAM — the CUDA/CPU path is chosen per chunk, not once for the whole run.** In v0.9.23/0.9.24 a sequence whose whole batch did not fit the free VRAM (GPU ON) kept every frame on CPU and ran *all* chunks through slow CPU staging — even though each individual chunk would easily fit in VRAM. Now `_run_chunked()` decides per chunk: a CPU chunk that fits (input + temp output + 1 GiB headroom ≤ free VRAM) is moved to the bridge GPU and runs zero-copy, its result is copied back into the output tensor right after (one D2H copy per chunk), so peak VRAM stays O(chunk) while inference runs at VRAM speed. A chunk that does not fit — or any chunk after a non-native CUDA failure, which disables interop for the process — runs on pinned CPU staging; the failed chunk is retried there with fresh history (per-frame resets), exactly like the unchunked retry, and earlier chunks keep their results. When the whole batch *does* fit, behavior is unchanged: the input moves to VRAM once and chunks run zero-copy straight into the output tensor.

- **Log prefix renamed `[DLSS5-NR]` → `[DLSS5-Ox]`** across the node, `__init__.py`, the bridge build script, README, LICENSE and changelog log quotes. Upstream attribution (`lisitskyaa/ComfyUI-DLSS5-NR`) and the bridge source copyright headers are untouched.
- **Expected log for a 791-frame / 1 MP / `chunk_frames=100` run with GPU ON when the batch does not fit**: after `keeping frames on cpu: ... processing in chunks of 100 frames instead` you should now see `path: CUDA zero-copy, chunked (100 frames/chunk; chunks moved to the bridge GPU as they fit)` plus per-chunk `cuda zero-copy: frame N/791` heartbeats — inference in VRAM. The result tensor stays where the input was (CPU); peak VRAM ≈ 2 × one chunk + 1 GiB (~3.4 GiB for a 100-frame 1 MP chunk).
- **Copy `OreX_DLSS5.py`, then RESTART ComfyUI.** JS is unchanged since 0.9.23 — re-copy `js/OreX_DLSS5.js` only if you have not done it yet (then hard-refresh the browser, Ctrl+Shift+R). No bridge changes — no DLL to copy.

## 0.9.24 - 2026-09-04

**Fix: chunked CPU staging crashed on frame 0 with `can't assign a numpy.ndarray to a torch.FloatTensor`.** The v0.9.23 field run (791 frames, 1 MP, `chunk_frames=200`, GPU ON but the whole batch did not fit VRAM → chunked CPU staging) died right after `staging: frame 0/791`: `fill_out[i] = corrected` assigns a numpy array into a torch tensor slice, and PyTorch's `__setitem__` does not implicitly convert numpy values. Each result is now written with `fill_out[i].copy_(torch.from_numpy(corrected))` — a zero-copy view plus a device-correct copy (plain memcpy for a CPU output tensor, H2D DMA for a CUDA one).

- **Copy `OreX_DLSS5.py`, then RESTART ComfyUI.** JS is unchanged since 0.9.23 — re-copy `js/OreX_DLSS5.js` only if you have not done it yet (then hard-refresh the browser, Ctrl+Shift+R). No bridge changes — no DLL to copy.

## 0.9.23 - 2026-09-04

**`chunk_frames` — long videos no longer need to fit RAM/VRAM whole.** New INT widget under `gpu_mode`: `0` = off (the pre-0.9.23 behavior, bit-for-bit), `>0` = process that many frames per chunk and assemble the result into a single full-length tensor. Long videos were slow because the GPU ON VRAM fit check refused the move for big batches (silent ~2× slower CPU staging fallback) and both paths held input batch + output batch in memory at once; now only one chunk + the output tensor (which must exist anyway) are live at a time.

- **New `chunk_frames` widget** (`OreX_DLSS5.py`): INT, default 0, range 0..256, placed directly under `gpu_mode`; OreX EN/RU tooltip added in `js/OreX_DLSS5.js`.
- **New `_run_chunked()`**: pre-allocates the full output tensor once on the input device and fills it chunk by chunk. Per chunk: convert dtype + resample only that slice, compute its tiny scene-detection luma array, then run the existing per-frame core — CUDA zero-copy when the input sits on the bridge GPU, pinned CPU staging otherwise. The per-chunk pinned buffers are reused from the shape cache, so pinned RAM stays at a few GiB no matter the video length.
- **Identical-output guarantee**: frames stay in order on the same persistent bridge session, so temporal history carries across chunk boundaries exactly as in an unchunked run — no boundary resets, no frame overlap needed, no quality loss at the seams. Scene-detect state (`prev_gray`) is threaded through the chunks via `scene_state`; resets happen only at frame 0 and on detected scene changes, the same trajectory as unchunked.
- **Runner refactor**: `_run_cpu_staging()` / `_run_cuda_batch()` now accept a `start`/`total` frame range and an optional `fill_out` full-size tensor; with default arguments the unchunked calls are unchanged. Shared helpers extracted: `_scene_reset()`, `_scene_grays()`, `_cuda_blockers()` (the CUDA gate message is now identical on both paths).
- **Error semantics preserved**: watchdog timeout, native crash → fail the run with RESTART advice (no fallback), non-native CUDA failure → retry the whole chunked batch on CPU staging with forced resets — same as the unchunked path. With `chunk_frames > 0` the GPU ON "does not fit in VRAM" message now reads "processing in chunks of N frames instead".
- **Memory example** (600 frames, 1080p → 1.5x): node buffers drop from ~48 GiB (input f32 batch + output f32 batch at once) to ~36 GiB (full output + one 32-frame chunk); the gap grows linearly with sequence length — a 3000-frame video is simply impossible unchunked (~241 GiB) but runs with `chunk_frames=32`.
- **Copy `OreX_DLSS5.py` and `js/OreX_DLSS5.js`, then RESTART ComfyUI** (node definitions are read at startup) and hard-refresh the browser (Ctrl+Shift+R). No bridge changes — no DLL to copy.

## 0.9.22 - 2026-09-04

**The `preset` combo was removed from the node — all values measured to produce identical output on current builds.** The widget is gone from the UI; `process()` still accepts `preset="Default"` so old saved workflows keep loading (unknown prompt inputs are dropped server-side). `upscaling_mode` moved back to the top of the node.

- **Copy `OreX_DLSS5.py`, then RESTART ComfyUI** and hard-refresh the browser (Ctrl+Shift+R).

## 0.9.21 - 2026-09-04

**The floppy Save button is now drawn directly on the node canvas — no DOM involved — because the DOM version did not appear in field builds.** In v0.9.19/0.9.20 the button was a `<div>` attached to `node._dom_element` (with a fallback to the preset field's container); that layer has proven unreliable here, so the button is now part of the node's own draw pass — it renders wherever the node renders, at any zoom/pan.

- **Canvas-drawn button** (`js/OreX_DLSS5.js`): a 26×26 rounded square with the floppy icon (SVG path via `Path2D`) at the right edge of the preset row; the preset select is narrowed so it does not cover the button area. Hover highlights the border in OreX blue and sets a pointer cursor; clicking runs the same save flow as before — the 7 parameters (`upscaling_mode`, `dlss_model_preset`, `style`, `intensity`, `tone`, `structure`, `skin`) are POSTed to `/dlss5/presets/save` under the currently selected preset name, the button flashes green on success / red on failure, and details go to the browser console.
- **Clicks** are hit-tested in a wrapped `node.onMouseDown` (node-local coordinates, 2 px padding) and consumed so they never fall through to widgets or the graph; hover state is tracked in wrapped `onMouseMove`/`onMouseLeave`, and the OreX tooltip is suppressed while the pointer is over the button.
- **Late-setup safety net:** if a node was created before the extension registered (so `nodeCreated` never fired for it), its controls are set up on the first draw via a guarded hook in the prototype `onDrawForeground`. A one-line console marker per node (`[OreX_DLSS5] save button ready`) confirms the button is active.
- **Removed:** all `.dlss5-save-btn` CSS and the `_dom_element` attach/positioning machinery — the JS file no longer creates any DOM element of its own (the tooltip only binds to the fields ComfyUI already renders).
- **Python `0.9.20` → `0.9.21`:** version bump + comment refresh only — no functional Python changes, the button is pure frontend.
- **Copy `OreX_DLSS5.py` and `js/OreX_DLSS5.js`, then RESTART ComfyUI** and hard-refresh the browser (Ctrl+Shift+R). `__init__.py` is unchanged since 0.9.19. If the button still does not appear, check the console (F12) for `[OreX_DLSS5] save button ready` — its absence means the JS file did not load.

## 0.9.20 - 2026-09-04

**Fix for v0.9.19: `gpu_mode` is back as a native widget, styled exactly like the OreX LM Studio nodes — which was the original intent all along.** The v0.9.19 JS DOM switch hid the native BOOLEAN row and rendered its own pill over it; in field builds the overlay did not appear, so the row simply vanished from the node. Since `gpu_mode` is the most important control of the node, the overlay approach is dropped entirely: the widget now renders natively with `label_on`/`label_off` ("🟢 GPU ON" / "🔴 GPU OFF") — the same mechanism as `include_reasoning` in OreX_LMStudio.py — and the OreX EN/RU tooltip box works over its field as before.

- **Python `0.9.19` → `0.9.20`:** `gpu_mode` labels are now emoji-first, matching the LM Studio nodes: `label_on` = "🟢 GPU ON", `label_off` = "🔴 GPU OFF". Comments updated; no other Python changes — the widget is a plain native BOOLEAN again, nothing hides or redraws its row.
- **JS (`js/OreX_DLSS5.js`):** the entire DOM switch overlay for gpu_mode is removed (`.dlss5-switch*` CSS, element construction, click/tooltip handlers, and the `gpuW.draw = () => {}` / `inputEl.style.display = "none"` row hiding) — the native widget is untouched again. The floppy Save button and preset-apply-on-selection from v0.9.19 are kept unchanged; the Save button now also falls back to the container of the preset field if `node._dom_element` is unavailable, so it cannot silently disappear with it.
- **Copy `OreX_DLSS5.py` and `js/OreX_DLSS5.js`, then RESTART ComfyUI** (the node definitions are read at startup) and hard-refresh the browser (Ctrl+Shift+R). `__init__.py` is unchanged since 0.9.19 — no need to copy it again.

## 0.9.19 - 2026-09-04

**`gpu_mode` now looks like the OreX toggle switch, a floppy Save button appears to the right of the preset row, and selecting a preset applies its saved parameters.** Three UI features added on request — all frontend plus a small presets API; no bridge or processing-path changes.

- **gpu_mode — OreX-style DOM toggle switch** (`js/OreX_DLSS5.js`): the widget row is rendered as a pill switch — green track/knob with "GPU ON 🟢" when on, gray/red "GPU OFF 🔴" when off — positioned over the original row. The native BOOLEAN widget itself is NOT removed (it stays the single source of truth for serialization; only its `draw` and `inputEl` are hidden), so saved workflows keep working unchanged. If the extension ever fails to load, the new Python-side `label_on`/`label_off` ("GPU ON 🟢" / "GPU OFF 🔴") give the same look as a fallback, matching the LM Studio nodes. The OreX tooltip box also works over the switch.
- **Save button (floppy icon)**: on the same row as `preset`, at its right edge (the preset select is narrowed so it does not run under the button). Clicking it saves the current `upscaling_mode`, `dlss_model_preset`, `style`, `intensity`, `tone`, `structure` and `skin` under the preset name currently selected in the list, into `presets.json` in the node's root folder. The button flashes green on success / red on failure; details go to the browser console.
- **Preset apply on selection**: choosing a preset from the list applies its saved parameters to the widgets. A parameter that does not exist on the node, or whose value is outside the allowed choices/range, is ignored (the working ones are still applied) and each problematic/non-existent parameter name is written to the console log with the reason.
- **New presets API** (`__init__.py`): `GET /dlss5/presets` returns the current `presets.json` content; `POST /dlss5/presets/save` validates the preset name and every parameter (combo values against the current option lists, floats must be finite) before writing — invalid entries are dropped and reported in the response. An empty `presets.json` is created at server start if missing, so it can also be edited by hand between runs.
- **Python `0.9.18` → `0.9.19`:** `label_on`/`label_off` on `gpu_mode`, presets API routes in `__init__.py`; the processing path and the bridge are untouched.
- **Copy `OreX_DLSS5.py`, `__init__.py` and `js/OreX_DLSS5.js`, then RESTART ComfyUI** (the new API routes register at import time) and hard-refresh the browser (Ctrl+Shift+R). No bridge changes — no DLL to copy.

## 0.9.18 - 2026-09-04

**The zero-copy path is fast but the pictures had stripes — because the conversion kernels addressed the frame textures with raw pointer math that assumed the driver's physical row layout equals its copyable footprint. Field drivers violate that; now every pixel move goes through `CopyTextureRegion` (the exact mechanism of the clean CPU path), so the driver itself translates to the true physical layout.** The v0.9.17 field run with GPU ON completed all 241 frames in 10.34 s (vs 20.68 s GPU OFF) with no crash and no fallback — but every output had fine and coarse horizontal/vertical moiré stripes ("old TV with a bad signal"), persistent across all node settings and modes. GPU OFF (CPU staging) produced clean images at the same size.

- **Why the CPU path was clean and the CUDA path striped.** The CPU path never touches texture memory directly: pixels move between plain LINEAR buffers (unambiguous row-major layout) and the textures via `CopyTextureRegion`, which makes the driver translate to whatever physical layout it actually chose. The old CUDA path instead computed raw pointers into the placed shared-heap textures (`mapped heap base + GetCopyableFootprints().Offset`, rows at `RowPitch`) and let the kernels read/write there directly — correct only if the texture's physical row pitch equals the copyable footprint's RowPitch. On this field GPU/driver (RTX 5090 / 616.56) a placed shared-heap R16G16B16A16_FLOAT texture does NOT honor that, so each row was sheared by a growing amount — the classic progressive-shift moiré, in both directions and at several frequencies, independent of all NR settings (it is a memory-layout error, not an inference one). The standalone probe never caught it because its round trip read back through the same raw pointers: a systematic layout mismatch cancels in a round trip.
- **Bridge `0.5.7-nosem` → `0.5.8-bufcopy`:**
  - Two new shared DEFAULT-heap LINEAR buffers in VRAM (`g_inbuf`, `g_outbuf`; each `row_pitch × height`, heap rounded to the 64 KB shared alignment) are created in `AllocateFrameResources` and imported into CUDA as external memory **instead of the frame-texture heaps**. The kernels now address only plain linear memory (base = heap offset 0, pitch = copyable RowPitch — a layout with no ambiguity).
  - Per-frame flow: input kernel → `g_inbuf`; `cuStreamSynchronize`; record `[CopyTextureRegion g_inbuf → g_color]` + NGX evaluate + `[CopyTextureRegion g_output → g_outbuf]` in one command list; `ExecuteAndWait` (the CPU fence wait now covers input copy + inference + output copy); output kernel ← `g_outbuf`. This is the exact barrier/copy structure of the proven-clean CPU path, with all pixels staying in VRAM — no PCIe traffic, speed preserved.
  - Cross-frame ordering is safe by construction: the next frame's input kernel is queued on the same CUDA stream after this frame's async output tail and only touches `g_inbuf` after D3D12 has consumed it (CPU-synced); the next D3D12 write of `g_outbuf` is recorded only after the `cuStreamSynchronize` that waits for this frame's output kernel to finish reading it.
  - Barriers transition `g_color` NPSSR→COPY_DEST→NPSSR and `g_output` UA→COPY_SOURCE→UA, ending in exactly the states the CPU path leaves behind — mixed sessions stay consistent. VRAM cost: two extra ~18 MiB buffers at 1344×1728 (negligible next to the batch).
  - Stage list updated for the new copy steps: `register cuda textures` → `register cuda buffers`, plus `input buffer->texture copy (d3d12)` and `output texture->buffer copy (d3d12)`.
- **Python `0.9.17` → `0.9.18`:** version bump only — the `dlss5nr_process_cuda` signature is unchanged, no functional Python changes.
- **Copy `bridge/bin/dlss5nr_bridge.dll` and `OreX_DLSS5.py`, then RESTART ComfyUI** (the bridge is loaded at node init) and hard-refresh the browser (Ctrl+Shift+R). Retest with GPU ON: expect `[DLSS5-Ox] path: CUDA zero-copy (pixels stay in VRAM, no PCIe traffic)`, ~10 s for 241 frames as before, and **clean images — no stripes**. If any stage-tagged AV appears, send the log; the new stage names pinpoint which copy step is involved.

## 0.9.17 - 2026-09-04

**The stage tag did its job: the crash is pinned to `cuSignalExternalSemaphoresAsync` — so the semaphore is gone, and a native crash now fails the run fast instead of wedging.** The v0.9.16 field run with GPU ON hung at 17% after the SEH trampoline caught the AV precisely at stage `'signal fence (cuda)'`; the CPU-staging fallback then froze right after `staging: frame 0/241` (the known rebuild wedge — blocked on a GPU fence, zero load). The follow-up GPU OFF run completed all 241 frames in 24.75 s.

- **What the field log actually showed.** The "Windows fatal exception: access violation" + thread-stack dump and the stage-tagged error are **two prints of one AV**: ComfyUI enables `faulthandler`, whose vectored handler runs before the SEH trampoline's `__except` and prints all threads, then the trampoline catches the same AV. The crash is a driver-internal state interaction specific to torch-hosted processes — the standalone probe passes all 13 handshake steps (including the semaphore signal/wait) at both 128×128 and 768×1344 on the same GPU/driver, so the call itself is legal; it only crashes after NGX inference has run in-process.
- **Bridge `0.5.6-cuda-seh` → `0.5.7-nosem`:**
  - The GPU-side external-semaphore handoff is removed entirely (`cuImportExternalSemaphore` / `cuSignalExternalSemaphoresAsync` / `cuWaitExternalSemaphoresAsync` gone). Ordering between the caller's CUDA stream and the D3D12 queue is now **CPU-side**: input kernel → `cuStreamSynchronize` (~tens of µs against a ~100 ms inference) → eval + `ExecuteAndWait` (its CPU fence wait proves `g_output` is fully written) → output kernel left queued as an async tail. **Zero-copy memory import is preserved** — pixels still never cross PCIe; only two cheap stream syncs were added per frame.
  - The D3D12 fence keeps `D3D12_FENCE_FLAG_SHARED` (no longer required for the handoff, harmless) and `g_fence_value` is untouched, so `ExecuteAndWait`/`WaitQueueIdle` are unchanged.
  - **Post-crash behavior changed:** `g_native_av_pending` no longer triggers an in-process session rebuild — that rebuild has been observed twice to wedge (v0.9.11, and this field run). Both process prologues now fail fast with "session corrupted by a previous native crash — RESTART ComfyUI before the next run". The device-removed (`DeviceHealthy()` false) rebuild path is kept separately for driver resets/TDR, where it is safe and necessary.
  - Stage list updated: `signal fence (cuda)` / `queue wait (d3d12)` / `wait fence (cuda)` → single `input stream sync (cuda)`.
- **Python `0.9.16` → `0.9.17`:**
  - New `_is_native_crash_error()`: matches access violations (caught or escaped), the "Windows fatal exception" text, and watchdog "wedged" timeouts.
  - CUDA-path except: a native crash now **fails the run with restart advice instead of falling back to CPU staging** (that fallback is exactly what wedged in the field run). Non-native CUDA failures (stream mismatch, size change, ...) still fall back as before.
  - CPU-staging except generalized beyond optical_flow: any native crash there also prints restart advice (previously only `use_of` + AV printed, and OF is dormant since v0.9.15).
- **Copy `bridge/bin/dlss5nr_bridge.dll` and `OreX_DLSS5.py`, then RESTART ComfyUI** (the bridge is loaded at node init) and hard-refresh the browser (Ctrl+Shift+R). Retest with GPU ON: expect `[DLSS5-Ox] path: CUDA zero-copy (pixels stay in VRAM, no PCIe traffic)` and all 241 frames completing with no fallback line. If a native AV ever fires again, the run now stops immediately with a clear RESTART message instead of hanging — send that log, the stage name still pinpoints the call.

## 0.9.16 - 2026-09-04

**The zero-copy path now survives native crashes instead of killing ComfyUI — and `gpu_mode` becomes a real switch.** Field test with GPU ON activated the CUDA zero-copy path for the first time, then frame 0 died inside `dlss5nr_process_cuda` with an access violation: CPython converted it into "Windows fatal exception: access violation" + `OSError`, which the watchdog re-raised verbatim and failed the whole run (4.96 s). The process was left poisoned — a follow-up GPU OFF run froze at ~20% because the interrupted frame had left the D3D12 queue waiting on a fence value that is never signaled again.

- **The handshake itself is proven sound.** A new probe (`bridge/tools/probe_handshake.cpp`, built by `bridge/tools/build_probe_handshake.ps1`) executes all 13 zero-copy handshake steps on a live GPU without any NGX session — shared heap create + `cuImportExternalMemory`, `GetMappedBuffer` ×2, PTX module load, kernel launches into the imported pointers, fence NT handle → `cuImportExternalSemaphore`, signal/wait, and a round-trip value check (max|diff| = 0.000242). It passes at both 128×128 and the exact field size 768×1344 on the RTX 5090 / driver 616.56. So the mechanism works; the field AV is a driver-internal state interaction inside a torch-hosted process — which cannot be replicated in a standalone probe, so the fix is **containment**, not reproduction.
- **Bridge `0.5.5-shared-fence` → `0.5.6-cuda-seh`:**
  - The entire per-frame CUDA body now runs inside an SEH trampoline (`SehCudaFrame`) with stage tags. A native access violation anywhere — input kernel, fence signal, queue wait, NGX evaluate, execute/wait, output kernel — becomes a clean error `Native access violation in CUDA zero-copy path at stage '<stage>'`, disables the CUDA path for this session, and the host retries the batch on CPU staging. ComfyUI is no longer killed by an escaping `OSError`.
  - A crash latch (`g_native_av_pending`) makes the retry call rebuild the whole session (`ReinitUnlocked`) before processing — the same self-heal the NGX-evaluate AV path already had; a clean frame clears it.
  - New optional diagnostic: `cuStreamGetCtx` verifies the caller's stream belongs to the bridge's primary context before any launch on it (mismatch → clean disable with an explicit error, eliminating a whole failure class).
  - Defensive null-init of `ImportTextureHeap` out-params so early failures never read driver garbage.
- **Python `0.9.15` → `0.9.16`:**
  - `gpu_mode` is now a real **BOOLEAN switch widget** (on = GPU ON 🟢, off = GPU OFF 🔴) instead of the two-option combo — per request. Old workflows that saved the v0.9.15 strings are normalized automatically (`_gpu_on()`); values from the `gpu_index` era fall back to on.
  - The CUDA→CPU fallback now catches `(DLSS5NRError, OSError)`: even if a native AV still escapes as an `OSError`, the batch retries on CPU staging instead of failing the run.
- **About the GPU OFF freeze:** after any native crash the process must be restarted (the v0.9.11 lesson) — that is exactly what the 20% hang was. With the new containment a CUDA-path AV no longer kills the run at all; but if you ever see "Windows fatal exception" again (e.g. from the CPU path), **RESTART ComfyUI** before the next run.
- **Copy `bridge/bin/dlss5nr_bridge.dll`, `OreX_DLSS5.py` and `js/OreX_DLSS5.js`, then RESTART ComfyUI** and hard-refresh the browser (Ctrl+Shift+R) — the widget type changed. Retest with GPU ON: expect `[DLSS5-Ox] path: CUDA zero-copy (pixels stay in VRAM, no PCIe traffic)` and all 241 frames completing. If a stage-tagged AV fires, the log will show `... at stage '<stage>'` followed by `path: CUDA zero-copy failed (...); retrying this batch on CPU staging`, and the run will finish via CPU staging — send that log, the stage name pinpoints exactly which driver call crashes.

## 0.9.15 - 2026-09-04

**The `CreateSharedHandle(fence) failed: 0x00000000` mystery is solved and fixed — plus `optical_flow` is removed from the options and `gpu_index` becomes a two-state toggle.** The v0.9.14 field log showed the fence share failing, but the error text printed `GetLastError()`, which COM methods do not set (always 0), so the real HRESULT was hidden. A new diagnostic probe (`bridge/tools/probe_fence_shared.cpp`, built by `bridge/tools/build_probe.ps1`) reproduces the exact call sequence on a live GPU without running any kernel and using ~64 KB of VRAM — safe even while the card is busy with an LLM. On the field RTX 5090 / driver 616.56 it found two driver requirements the bridge violated:

- **Fences must be created with `D3D12_FENCE_FLAG_SHARED`.** A fence made with `FLAG_NONE` is rejected by `CreateSharedHandle()` with E_INVALIDARG (0x80070057) for every access mask — that was the v0.9.14 failure. With the SHARED flag + `GENERIC_ALL` the share succeeds, and `cuImportExternalSemaphore(D3D12 fence)` on that NT handle also returns OK on this exact GPU/driver, validating the bridge's hand-written ABI structs end to end.
- **Heap sharing only accepts `GENERIC_ALL`.** `CreateSharedHandle(heap, GENERIC_READ|GENERIC_WRITE)` fails with E_INVALIDARG even on a `D3D12_HEAP_FLAG_SHARED` heap — and that was exactly the mask `ImportTextureHeap` used, so it would have been the very next crash after the fence fix.
- **Bridge fixes** (`0.5.4-cuda-semfix` → `0.5.5-shared-fence`): `g_fence` is now created with `D3D12_FENCE_FLAG_SHARED` (Signal/GetEvent/WaitForSingleObject semantics are unchanged, so the CPU-sync paths need no edits), heap shares use `GENERIC_ALL`, and both failure sites now print the real HRESULT plus the adapter name instead of `GetLastError()`.
- **`optical_flow` removed from `temporal_mode`** (per request — it never worked on this runtime). The NVOF code stays dormant; old workflows that saved `optical_flow` are clamped to `auto` with a log line instead of re-enabling the crashing path.
- **`gpu_index` INT replaced by the `gpu_mode` toggle**: `GPU ON 🟢` (default, = old 0 — bridge on GPU 0 + batch moved to VRAM if it fits) and `GPU OFF 🔴` (= old -1 — frames stay where they are). Old workflows that saved an int no longer match the key and fall back to the default. The RuntimeInfo node keeps its own INT `gpu_index` for per-GPU diagnostics.
- **Copy `bridge/bin/dlss5nr_bridge.dll`, `OreX_DLSS5.py` and `js/OreX_DLSS5.js`, then RESTART ComfyUI** (the bridge is loaded at node init) and hard-refresh the browser (Ctrl+Shift+R). Retest with `GPU ON 🟢`: the log line should now read `[DLSS5-Ox] path: CUDA zero-copy (pixels stay in VRAM, no PCIe traffic)` — or, if a later interop step still fails, it will print that step's own reason with the real HRESULT.

## 0.9.14 - 2026-09-04

**Bridge fix: CUDA interop now imports the D3D12 fence correctly — the zero-copy path can finally activate.** The v0.9.13 field log delivered the exact failure reason v0.9.12 was built to surface: `cuImportExternalSemaphore(D3D12 fence) failed: unknown error (0x000003E7)` (E_INVALIDARG).

- **Root cause**: the bridge passed the raw `ID3D12Fence*` COM pointer where cuda.h requires *"a shared NT handle referencing a D3D12 fence object"* — so the driver rejected the import with E_INVALIDARG on every GPU, and CUDA interop never worked in any release.
- **Fix** (bridge only, `dlss5nr_bridge.cpp`): create the NT handle via `ID3D12Device::CreateSharedHandle(fence, GENERIC_ALL)` — the fence itself has no such method, and per the D3D12 API docs `GENERIC_ALL` is the only accepted access value — pass that handle to `cuImportExternalSemaphore`, keep it alive for the lifetime of the imported semaphore, and close it in both cleanup paths (`ReleaseCudaExternal` on shutdown/size change, `CleanupCudaModule` on failed init).
- Bridge version `0.5.3-av-recovery` → `0.5.4-cuda-semfix`; the DLL is rebuilt. Python side: version bump only, no behavior change.
- **Copy `bridge/bin/dlss5nr_bridge.dll` and `OreX_DLSS5.py`, then RESTART ComfyUI** (the bridge is loaded at node init). Retest with `gpu_index=0`: the log line should now read `[DLSS5-Ox] path: CUDA zero-copy (pixels stay in VRAM, no PCIe traffic)` — or, if a later interop step fails, it will print that step's own reason instead.

## 0.9.13 - 2026-09-04

**`gpu_index` now also moves a CPU input batch into that GPU's VRAM before processing — the separate "move frames to VRAM" node is removed.** Field report (v0.9.11/0.9.12): with the ToVRAM node in the graph, 256 frames already took ~22.5 GiB of VRAM, so a 723-frame batch cannot fit on the card; the move belongs inside the NR node itself, behind its existing `gpu_index` widget, with an explicit off switch.

- `gpu_index >= 0` (default 0): if the input tensor is on CPU (or another GPU), the batch is moved to `cuda:gpu_index` before processing — this unlocks faster staging (one bulk pinned DMA instead of pageable transfers) and, once bridge CUDA interop works, the full zero-PCIe path.
- **Safety fit check**: the move is refused when input + output batches plus a 2 GiB margin do not fit the free VRAM (`torch.cuda.mem_get_info`). Peak usage holds both the input-res and the output-res batch at once (resampling allocates the second one, and the final upload / zero-copy output needs it too), so a blind `.to()` would OOM long sequences. Non-fitting batches stay in RAM with an explicit log line instead of crashing ComfyUI.
- `gpu_index == -1` (new minimum): disables the VRAM move entirely — frames stay where they are; the bridge then runs on GPU 0.
- The `DLSS5NRMToVRAM` node is removed (class, registration, display name). If your workflow uses it: delete the node and connect the video loader directly to the DLSS5 node.
- Python + JS only; no bridge rebuild (DLL unchanged, `0.5.3-av-recovery`). Copy `OreX_DLSS5.py`, `__init__.py` and `js/OreX_DLSS5.js`, then hard-refresh the browser (Ctrl+Shift+R).

## 0.9.12 - 2026-09-04

**The `CUDA interop unavailable in bridge` blocker now carries the bridge's own reason instead of a static string.** The bridge already records why CUDA interop init failed (`g_cuda_note`: nvcuda.dll missing, missing driver API exports, cuInit error, GPU name mismatch with the D3D12 device, …) and exposes it via `dlss5nr_cuda_status()` — but the main gate only ever asked the boolean `dlss5nr_cuda_supported()`, so every run logged the same unhelpful "unavailable". The gate now prints e.g. `CUDA interop unavailable in bridge (nvcuda.dll is missing required driver API exports …)`, so the next run on a clean machine tells us exactly which init step to fix — no separate Runtime Info node run needed.

- Python only; no bridge rebuild (DLL unchanged, `0.5.3-av-recovery`). Copy `OreX_DLSS5.py`.
- Field report confirming v0.9.11: optical_flow now fails cleanly on frame 0 (no hang), and after a ComfyUI restart `auto` completed all 723 frames in ~72 s with the per-64-frame heartbeats visible in the log.

## 0.9.11 - 2026-09-04

**Stops the post-crash hang: no more in-process retry after an `optical_flow` native crash, plus a watchdog that turns any wedged bridge call into a loud error instead of an infinite "pretending to work" freeze.** Field report (v0.9.10): after the first-frame AV at 1.5x the auto-retry started, then the run froze with zero CPU and GPU load while ComfyUI kept showing the node as running — the in-process session rebuild after the AV wedges a thread on a GPU fence.

- **The v0.9.10 auto-retry is removed.** An `optical_flow` first-frame native crash now fails the run with an explicit message: switch `temporal_mode` to `auto` (or `none`) and **restart ComfyUI** before the next run — the bridge session left in this process would trigger the same unreliable rebuild on its next frame.
- **Native-call watchdog (both paths).** Every `dlss5nr_process` / `dlss5nr_process_cuda` call now runs in a daemon worker with a 5-minute join timeout; if the bridge does not return, the node raises "the native session is wedged … RESTART ComfyUI" instead of hanging forever. A blocked ctypes call cannot be preempted, so this only makes the failure visible — the process still needs a restart.
- **Progress prints.** The staging and CUDA paths print `frame N/count` every 64 frames, and the staging path prints a marker before the final result upload — a future stall can be localized to a frame (or to the upload) instead of looking like a dead node. ComfyUI's own progress bar is per-node, not per-frame: "stopped at 17%" only meant our node was executing.
- **New node `DLSS5NRMToVRAM`** ("🕹️DLSS 5 NR: move frames to VRAM"): moves the frame batch to the GPU before the NR node. While ComfyUI's RAM-pressure cache or a loader keeps frames in system memory the zero-copy path is impossible; on a GPU with free VRAM this cuts the staging path's per-frame cost (uploads then come from page-locked memory after one bulk DMA) and, once bridge CUDA interop is active, unlocks the full zero-PCIe path.
- Python + JS only; no bridge rebuild (DLL unchanged, `0.5.3-av-recovery`). Copy `OreX_DLSS5.py`, `__init__.py` and `js/OreX_DLSS5.js`, then hard-refresh the browser (Ctrl+Shift+R).

## 0.9.10 - 2026-09-04

**`optical_flow` no longer fails the run on its known first-frame native crash — it now retries the whole batch in `auto` behavior (the fallback the v0.9.8 tooltip already promised but the code never implemented).** Field report: with a saved workflow still on `1x (DLAA / native)`, `optical_flow` died with `Frame 0: Native access violation inside DLSSNR evaluate`. The bridge's SEH trampoline survives the AV and rebuilds the session on the next call, so the node can now catch that specific error and re-run the batch without motion vectors.

- The CPU staging section was extracted into `_run_cpu_staging()` so the retry re-runs it from scratch with fresh buffers and a cold temporal state (the input tensor is never modified in place, so the retry always sees pristine frames).
- On fallback the console prints `[DLSS5-Ox] optical_flow: native crash (...); retrying the batch with temporal_mode=auto behavior.` — the video completes in auto mode instead of the node erroring out.
- The `input tensor is on CPU` blocker message now hints at the likely cause (upstream loader or ComfyUI's "RAM pressure cache" offload).
- Python only; no bridge rebuild (DLL unchanged, `0.5.3-av-recovery`). Copy `OreX_DLSS5.py`. Note: saved workflows keep their old `upscaling_mode` — if yours still says `1x (DLAA / native)`, set it to `1.5x (Quality)` explicitly; the v0.9.8 default only applies to newly created nodes.

## 0.9.9 - 2026-09-04

**Diagnostics: the node now prints which execution path it actually used on every run.** Field report: `auto`, `none` and `optical_flow` all consumed identical RAM/VRAM — the signature of every run going through the CPU staging path. That was invisible before: a missed CUDA gate (input on CPU, GPU index mismatch, interop unavailable) or a runtime CUDA failure fell back to CPU staging with **no log output at all**, so all three temporal modes looked exactly the same in memory usage and speed.

- Every run now prints one line to the ComfyUI console: `path: CUDA zero-copy (pixels stay in VRAM, no PCIe traffic)` when the v0.7.0 interop path runs, or `path: CPU staging — <reason>` with the exact blocker(s) (e.g. `input tensor is on CPU`, `input on GPU 1, bridge on GPU 0`, `optical_flow needs CPU frames (NVOF)`, `CUDA interop unavailable in bridge`).
- A CUDA-path runtime failure (which latches the bridge off for the whole process) now prints the error instead of silently retrying on CPU staging.
- Python only; no bridge rebuild (DLL unchanged, `0.5.3-av-recovery`). Copy `OreX_DLSS5.py` and run one sequence with `temporal_mode=auto` — the printed line tells us exactly which path runs on your machine and why, which decides the next step (fixing interop vs. the D3D12 NVOF migration for optical_flow).

## 0.9.8 - 2026-09-04

**New default: `upscaling_mode` = `1.5x (Quality)`; `temporal_mode` default confirmed as `auto`; tooltip clearing logic inverted so hints can no longer be over-killed.** Field report: `optical_flow` works with `1.5x (Quality)` — the first-frame native crash only happens at `1x` (native) — so 1.5x becomes the default upscaling mode for new nodes.

- **`upscaling_mode` default is now `1.5x (Quality)`.** Only affects newly created nodes; saved workflows keep their explicit value. The `temporal_mode` tooltip now notes that `optical_flow` is verified at 1.5x on current runtime builds, and the `upscaling_mode` tooltip marks 1.5x as the default.
- **Tooltip: "clear only when proven elsewhere".** The v0.9.7 safety net still over-cleared in some ComfyUI builds — `elementFromPoint` over a widget field returned an element that matched none of the keep-rules, so every micro-movement killed the pending 800 ms timer and no tooltip could appear. The document-level listener now resets state only with positive evidence the pointer left: the cursor is outside the window, on the graph canvas outside the node's screen rectangle, or over another node (`.lgraph-node`). Unknown elements (UI panels, overlays) keep the state — a box may linger briefly instead of never appearing; it clears as soon as the cursor reaches the graph or another node.
- Extra stuck-tooltip guards: LiteGraph `onMouseLeave` clears canvas-path hover when the pointer leaves the node in graph space, and `onDrawForeground` drops a visible box that has no live hover source (neither a DOM field nor a canvas widget).
- Python + JS only; no bridge rebuild (DLL unchanged, `0.5.3-av-recovery`). Copy `OreX_DLSS5.py` and `js/OreX_DLSS5.js`, then **hard-refresh the browser (Ctrl+Shift+R)** — ComfyUI may serve a cached copy of the old JS, which would make the fix look like it did nothing.

## 0.9.7 - 2026-09-04

**Fixes the v0.9.3 regression: parameter tooltips stopped appearing at all — and moves `preset` to the top of the node.** The v0.9.3 "stuck tooltip" safety net (a document-level pointermove listener that re-checks the pointer via elementFromPoint) was over-aggressive: widget fields are HTML elements above the canvas, and in this ComfyUI build their ancestor chain does not include the canvas element — so every micro-movement of the cursor over a field registered as "pointer left the node" and killed the pending 800 ms timer. No tooltip could ever appear.

- The safety net now clears the state only when the pointer is *provably* outside the node: it keeps the state while the point is over the hovered field (or its child), any of the node's widget fields, the node's DOM container (labels and gaps between fields), or the graph canvas inside the node's screen rectangle.
- The canvas hit-test path no longer clobbers an active DOM-field hover — in some builds both paths fire under a field and were racing each other, cancelling the timer that was about to show the box.
- **`preset` moved to the top of the node** (first widget row, above `upscaling_mode`). ComfyUI matches prompt keys by name, so saved workflows are unaffected by the reorder.
- JS + Python only; no bridge rebuild (DLL unchanged, `0.5.3-av-recovery`). Copy `js/OreX_DLSS5.js` and `OreX_DLSS5.py`.

## 0.9.6 - 2026-09-04

**Fixes the persistent crash after `optical_flow`: once any run used optical flow, every later prompt (even `auto`) failed with a native access violation until ComfyUI was restarted.** The v0.9.2 self-heal only rebuilt the session when the D3D12 device entered *removed* state — but this runtime's crash is a CPU-side access violation that leaves the device reporting healthy, so the poisoned closed-source core/snippet state survived across prompts and every later `EvaluateFeature` crashed at frame 0.

- **Any caught native AV now forces a full session rebuild.** The SEH trampolines set a bridge-level flag when they catch an access violation inside runtime code; the next call (the same frame's retry, or any later prompt in any mode) tears down and rebuilds the entire session — core shutdown, all three DLLs unloaded, fresh D3D12 device, fresh module load and init — which clears the poisoned static state. The flag is cleared after a successful frame.
- Behavior on an `optical_flow` run: the first frame may still crash once (that is the runtime's MVec-contract bug), the bridge rebuilds the session, latches optical flow off for the session with the one-time warning `optical flow disabled after native crash (...); bridge reinitialized, continuing in auto mode`, and continues the video in auto. **All later prompts — including `auto` — now work without a restart.**
- Bridge version is now `0.5.3-av-recovery`. Copy `bridge/bin/dlss5nr_bridge.dll` and `OreX_DLSS5.py`; after restarting ComfyUI, the Runtime Info node must show bridge version `0.5.3-av-recovery` (anything else means an old DLL is still loaded).

## 0.9.5 - 2026-09-04

**Renames the `motion` parameter to `temporal_mode`.** The old name collided with the motion-vector feature that only `optical_flow` uses; the control actually selects how the temporal history is managed, and the values (`none` / `auto` / `optical_flow`) are unchanged.

- Old saved workflows that carry a `motion` value simply ignore it: the key no longer exists in the node's inputs and ComfyUI drops unknown prompt keys, so the node falls back to the default `auto`. If you used `optical_flow`, re-select it under `temporal_mode` after loading such a workflow.
- Copy `OreX_DLSS5.py` and `js/OreX_DLSS5.js`; no bridge rebuild (DLL unchanged, `0.5.2-external-memory-nvof`).

## 0.9.4 - 2026-09-04

**Housekeeping: the `native/` folder is now `bridge/`, and `channel_order` is hidden (always `auto`).** No bridge rebuild — the DLL is unchanged (`0.5.2-external-memory-nvof`).

- **Folder rename `native/` → `bridge/`.** Bridge sources, build script and prebuilt DLL now live under `bridge/`: `bridge/bin/dlss5nr_bridge.dll`, `bridge/build_bridge.ps1` (was `native/build_native.ps1`), `bridge/dlss5nr_bridge.cpp`, `bridge/nvof_flow.*`, `bridge/caller_shim.cpp`, `bridge/cuda_interop.cu`, `bridge/cuda_kernels_ptx.h`. All path references in the Python loader, `.gitignore`, README.md, PROJECT.md and THIRD_PARTY_NOTICES.md are updated. **When updating your install: delete the old `native/` folder from `custom_nodes\ComfyUI-DLSS5-orex` and copy the new `bridge/` folder** — the DLL path moved to `bridge/bin/dlss5nr_bridge.dll`.
- **`channel_order` widget removed — the node always uses `auto` now.** The auto-detection (low-frequency colour statistics vs the source, picking RGBA or BGRA) has worked reliably on every tested build, so the manual override is gone from the node. Saved workflows that carried a `channel_order` value simply ignore it: the key no longer exists in the node's inputs, and ComfyUI drops unknown prompt keys.

## 0.9.3 - 2026-09-04

**Fixes the stuck tooltip: it could appear after the cursor had already left the parameter and then never close.** The 800 ms show-timer was not cancelled on every leave path — moving from a widget into an empty area of the node registered no state change at all, so a pending timer fired late and set the tooltip with nothing left to clear it.

- The hover state machine now tracks *which* widget/field the pointer is over (`dlss5HoverWidget` / `dlss5HoverEl`) and reacts only on enter/leave transitions: moving inside the same parameter no longer restarts the delay, and leaving it — even into an empty area of the node — cancels a pending show and hides a visible tooltip immediately.
- Both 800 ms timers re-validate at fire time (still over the same widget / field still in the DOM), so a tooltip can no longer appear after the cursor has left.
- New safety net: while a show is pending or a tooltip is visible, a temporary `document.pointermove` listener checks `elementFromPoint` and closes the state as soon as the pointer is anywhere else — empty graph, another node, UI panels, or outside the window (covers leaves that fire no DOM event at all). The listener detaches automatically when the state clears.
- Collapsing the node and removing it from the graph also clear any pending/visible tooltip.
- JS-only change (`js/OreX_DLSS5.js`); no bridge rebuild. Copy `js/OreX_DLSS5.js` and `OreX_DLSS5.py` (version bump).

## 0.9.2 - 2026-09-04

**Fixes the `optical_flow` crash and makes the bridge self-heal after it.** Field report on the 0.9.1 build: `auto`/`none` work, but the first `optical_flow` frame dies with a native `access violation writing 0x0` inside the runtime — and afterwards *every* mode fails until ComfyUI is restarted (the D3D12 device enters removed state after the crash).

- **The crash is now caught instead of fatal.** Every call into closed-source territory on the frame path — `EvaluateFeature`, `CreateFeature`, command-list Close/submit, queue drain, feature/parameter release, core shutdown — goes through a structured-exception guard. A native access violation becomes a clean frame error instead of taking down the whole ComfyUI process.
- **Self-healing session.** If a crash leaves the D3D12 device removed, the bridge detects it (`GetDeviceRemovedReason`) and rebuilds the entire session — device, NGX core, feature, CUDA interop, optical flow — from the stored init parameters, *inside the same frame call*. The failed frame is retried in auto mode, so the video completes with at most one slow frame; every later `auto`/`none`/`optical_flow` prompt works without a restart.
- **Optical flow latch.** A native crash while motion vectors are active latches optical flow off for the session (same as the clean-rejection path), with a distinct one-time warning: `[DLSS5-Ox] optical flow disabled after native crash (...); bridge reinitialized, continuing in auto mode`. The Runtime Info node shows it under `Optical flow state:`.
- Bridge version is now `0.5.2-external-memory-nvof` (shown by the Runtime Info node — anything else means an old DLL is still loaded). Python change is the version string only; copy `native/bin/dlss5nr_bridge.dll` and `OreX_DLSS5.py`.

## 0.9.1 - 2026-09-04

**Fixes the v0.9.0 regression: `Frame 0: CommandList::Close failed (0x80070057)` in both `auto` and `optical_flow`.** This runtime build rejects the very *presence* of the `DLSSNR.MVec` parameter key in the NGX table — v0.9.0 set it on every frame (to null in auto, to a real texture in optical_flow), so even `auto`, which never uses optical flow, broke on the first frame.

- **`auto`/`none` restored to the verified-working configuration.** With no motion input the bridge now sets only `DLSSNR.MVecScaleX/Y = 1.0` and never touches the MVec resource or subrect keys — exactly what v0.8.4 did, and what NVIDIA's own CLI diagnostic log shows for a motionless run.
- **Parameter-table lifecycle.** The parameter object is now allocated per feature rebuild and destroyed with the feature via `NVSDK_NGX_D3D12_DestroyParameters` (optional export; on cores without it one small object leaks per rebuild). Stale keys from a previous mode/size session can no longer survive.
- **`optical_flow` degrades gracefully instead of throwing.** If a frame fails while motion vectors are active (typically the first-frame Close rejection), the bridge latches optical flow off for the session, rebuilds the feature in auto mode, and retries the *same* frame — the video completes with a one-time log warning: `[DLSS5-Ox] optical flow disabled after frame error (...); continuing in auto mode`. If a future driver/runtime build accepts MVec, this path never triggers and you get real motion vectors.
- **Command-list hygiene.** A `Close` failure now recycles the command list before returning, so a retry starts from a clean list instead of re-recording on top of rejected commands.
- The Runtime Info node reports a new `Optical flow state:` line (`active` or `disabled (reason)`).
- New bridge export: `dlss5nr_motion_status`. Bridge version is now `0.5.1-external-memory-nvof`.

## 0.9.0 - 2026-09-04

**`motion=optical_flow` now feeds real motion vectors.** Previously `optical_flow` was an alias of `auto` (scene-change detection only). Now, for sequences of 2+ frames, the node runs **NVIDIA Optical Flow** on each raw input frame and passes the resulting flow into the runtime's `DLSSNR.MVec` parameter.

- The flow comes from `nvofapi64.dll`, which ships inside your NVIDIA display driver — **no extra file to install**. If the driver does not provide the NVOF API, the node prints a warning and falls back to `auto` behavior; everything else keeps working.
- How it works: each RGB frame is converted to luma (Rec.709) and fed to NVOF (grid 2, performance level FAST) on a private D3D11 device created on the same GPU as the bridge's D3D12 device. The flow comes back as S10.5 fixed-point displacements at half resolution (ceil(w/2)×ceil(h/2)); the bridge nearest-upsamples it to full resolution, converts it to normalized UVs, and writes an R16G16_FLOAT texture that is passed to `DLSSNR.MVec` with `MVecScaleX/Y = width/height`.
- The first frame of a run — and every frame after a scene cut — carries **zero motion vectors**, because NVOF needs two frames to produce flow. Scene-change detection via `scene_change_threshold` is unchanged; it now also resets the optical-flow ping-pong buffers, so a cut never feeds stale flow into the new scene.
- This mode runs on the CPU staging path (the CUDA zero-PCIe path does not carry motion vectors). `auto` and `none` are untouched and keep using the fast path when available.
- The Runtime Info node now reports NVIDIA Optical Flow availability (`available (grid 2, perf level 20 = FAST)` or `NOT FOUND in driver`).
- New bridge exports: `dlss5nr_nvof_available`, `dlss5nr_nvof_grid`, `dlss5nr_nvof_perf`. Bridge version is now `0.5.0-external-memory-nvof`.
- The optical-flow code is ported from the MIT-licensed lisitskyaa/ComfyUI-DLSS5-NR project; see THIRD_PARTY_NOTICES.md for full attribution.

## 0.8.4 - 2026-09-04

**Root cause found and fixed.** The 0.8.3 diagnostics pinpointed it: the failure line for 896×1152 at `upscaling_mode = 1.5x` showed `w=1344 h=1728 ... alloc=0 align=1 heap=19070976`. Two facts in one line:

- At 1.5x the bridge allocates frame textures at the *target* resolution (1344×1728), not the input size — so "896×1152 failing" was always really 1344×1728 failing, which also retroactively explains round 3's "regression".
- `GetResourceAllocationInfo` returns `{0, 1}` on this GPU/driver (RTX 5090, 616.56), so the 0.8.2/0.8.3 sizing from it was silently inert — the heap fell back to the copyable total only.

The true rule, verified against **all 17 observed sizes from every round**: the driver places `LAYOUT_UNKNOWN` textures with the height rounded up to a multiple of **128 rows** (placement = pitch × ⌈h/128⌉ × 128). Every failure had h % 128 ≠ 0, every success h % 128 == 0 — including the identical-copyable-total pair 768×1344 (fail) vs 896×1152 (pass), and 1344×1728 needing exactly 294 × 64 KB where the 0.8.3 ladder maxed out at 290.

- The placement is now estimated directly as `pitch × ceil(h/128) × 128` and included in the heap-size max (exact-multiple heights keep their previous size, so nothing that worked changes).
- The retry ladder remains as insurance, extended from 7 to 31 growth steps (+2 MB).
- Bridge version is now `0.4.4-external-memory`. No Python changes other than the version bump.

## 0.8.3 - 2026-09-04

Makes frame allocation **self-healing** and makes any remaining failure fully diagnosable, after field data where a previously *working* resolution (896×1152) failed on a later run with the same `CreatePlacedResource ... 0x80070057`. If the 0.8.2 build were loaded, that regression is impossible — its heap is provably at least as large as the one that worked before. So either an older build was still being loaded from the sibling node folder, or the driver's reported footprints are not a reliable upper bound on this GPU/driver. Instead of guessing which: allocation now grows until it fits, and any failure reports every number involved.

- **Retry ladder:** if `CreatePlacedResource` rejects the placement (E_INVALIDARG), the shared heap is grown by 64 KB and retried, up to 7 times. The output texture pair starts from whatever size the color pair proved, so both pairs always end at a working size.
- Sizing now also accounts for `GetResourceAllocationInfo`'s **Alignment**: heap = max(copyable total, data end, `SizeInBytes` rounded up to its Alignment), then rounded up to 64 KB.
- **Full diagnostics in the failure message:** HRESULT plus `w/h/pitch/offset/copyable/alloc/align/heap` — one failed run now reveals exactly which invariant broke, no more inference from resolution lists.
- Bridge version is now `0.4.3-external-memory`. The Runtime Info node must show this string after the update; anything else means an old DLL is still being loaded (two node folders exist: `ComfyUI-DLSS5` and `ComfyUI-DLSS5-orex`).
- No Python changes other than the version bump.

## 0.8.2 - 2026-09-04

Makes frame-resource allocation robust for **any** resolution, and makes it trivial to verify which bridge build is actually loaded.

- The shared heap is now sized from `ID3D12Device::GetResourceAllocationInfo` — the driver's true placement footprint for the texture — instead of trusting the `GetCopyableFootprints` total alone. If the driver's placement pitch ever differs from its copy pitch, the old sizing could still be too small even after the 64 KB rounding; the max of both values (still rounded up to 64 KB) closes that gap.
- Field data confirming the sizing hole (two rounds): errors now carry an HRESULT (`0x80070057` = E_INVALIDARG), which pre-0.8.1 messages lacked, so a 0.8.1-era build was loaded; every resolution whose footprint total is an exact multiple of 64 KB worked while all half-integer totals failed — and 768×1344 (exactly 126 × 64 KB) failed where 896×1152 with the *identical* total succeeded. Identical copyable totals with opposite outcomes prove the driver's placement footprint can exceed the `GetCopyableFootprints` total for some shapes — precisely the gap this entry closes. The per-build version string also lets the Runtime Info node show exactly which DLL is loaded (two node folders exist: `ComfyUI-DLSS5` and `ComfyUI-DLSS5-orex`).
- Bridge version is now `0.4.2-external-memory`. No Python changes other than the version bump.

## 0.8.1 - 2026-09-04

Fixes a size-dependent failure in the v0.8.0 interop path: `Frame 0: CreatePlacedResource(color) failed` for videos whose resolution produced an unaligned shared-heap size (reported with 544×704, while 768×768 worked).

- Root cause: `D3D12_HEAP_FLAG_SHARED` heap sizes must be a multiple of 64 KB (`D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT`), but `GetCopyableFootprints` is heap-agnostic and returns unaligned totals for many common resolutions (1080p, 720p and 4K would all have failed; 1440p happened to be aligned).
- Fix: the frame-heap size is rounded up to 64 KB before `CreateHeap` (the texture still sits at the driver's footprint offset inside it, and CUDA imports the full rounded heap). The CPU staging path is unaffected.
- Diagnostics: `CreateHeap`/`CreatePlacedResource` failures now include the actual HRESULT in the error message.
- Bridge version is now `0.4.1-external-memory`. No Python changes other than the version bump.

## 0.8.0 - 2026-09-04

**The CUDA/D3D12 interop path actually works now.** The `cuGraphicsD3D12RegisterResource` function the v0.7.0 bridge relied on does not exist in the CUDA driver API (verified against `nvcuda.dll` 616.56 with dumpbin), so the zero-PCIe path never activated and every frame silently used the CPU staging fallback. The bridge is rewritten on the real external-memory mechanism:

- **External memory interop.** Frame textures are placed on explicit `D3D12_HEAP_FLAG_SHARED` heaps, exported with `ID3D12Device::CreateSharedHandle`, and imported into this process's CUDA context via `cuImportExternalMemory` (`CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP`). The mapped device pointers are persistent — no per-frame map/unmap.
- **GPU-side handoff.** The D3D12 fence is wrapped once as an external semaphore (`cuImportExternalSemaphore`, type D3D12_FENCE). Per frame: PTX input kernel → `cuSignalExternalSemaphoresAsync` → the D3D12 command queue waits on the fence value **on the GPU** (no CPU round-trip) → NGX evaluation → `cuWaitExternalSemaphoresAsync` on the post-eval fence value → PTX output kernel, left async on the caller's stream. This removes one CPU↔GPU synchronization compared to the v0.7.0 design.
- **Audited NGX parameter types** against reference implementations (Zonnery/dlss5-nr-player): every `SetCommonParams` type is already correct (including `UICorrection=0`, `DepthInverted=1`, `ScalingRatio=1.0f`, `MVecScaleX/Y=1.0f`) — no behavior change; the audit confirms the current values.
- Same 8-export ABI; no widget or signature changes, saved workflows load as-is. Fallback behavior is unchanged: any interop failure retries the whole batch on the CPU staging path with forced history resets, and `dlss5nr_cuda_status` reports the reason. Bridge version is now `0.4.0-external-memory`.

## 0.7.0 - 2026-09-04

New native bridge with CUDA/D3D12 external-memory interop — the structural optimization that v0.6.0 left as a roadmap item:

- **Zero-PCIe pixel path.** When the input tensor already sits on the bridge's own GPU, pixels never cross PCIe: the bridge maps its D3D12 frame textures into this process's CUDA context, converts RGB(f32) ↔ RGBA16F with small embedded PTX kernels launched on the caller's stream, runs NGX inference in between, and writes each frame directly into a slot of the output tensor. Per-frame CPU↔GPU traffic is reduced to raw pointers + one stream handle (plus the tiny scene-detection luma arrays).
- No CUDA toolkit needed to build or run: `nvcuda.dll` is loaded dynamically at runtime and the conversion PTX (compute_75 floor = Turing, JITed by the driver) is embedded in the DLL. If `nvcuda.dll` is missing, the GPU names do not match, or interop init fails, `dlss5nr_cuda_supported()` reports 0 and the node transparently falls back to the v0.6.0 CPU staging path; a mid-batch CUDA failure retries the whole batch on the CPU path with forced history resets (NGX temporal state is unknown after a failed frame).
- New bridge exports: `dlss5nr_process_cuda`, `dlss5nr_cuda_supported`, `dlss5nr_cuda_status`. The Runtime Info node now reports `CUDA interop: active (zero-PCIe path)` or the fallback reason.
- Channel-order `auto` detection runs fully on-GPU in the CUDA path (strided low-frequency comparison, same policy as the CPU path).
- Fixed a crash (`NameError`) on frame 0 of any multi-frame video with `motion=auto/optical_flow`: `prev_gray` was referenced before initialization.
- Bridge sources are back in the working tree: `native/dlss5nr_bridge.cpp`, `native/caller_shim.cpp`, `native/build_native.ps1`, `native/cuda_interop.cu`, `native/cuda_kernels_ptx.h`. Rebuilding needs only MSVC + Windows SDK (no NGX headers, no CUDA toolkit): `powershell -ExecutionPolicy Bypass -File native\build_native.ps1`.
- No widget or signature changes; saved workflows load as-is.

> **Correction (v0.8.0):** the interop described here was built on `cuGraphicsD3D12RegisterResource`, which does not exist in the CUDA driver API — the zero-PCIe path never activated and all frames silently ran through the CPU staging fallback. v0.8.0 rewrites the mechanism on external memory + external semaphores; see the entry above.

## 0.6.0 - 2026-09-03

Performance (Python side only — the native bridge is unchanged):

- >1x upscaling modes now resample (bicubic, antialiased) on the **source device** before the single batch download instead of running the filter on a CPU copy; largest win for 1.5x–3x modes.
- Scene-change detection (luma + area downsample to ≤256 px edge) now runs on the source device once per batch; only the tiny per-frame arrays cross to CPU for thresholding, removing 3–4 full-resolution numpy passes per frame.
- Both batch copies (GPU→CPU download, CPU→GPU upload) use cached page-locked staging buffers (`_pinned_buffer`, 4 GiB cap, per-shape eviction) so CUDA DMA runs at full bandwidth; automatic fallback to pageable copies on allocation failure or cap overflow.
- No widget, signature, or output-behavior changes; saved workflows load as-is.

## 0.5.1 - 2026-09-03

- Removed the built-in plain ComfyUI tooltip that rendered on top of the OreX-style box: all `"tooltip"` fields are gone from `INPUT_TYPES` (widget descriptions now live only in `js/OreX_DLSS5.js`, which matches widgets by name); the JS keeps a defensive `w.tooltip`/`title` clearing sweep as backup.
- Project cleanup — deleted files not needed for operation: GitHub publishing material (`.github/workflows/`, `tools/package_release.ps1`, `tools/check_no_proprietary_binaries.py`, `pyproject.toml.example`, `CONTRIBUTING.md`, `SECURITY.md`), bridge sources and build script (`native/dlss5nr_bridge.cpp`, `native/caller_shim.cpp`, `native/build_native.ps1`), diagnostics (`diagnose_runtime.bat`, `diagnose_runtime.ps1`, `tools/check_channel_order.py`) and the whole `docs/` folder. Everything except the untracked `check_channel_order.py` remains recoverable from git history.
- Added `PROJECT.md` — a single detailed orientation document for future AI assistants/contributors (architecture, ctypes API surface, full parameter table with measured behavior, tooltip system, legal notes); folds in the useful content of the former `docs/` files.

## 0.5.0 - 2026-09-03

- Renamed the node module to `OreX_DLSS5.py` and the tooltip extension to `js/OreX_DLSS5.js` (one file per node, OreX convention). Internal class keys (`DLSS5NeuralRendering`, `DLSS5NRRuntimeInfo`) are unchanged, so saved workflows keep loading.
- Display names changed: the main node is now **🕹️DLSS 5 (OreX)**, the runtime info node is **🕹️DLSS 5 NR Runtime Info (OreX)**.
- Tooltip border switched from green to the signature OreX blue (`rgba(0, 150, 255, 0.7)`), matching `OreXImageLoadBatch.js`.
- Supplemented widget tooltips with the reference web interface's parameter labels: upscaling uses NVIDIA's fixed DLSS modes (DLAA keeps source resolution); model preset — Default lets NVIDIA select mode-specific presets, J/K/L/M forces one for every scaling mode; NR preset is an experimental content-dependent hint (Default recommended); tone = local tone and contrast enhancement; structure = local detail and texture structure; skin `-1.00` is the native default; auto_mask is an experimental runtime-generated mask that may cause flicker or inconsistent results.
- Fixed release packaging: `tools/package_release.ps1` now stages `js/OreX_DLSS5.js` (previously missing, so release ZIPs shipped without tooltips) and references the renamed module.

## 0.4.0 - 2026-09-02

- Added `dlss_model_preset` (`Default`, `J`, `K`, `L`, `M`, default `M`) directly below `upscaling_mode`, mirroring the reference DLSS 5 enhancer. **Reserved control:** the current feature-18 runtime exposes no model-preset parameter in its NGX parameter table (verified against the `nvngx_dlssnr.dll` string table: 61 `DLSSNR.*` parameters, none for model selection), so the value is validated but does not change output until a runtime build exposes model selection.
- Changed `preset` from an integer (0-3) to a combo list (`Default`, `Preset #1..#3`, default `Default`) like the reference repo. Old saved workflows with a numeric preset still execute (int 0-3 fallback). Note: the reference measurements report identical output at every preset value on current builds, so the new `Default` (0) behaves the same as the old default (3).
- Added tooltips to every widget (`upscaling_mode`, `dlss_model_preset`, `image`, `style`, `preset`, `intensity`, `tone`, `structure`, `skin`, `auto_mask`, `motion`, `scene_change_threshold`, `warmup_frames`, `gpu_index`, `channel_order`), adapted from the reference repo's tooltips with this bridge's honest caveats.
- Added a canvas tooltip extension (`js/dlss5nr_tooltip.js`, registered via `WEB_DIRECTORY`) in the OreX style: hover a widget for 800 ms and a dark box with a green border appears to the right of the node with an icon + name title, an EN line and an RU line; long descriptions wrap.
- Widget order changed again (`dlss_model_preset` inserted below `upscaling_mode`); saved workflows from v0.3.0 need their widget values re-checked after loading.

## 0.3.0 - 2026-09-02

- Added `upscaling_mode` as the first widget with the full mode list (`1x (DLAA / native)`, `1.5x (Quality)`, `1.724x (Balanced)`, `2x (Performance)`, `3x (Ultra Performance)`). 1x runs at native resolution; larger modes resample first and then run the neural pass — this bridge has no DLSS Super Resolution models.
- Replaced `batch_mode` with `motion` (`auto`, `optical_flow`, `none`) controlling temporal history: `none` resets only the first frame, `auto`/`optical_flow` reset on detected scene changes (mean luminance difference). The bridge has no motion-vector input, so `optical_flow` behaves like `auto`.
- Added `scene_change_threshold` (0.01..1.0, default 0.24) and `warmup_frames` (0..16, extra re-evaluations after each history reset).
- Removed experimental numeric styles 3-6 from the style list.
- Added a tooltip to `auto_mask` explaining that it is the mask for `skin` (skin_structure_strength).
- Widget order changed; saved workflows from v0.2.0 need their widget values re-checked after loading.
- Removed redundant `build_native.bat` wrappers; the builder is `native\build_native.ps1` (used by CI and releases).
- Added `tools/check_channel_order.py` diagnostic for determining the channel order of a specific runtime build.

## 0.2.0 - 2026-09-01

- Prepared repository for public GitHub distribution.
- Added GitHub Actions Windows CI and tagged-release packaging.
- Normal users can use prebuilt project-owned DLLs from release ZIPs; MSVC is developer-only.
- Added safeguards against accidentally bundling NVIDIA proprietary runtime DLLs.
- Improved DriverStore discovery to use the actual Windows directory instead of a hard-coded `C:\Windows` path.
- Build script now searches both Program Files roots, including full Visual Studio installations used by GitHub Actions.
- Runtime Info now reports selected GPU and SHA-256 of the user-supplied Neural Rendering runtime.
- Retained `channel_order = auto | RGBA | BGRA` compatibility handling.
- Added third-party notices, legal-release notes and publishing documentation.

## 0.1.10

- Preserved widget ordering for old workflows after adding `channel_order`.
