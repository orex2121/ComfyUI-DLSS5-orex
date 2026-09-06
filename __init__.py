# SPDX-License-Identifier: MIT
import time

import server
from aiohttp import web

from . import OreX_DLSS5 as _ox
from .OreX_DLSS5 import (
    DLSS5NeuralRendering,
    __version__,
)
from .OreX_DLSS5_video import DLSS5NeuralRenderingVideo

NODE_CLASS_MAPPINGS = {
    "DLSS5NeuralRendering": DLSS5NeuralRendering,
    "DLSS5NeuralRenderingVideo": DLSS5NeuralRenderingVideo,
}

NODE_DISPLAY_NAME_MAPPINGS = {
    "DLSS5NeuralRendering": "🕹️DLSS 5 (OreX)",
    "DLSS5NeuralRenderingVideo": "🕹️DLSS 5 Video (OreX)",
}

WEB_DIRECTORY = "./js"

__all__ = ["NODE_CLASS_MAPPINGS", "NODE_DISPLAY_NAME_MAPPINGS", "__version__", "WEB_DIRECTORY"]


# ---------------------------------------------------------------------------
# Frame preview API (v0.9.27).
#
# The v0.9.19 presets API (GET/POST /dlss5/presets + the presets.json
# auto-creation at server start) was removed in v0.9.26 — the preset feature
# left the node itself back in v0.9.22 and only dead backend code remained;
# presets.json is no longer created or read anywhere.
#
# Preview flow (v0.9.27, reworded v0.9.28): the JS button first POSTs
# /dlss5/preview-arm, then queues ONLY the subgraph feeding the DLSS5 node —
# the prompt is pruned client-side to that node + its upstream (the rgthree-
# comfy "Queue Selected Output Nodes" pattern) and sent via api.queuePrompt.
# Everything upstream runs exactly like a full run; downstream nodes are not
# in the pruned prompt, so no outputs are written. When the DLSS5 node's
# process() executes it sees the fresh arm and processes only the change_frame
# slice of its input batch (clamped to the last frame when out of range),
# stashing the PNG in _ox._preview_result (or the failure text in
# _ox._preview_error). The JS polls /dlss5/preview-status until done/error and
# fetches the PNG from /dlss5/preview-result. The state lives in OreX_DLSS5.py
# and is mutated through module attributes — a `from X import y` rebind here
# would not reach the node code. An arm expires after _ox._PREVIEW_ARM_TTL so a
# dead run cannot turn a later ordinary run into a preview.
# ---------------------------------------------------------------------------

@server.PromptServer.instance.routes.post("/dlss5/preview-arm")
async def dlss5_preview_arm(request):
    """Arm single-frame preview capture for the next run of the DLSS5 node."""
    _ox._preview_result = None
    _ox._preview_error = None
    _ox._preview_arm_ts = time.time()
    return web.json_response({"ok": True})


@server.PromptServer.instance.routes.post("/dlss5/preview-disarm")
async def dlss5_preview_disarm(request):
    """Drop a pending arm (the JS calls this when its preview flow fails)."""
    _ox._preview_arm_ts = 0.0
    return web.json_response({"ok": True})


@server.PromptServer.instance.routes.get("/dlss5/preview-status")
async def dlss5_preview_status(request):
    """Poll state: idle / armed / done / error (error carries the message)."""
    if _ox._preview_result is not None:
        return web.json_response({"state": "done"})
    if _ox._preview_error is not None:
        return web.json_response({"state": "error", "error": _ox._preview_error})
    if _ox._preview_arm_ts and (time.time() - _ox._preview_arm_ts) < _ox._PREVIEW_ARM_TTL:
        return web.json_response({"state": "armed"})
    return web.json_response({"state": "idle"})


@server.PromptServer.instance.routes.get("/dlss5/preview-result")
async def dlss5_preview_result(request):
    """Fetch (and consume) the rendered preview frame as PNG."""
    if _ox._preview_error is not None:
        err = _ox._preview_error
        _ox._preview_error = None
        return web.json_response({"ok": False, "error": err}, status=409)
    res = _ox._preview_result
    if res is not None:
        _ox._preview_result = None
        return web.Response(
            body=res["png"], content_type="image/png",
            headers={"X-DLSS5-Frame": str(res["frame"])})
    return web.json_response({"ok": False, "error": "no preview result yet"}, status=409)
