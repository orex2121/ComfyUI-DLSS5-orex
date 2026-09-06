// Tooltips + Preview/Hide controls for the 🕹️DLSS 5 Video (OreX) node
// (DLSS5NeuralRenderingVideo, OreX_DLSS5_video.py). A visual copy of
// js/OreX_DLSS5.js re-keyed to the video node: hover a widget for 800 ms and
// a dark box with the signature blue border appears to the right of the node —
// icon + name title, an EN line and an RU line. Long descriptions wrap instead
// of stretching one line across the screen. All module-level identifiers are
// namespaced with a trailing v (DLSS5V_* / _dlss5v*) so this file can coexist
// with js/OreX_DLSS5.js in the same WEB_DIRECTORY without colliding, and it
// only attaches to nodeData.name === "DLSS5NeuralRenderingVideo"[cite: 6].

// ---------------------------------------------------------------------------
// Node controls (DLSS5NeuralRenderingVideo only):
//   * gpu_mode stays a NATIVE BOOLEAN widget — styled purely via the
//     Python-side label_on/label_off ("🟢 GPU ON" / "🔴 GPU OFF"), exactly
//     like the OreX LM Studio nodes; no DOM overlay over its row (v0.9.20:
//     the v0.9.19 JS switch hid the native row and did not render in all
//     builds, so it was removed);
//   * v0.9.27 (reworked v0.9.28): under the change_frame widget two buttons
//     are drawn ON CANVAS in one row — Preview and Hide (no DOM element: the
//     v0.9.19–v0.9.21 DOM button did not appear in all builds). Preview arms
//     the server (POST /dlss5/preview-arm) and then queues ONLY the subgraph
//     feeding this node — the rgthree-comfy "Queue Selected Output Nodes"
//     pattern: app.graphToPrompt() is pruned to this node + everything linked
//     into it, and that dict goes through api.queuePrompt. Upstream runs
//     exactly like a full run, the node processes only the change_frame frame
//     (an index beyond the end of the sequence is clamped to the last frame),
//     and downstream nodes are not in the prompt at all — no outputs written.
//     The JS polls /dlss5/preview-status until the result is ready, fails fast
//     on the execution_error websocket event (matched by prompt_id) and draws
//     the PNG from /dlss5/preview-result under the buttons with the signature
//     blue border and a "frame N" label; Hide removes it. computeSize is
//     patched per node type so the extra rows are never clipped[cite: 6].
// ---------------------------------------------------------------------------

// v0.9.26 Preview/Hide layout constants (node-local px)[cite: 6].
const DLSS5V_PREVIEW_MARGIN = 10;
const DLSS5V_BTN_H = 24;
const DLSS5V_BTN_GAP_X = 8;
const DLSS5V_ROW_GAP = 6;
const DLSS5V_IMG_GAP = 6;
const DLSS5V_MAX_IMG_H = 480;

// Scaled size of the current preview image for a node (null when not shown or
// not loaded yet) — width follows the node, height capped[cite: 6].
function _dlss5vPreviewImageSize(node) {
    const img = node.dlss5PreviewImage;
    if (!node || !node.dlss5PreviewShown || !img || !img.complete
            || !img.naturalWidth || !img.naturalHeight) return null;
    let w = node.size[0] - 2 * DLSS5V_PREVIEW_MARGIN;
    let h = img.naturalHeight * (w / img.naturalWidth);
    if (h > DLSS5V_MAX_IMG_H) {
        h = DLSS5V_MAX_IMG_H;
        w = img.naturalWidth * (DLSS5V_MAX_IMG_H / img.naturalHeight);
    }
    return { w, h };
}

// Extra node height the canvas-drawn controls need below the change_frame row:
// always the button row, plus the preview image when one is shown. Used by the
// computeSize patch in _dlss5vApplyTooltipToNodeType[cite: 6].
function _dlss5vPreviewExtraHeight(node) {
    if (!node || node.comfyClass !== "DLSS5NeuralRenderingVideo"
            || (node.flags && node.flags.collapsed)) return 0;
    const frameW = node.widgets ? node.widgets.find(w => w.name === "change_frame") : null;
    if (!frameW) return 0;
    let h = DLSS5V_ROW_GAP + DLSS5V_BTN_H;
    const img = _dlss5vPreviewImageSize(node);
    if (img) h += DLSS5V_IMG_GAP + img.h;
    return h;
}

// v0.9.28: rgthree-comfy "Queue Selected Output Nodes" pruning pattern
// (their recursiveAddNodes): keep the node and everything that feeds it. In
// a ComfyUI prompt dict, linked inputs are [upstreamNodeId, slot] arrays;
// widget values are scalars or {__value__: [...]} wrappers, so a plain array
// marks a link. The dst[id] != null guard also cuts cycles and dedupes[cite: 6].
function _dlss5vAddUpstream(id, src, dst) {
    if (dst[id] != null) return;
    const entry = src[id];
    if (!entry) return; // defensive: link to a node missing from the prompt
    dst[id] = entry;
    const inputs = entry.inputs || {};
    for (const v of Object.values(inputs)) {
        if (Array.isArray(v) && v.length >= 1) _dlss5vAddUpstream(String(v[0]), src, dst);
    }
}

// Prefer ComfyUI's api.fetchApi (it honors --api-base) when the global is
// available, fall back to a same-origin fetch — this file intentionally uses
// no imports (the 'app' global can appear late in some builds).
// The frontend app object, captured in _dlss5vRegister — runPreview needs
// app.graphToPrompt() + app.api.queuePrompt to queue the pruned subgraph
// (v0.9.27, pruning added in v0.9.28)[cite: 6].
let _dlss5vApp = null;

async function _dlss5vFetchApi(path, options) {
    try {
        if (typeof api !== "undefined" && api && typeof api.fetchApi === "function") {
            return await api.fetchApi(path, options);
        }
    } catch (_) { /* fall through to plain fetch */ }
    return fetch(path, options);
}

const DLSS5V_HELP_DESCRIPTIONS = [
    { name: 'upscaling_mode', icon: '🔍',
        desc: "Output size relative to the source, using NVIDIA's fixed DLSS modes. 1x runs Neural Rendering at native resolution (DLAA keeps the source resolution); larger modes resample the frame first, then run the neural pass at the target size. This bridge has no DLSS Super Resolution models. Default since v0.9.8: 1.5x (Quality).",
        ru_desc: 'Размер вывода относительно исходника, фиксированные режимы DLSS от NVIDIA. 1x запускает нейрорендеринг в нативном разрешении (DLAA сохраняет исходное разрешение); большие режимы сначала ресемплируют кадр, затем прогоняют нейронный проход в целевом размере. В этом мосту нет моделей DLSS Super Resolution. По умолчанию с v0.9.8 — 1.5x (Quality).' },
    { name: 'dlss_model_preset', icon: '🧠',
        desc: "Default lets NVIDIA select its normal mode-specific presets; J, K, L or M forces that model preset for every scaling mode. Reference enhancer measurements: Default, J and K are the softest; L and M reconstruct markedly more skin and hair texture. Reserved: the current feature-18 runtime exposes no model-preset parameter, so this control does not change output yet.",
        ru_desc: 'Default — NVIDIA сама выбирает пресеты для каждого режима; J, K, L или M принудительно задаёт эту модель во всех режимах масштабирования. По измерениям референсного энхансера: Default, J и K — самые мягкие; L и M заметно лучше восстанавливают текстуру кожи и волос. Зарезервировано: текущий runtime feature-18 не имеет параметра выбора модели, поэтому опция пока не влияет на результат.' },
    { name: 'style', icon: '🎨',
        desc: 'Look of the neural pass: natural stays closer to the source, cinematic is stronger, default leaves it to the model.',
        ru_desc: 'Характер нейронного прохода: natural ближе к исходнику, cinematic сильнее, default оставляет выбор модели.' },
    { name: 'intensity', icon: '🎚️',
        desc: 'Overall strength of the neural rendering pass. Measured on current runtime builds, values above 1.0 have no further effect; use below 1.0 to blend back towards the source.',
        ru_desc: 'Общая сила нейронного прохода. На актуальных сборках значения выше 1.0 не дают дополнительного эффекта; ниже 1.0 — сдвиг к исходнику.' },
    { name: 'tone', icon: '🌗',
        desc: 'Local tone and contrast enhancement (DLSSNR.LocalToneStrength).',
        ru_desc: 'Усиление локального тона и контраста (DLSSNR.LocalToneStrength).' },
    { name: 'structure', icon: '🧵',
        desc: 'Local detail and texture structure reconstruction (DLSSNR.LocalStructureStrength). Higher keeps more texture; 1.5 is a good balance for AI generated footage.',
        ru_desc: 'Восстановление локальных деталей и текстуры (DLSSNR.LocalStructureStrength). Выше — больше текстуры; 1.5 — хороший баланс для AI-генерации.' },
    { name: 'skin', icon: '🧑',
        desc: 'Skin-specific structure and pore reconstruction (DLSSNR.SkinStructureStrength). Requires auto_mask, which tells the model where skin is; with the mask off this control does nothing. -1.00 is the native default and leaves it to the model.',
        ru_desc: 'Структура, специфичная для кожи, и восстановление пор (DLSSNR.SkinStructureStrength). Требует auto_mask — он говорит модели, где кожа; без маски опция не работает. -1.00 — нативное значение по умолчанию, оставляет выбор модели.' },
    { name: 'auto_mask', icon: '🎭',
        desc: 'Experimental runtime-generated mask that changes where Neural Rendering is applied; it may cause flicker or inconsistent results. Also the gate for the skin control: with the mask off, skin does nothing.',
        ru_desc: 'Экспериментальная маска, генерируемая в рантайме: меняет области применения нейрорендеринга; может вызывать мерцание или нестабильный результат. Также это выключатель для опции skin: без маски skin не работает.' },
    { name: 'temporal_mode', icon: '🎞️',
        desc: 'Temporal history management. none keeps history across all frames and resets only the first one. auto detects scene changes via scene_change_threshold and resets history there (fast path, no motion vectors). optical_flow was removed in v0.9.15 — on current runtime builds it crashed natively on the first frame; old workflows that saved it are clamped to auto automatically.',
        ru_desc: 'Управление временной историей. none держит историю для всех кадров и сбрасывает только первый. auto детектирует смену сцены по scene_change_threshold и сбрасывает историю там (быстрый путь, без motion vectors). optical_flow удалён в v0.9.15 — на актуальных сборках рантайма он падал нативно на первом кадре; старые схемы, где он был сохранён, автоматически переходят на auto.' },
    { name: 'scene_change_threshold', icon: '✂️',
        desc: 'Mean luminance change between consecutive frames (0..1) above which the temporal history is reset.',
        ru_desc: 'Среднее изменение яркости между кадрами (0..1), выше которого сбрасывается временная история.' },
    { name: 'warmup_frames', icon: '⏳',
        desc: 'After each history reset, re-evaluate the frame this many extra times so temporal accumulation settles before its output is kept.',
        ru_desc: 'После каждого сброса истории кадр пересчитывается ещё столько раз, чтобы временная аккумуляция успела стабилизироваться.' },
    { name: 'gpu_mode', icon: '🎮',
        desc: 'Switch. On = 🟢 GPU ON: the bridge runs on the first NVIDIA GPU (nvidia-smi order) and the node also moves a CPU input batch into its VRAM before processing (faster staging and, when CUDA interop is active, the zero-PCIe path) — but only if batch + output fit the free VRAM; long sequences that do not fit stay in RAM with a log message. Off = 🔴 GPU OFF: frames stay where they are (no VRAM move), the bridge still runs on the first GPU. Old workflows that saved the v0.9.15 combo strings are normalized automatically.',
        ru_desc: 'Переключатель. Вкл = 🟢 GPU ON: мост работает на первой NVIDIA GPU (порядок nvidia-smi), а узел дополнительно переносит входной батч с CPU в её VRAM перед обработкой (быстрый стажинг и, когда активен CUDA interop, нулевой PCIe-путь) — но только если батч + выход помещаются в свободную VRAM; длинные последовательности, которые не помещаются, остаются в RAM с сообщением в логе. Выкл = 🔴 GPU OFF: кадры остаются там, где были (без переноса в VRAM), мост при этом работает на первой карте. Старые схемы со строками v0.9.15 нормализуются автоматически.' },
    { name: 'chunk_frames', icon: '🧩',
        desc: "v0.9.23: memory-saving mode for long videos. 0 = off (the whole batch is processed at once, as before). >0 = process that many frames per chunk and assemble the result into a single full-length tensor. Frames keep their order on the same bridge session, so temporal history carries across chunk boundaries exactly as in an unchunked run — no quality loss at the seams. Peak RAM/VRAM drops from the whole batch to one chunk (plus the output tensor, which must exist anyway); sequences that do not fit VRAM are processed through pinned per-chunk staging instead of one giant buffer. Rule of thumb: pick N so that N × out_h × out_w × 12 bytes ≈ 1–2 GiB (e.g. 32 for 1080p→1.5x).",
        ru_desc: 'v0.9.23: режим экономии памяти для длинных видео. 0 = выкл (весь батч обрабатывается разом, как раньше). >0 = обрабатывать по N кадров за чанк и собирать результат в единый тензор полной длины. Кадры идут по порядку в той же сессии моста, поэтому временная история переносится через границы чанков так же, как без чанков — потери качества на стыках нет. Пиковые RAM/VRAM падают со всего батча до одного чанка (плюс тензор вывода, который нужен в любом случае); последовательности, не влезающие в VRAM, идут через пин-стажинг по чанкам вместо одного гигантского буфера. Ориентир: N × out_h × out_w × 12 байт ≈ 1–2 GiB (например, 32 для 1080p→1.5x).' },
    { name: 'change_frame', icon: '🎯',
        desc: "v0.9.27: frame index for the Preview button below (0 = first frame of the sequence). Preview runs the graph up to this node and processes only this one frame — temporal history starts fresh at it, so it is a fast parameter check, not a slice of the finished video. If the index is beyond the end of the sequence, the last frame is used instead.",
        ru_desc: 'v0.9.27: индекс кадра для кнопки Preview ниже (0 = первый кадр секвенции). Preview запускает схему до этого узла и обрабатывает только этот один кадр — временная история начинается с него, так что это быстрая проверка параметров, а не кусок готового видео. Если индекс больше длины секвенции, берётся последний кадр.' }
];

const DLSS5V_TOOLTIP_MAX_TEXT_WIDTH = 420;

function _dlss5vWrapText(ctx, text, maxWidth) {
    const words = String(text).split(" ");
    const lines = [];
    let line = "";
    for (const word of words) {
        const candidate = line ? line + " " + word : word;
        if (!line || ctx.measureText(candidate).width <= maxWidth) {
            line = candidate;
        } else {
            lines.push(line);
            line = word;
        }
    }
    if (line) lines.push(line);
    return lines.length ? lines : [""];
}

function _dlss5vBuildTooltipRows(ctx, item, maxTextW) {
    const rows = [];
    const pushBlock = (text, font, color, lineH, gapAfter) => {
        const lines = _dlss5vWrapText(ctx, text, maxTextW);
        lines.forEach((line, i) => {
            rows.push({
                text: line,
                font,
                color,
                h: lineH,
                gapAfter: (i === lines.length - 1) ? gapAfter : 0
            });
        });
    };
    pushBlock(`${item.icon || "💡"} ${item.name}`, "bold 13px Arial, sans-serif", "#ffffff", 17, 6);
    pushBlock(`EN: ${item.desc}`, "11px Arial, sans-serif", "#cccccc", 15, 4);
    pushBlock(`RU: ${item.ru_desc}`, "11px Arial, sans-serif", "#999999", 15, 0);
    return rows;
}

function _dlss5vApplyTooltipToNodeType(nodeType) {

        // Всплывающая подсказка при наведении на виджет в компактном виде узла:
        // хит-тест по прямоугольнику виджета (w.last_y/computeSize), задержка
        // перед показом, отрисовка прямо на канвасе узла.
        nodeType.prototype.onMouseMove = function (e, pos) {
            const [mx, my] = pos;

            // The DOM path owns the state while the pointer is over a widget
            // field: in some builds the canvas also receives mousemove under
            // HTML fields, and its last_y-based hit-test is not authoritative
            // there — clobbering would kill the pending timer / visible box[cite: 6].
            if (this.dlss5HoverEl && this.dlss5HoverEl.isConnected) return;

            let hoveredWidget = null;
            if (mx >= 0 && mx <= this.size[0] && my >= 0 && my <= this.size[1] && this.widgets) {
                for (const w of this.widgets) {
                    if (!w || w.last_y === undefined || w.hidden) continue;
                    const wy = w.last_y;
                    const wh = w.computeSize ? w.computeSize(this.size[0])[1] : 24;
                    if (mx >= 10 && mx <= this.size[0] - 10 && my >= wy && my <= wy + wh) {
                        hoveredWidget = w;
                        break;
                    }
                }
            }

            // Реагируем только на смену «наведённого» виджета: движения внутри
            // того же виджета не перезапускают задержку, а выход из него (даже в
            // пустую область узла) мгновенно отменяет ожидание и прячет подсказку.
            if (hoveredWidget === this.dlss5HoverWidget) return;
            this.dlss5ClearTooltipState();
            this.dlss5HoverWidget = hoveredWidget;
            if (!hoveredWidget) return;

            const tooltipInfo = DLSS5V_HELP_DESCRIPTIONS.find(item => item.name === (hoveredWidget.name || "").trim());
            if (!tooltipInfo) return;
            const widgetY = hoveredWidget.last_y;
            this.dlss5HoverTimer = setTimeout(() => {
                // За 800 мс курсор мог покинуть виджет — тогда подсказка не показывается.
                if (this.dlss5HoverWidget !== hoveredWidget) return;
                this.dlss5ActiveTooltip = tooltipInfo;
                this.dlss5ActiveTooltipY = widgetY;
                this.setDirtyCanvas(true);
                this.dlss5HoverTimer = null;
            }, 800);
            this._dlss5vEnsureDocVerify();
        };

        // ComfyUI рисует поля виджетов как HTML-элементы поверх канваса, поэтому
        // canvas-onMouseMove над ними не срабатывает: слушаем pointerenter /
        // pointerleave прямо на inputEl (тот же приём, что в OreXImageLoadBatch.js).
        // w.tooltip и title чистим, чтобы встроенная «голая» подсказка ComfyUI
        // больше не показывалась — остаётся только синяя рамка с EN/RU[cite: 6].
        nodeType.prototype.onDrawForeground = function (ctx) {
            // Late setup: nodes created before the extension registered never
            // receive nodeCreated — set up their controls on the first draw.
            if (this.comfyClass === "DLSS5NeuralRenderingVideo" && !this.dlss5ControlsReady) {
                try { _dlss5vSetupNodeControls(this); } catch (_) {}
            }
            if (this.flags?.collapsed) { this.dlss5ClearTooltipState(); return; }

            if (this.widgets) {
                for (const w of this.widgets) {
                    if (!w) continue;
                    if (w.tooltip) w.tooltip = null;
                    const el = w.inputEl;
                    if (el && !el._dlss5vTooltipBound) {
                        el._dlss5vTooltipBound = true;
                        if (el.title) el.title = "";
                        if (el.getAttribute("title")) el.removeAttribute("title");
                        el.addEventListener("pointerenter", () => {
                            // Курсор вошёл в DOM-поле — канвас-хиттест больше не
                            // актуален, поэтому состояние сбрасываем всегда, даже
                            // если у этого поля нет описания (иначе отложенный
                            // таймер канвас-пути сработает уже «не там»).
                            this.dlss5ClearTooltipState();
                            const norm = s => String(s || "").toLowerCase().trim().replace(/_/g, " ");
                            const hwName = norm(w.name);
                            const hwLabel = norm(w.label);
                            const tooltipInfo = DLSS5V_HELP_DESCRIPTIONS.find(item => {
                                const itemName = norm(item.name);
                                return hwName === itemName || hwLabel === itemName;
                            });
                            if (!tooltipInfo) return;
                            this.dlss5HoverEl = el;
                            const widgetY = w.last_y !== undefined ? w.last_y : 50;
                            this.dlss5HoverTimer = setTimeout(() => {
                                // pointerleave мог не сработать (например, элемент
                                // пересоздан под неподвижным курсором) — показываем
                                // только если это поле всё ещё наведено и в DOM.
                                if (this.dlss5HoverEl !== el || !el.isConnected) return;
                                this.dlss5ActiveTooltip = tooltipInfo;
                                this.dlss5ActiveTooltipY = widgetY;
                                this.setDirtyCanvas(true);
                                this.dlss5HoverTimer = null;
                            }, 800);
                            this._dlss5vEnsureDocVerify();
                        });
                        el.addEventListener("pointerleave", () => {
                            if (this.dlss5HoverEl !== el) return;
                            this.dlss5HoverEl = null;
                            this.dlss5ClearTooltipState();
                        });
                    }
                }
            }

            // Страховка от «зависшей» подсказки: видимый бокс без живого
            // источника hover (ни DOM-поле, ни канвас-виджет) — сбрасываем.
            if (this.dlss5ActiveTooltip && !this.dlss5HoverEl && !this.dlss5HoverWidget) {
                this.dlss5ClearTooltipState();
            }

            if (this.dlss5ActiveTooltip) this._dlss5vDrawTooltip(ctx);
        };

        nodeType.prototype.dlss5ClearTooltipState = function () {
            if (this.dlss5HoverTimer) { clearTimeout(this.dlss5HoverTimer); this.dlss5HoverTimer = null; }
            this.dlss5HoverWidget = null;
            this.dlss5HoverEl = null;
            if (this.dlss5ActiveTooltip) {
                this.dlss5ActiveTooltip = null;
                this.dlss5ActiveTooltipY = null;
                this.setDirtyCanvas(true);
            }
            this._dlss5vStopDocVerify();
        };

        // Страховка от «зависшей» подсказки, которая НЕ убивает показ: пока ждём
        // показ или подсказка видна, слушаем document.pointermove и сбрасываем
        // состояние ТОЛЬКО при доказательстве, что курсор ушёл в другое место —
        // за окно, в пустой граф вне прямоугольника узла или на другой узел[cite: 6].
        // Неопределённые случаи (UI-панели, оверлеи, чужие слои DOM) держим:
        // в некоторых сборках ComfyUI elementFromPoint над полем виджета возвращает
        // элемент, не совпадающий ни с одним «нашим» правилом, и старая логика
        // «закрой, если не доказано, что над узлом» убивала таймер при каждом
        // микродвижении — подсказки показывались вообще никогда. Лучше бокс
        // задержится на пару секунд, чем не появляться: как только курсор вернётся
        // на граф или на другой узел, сброс произойдёт. Слушатель живёт только пока
        // есть ожидание/видимый бокс.
        nodeType.prototype._dlss5vEnsureDocVerify = function () {
            if (this.dlss5DocMoveHandler) return;
            this.dlss5DocMoveHandler = (ev) => {
                try {
                    if (!this.dlss5HoverTimer && !this.dlss5ActiveTooltip) {
                        this._dlss5vStopDocVerify();
                        return;
                    }
                    const top = document.elementFromPoint(ev.clientX, ev.clientY);
                    if (!top) { this.dlss5ClearTooltipState(); return; } // курсор за окном

                    // Над нашим параметром (наведённое поле или его потомок) — держим.
                    if (this.dlss5HoverEl && (top === this.dlss5HoverEl || this.dlss5HoverEl.contains(top))) return;
                    // Любое место внутри DOM-слоя нашего узла (подписи, зазоры) — держим.
                    try { if (this._dom_element && this._dom_element.contains(top)) return; } catch (_) {}
                    // Над любым полем виджетов этого узла — держим.
                    if (this.widgets) {
                        for (const w of this.widgets) {
                            const el = w && w.inputEl;
                            if (el && (top === el || el.contains(top))) return;
                        }
                    }
                    // Над графом: внутри экранного прямоугольника узла — держим,
                    // вне — курсор ушёл в пустоту, сбрасываем.
                    const canvasEl = this._dlss5vGetCanvasEl();
                    if (canvasEl) {
                        let g = top;
                        while (g) { if (g === canvasEl) break; g = g.parentElement; }
                        if (g === canvasEl) {
                            const rect = this._dlss5vNodeScreenRect();
                            if (!rect || (ev.clientX >= rect.x && ev.clientX <= rect.x + rect.w &&
                                          ev.clientY >= rect.y && ev.clientY <= rect.y + rect.h)) return;
                            this.dlss5ClearTooltipState();
                            return;
                        }
                    }
                    // Доказательно над ДРУГИМ узлом — сбрасываем.
                    if (typeof top.closest === "function") {
                        const otherNodeEl = top.closest(".lgraph-node");
                        if (otherNodeEl && otherNodeEl !== this._dom_element) {
                            this.dlss5ClearTooltipState();
                            return;
                        }
                    }
                    // Неопределённо — держим (см. комментарий выше).
                } catch (_) { /* не критично */ }
            };
            document.addEventListener("pointermove", this.dlss5DocMoveHandler);
            // Курсор ушёл за окно — pointermove внутри документа больше не придёт,
            // закрываем сразу, а не ждём следующего движения.
            if (!this.dlss5DocLeaveHandler) {
                this.dlss5DocLeaveHandler = (ev) => {
                    if (!ev.relatedTarget) this.dlss5ClearTooltipState();
                };
                document.documentElement.addEventListener("pointerleave", this.dlss5DocLeaveHandler);
            }
        };

        nodeType.prototype._dlss5vStopDocVerify = function () {
            if (this.dlss5DocMoveHandler) {
                document.removeEventListener("pointermove", this.dlss5DocMoveHandler);
                this.dlss5DocMoveHandler = null;
            }
            if (this.dlss5DocLeaveHandler) {
                document.documentElement.removeEventListener("pointerleave", this.dlss5DocLeaveHandler);
                this.dlss5DocLeaveHandler = null;
            }
        };

        nodeType.prototype._dlss5vGetCanvasEl = function () {
            try {
                return (this.graph && this.graph._canvas) ||
                    (typeof app !== "undefined" && app && app.canvas && app.canvas.canvas) || null;
            } catch (_) { return null; }
        };

        // Экранный прямоугольник узла в CSS-пикселях вьюпорта. LiteGraph рисует
        // граф как screen = offset + pos * scale, поэтому нода переводится так же;
        // если трансформ недоступен — null и проверка просто пропускается[cite: 6].
        nodeType.prototype._dlss5vNodeScreenRect = function () {
            try {
                const canvasEl = this._dlss5vGetCanvasEl();
                if (!canvasEl || !this.pos || !this.size) return null;
                const view = (typeof app !== "undefined" && app && app.canvas) ? app.canvas : null;
                // Без надёжного трансформа прямоугольник был бы неверным —
                // лучше пропустить проверку, чем закрыть подсказку ложно.
                if (!view || typeof view.scale !== "number" || !Array.isArray(view.offset)) return null;
                const scale = view.scale;
                const offset = view.offset;
                const r = canvasEl.getBoundingClientRect();
                return {
                    x: r.left + offset[0] + this.pos[0] * scale,
                    y: r.top + offset[1] + this.pos[1] * scale,
                    w: this.size[0] * scale,
                    h: this.size[1] * scale
                };
            } catch (_) { return null; }
        };

        // Узел удалён из графа — снимаем слушатель и сбрасываем состояние.
        const _dlss5vPrevOnRemoved = nodeType.prototype.onRemoved;
        nodeType.prototype.onRemoved = function () {
            if (_dlss5vPrevOnRemoved) _dlss5vPrevOnRemoved.call(this);
            this.dlss5ClearTooltipState();
        };

        // LiteGraph сообщает, что курсор покинул узел в координатах графа —
        // состояние канвас-пути устарело. DOM-путь закрывается сам через
        // pointerleave на поле; сюда доходит только «чисто канвасовый» hover[cite: 6].
        const _dlss5vPrevOnMouseLeave = nodeType.prototype.onMouseLeave;
        nodeType.prototype.onMouseLeave = function (e) {
            if (_dlss5vPrevOnMouseLeave) _dlss5vPrevOnMouseLeave.call(this);
            if (!this.dlss5HoverEl) this.dlss5ClearTooltipState();
        };

        nodeType.prototype._dlss5vDrawTooltip = function (ctx) {
            if (!this.dlss5ActiveTooltip) return;
            const item = this.dlss5ActiveTooltip;
            const wy = this.dlss5ActiveTooltipY !== null ? this.dlss5ActiveTooltipY : 100;
            const margin = 12;
            const bx = this.size[0] + 25;

            ctx.save();
            const rows = _dlss5vBuildTooltipRows(ctx, item, DLSS5V_TOOLTIP_MAX_TEXT_WIDTH);

            let boxW = margin * 2;
            let contentH = 0;
            for (const row of rows) {
                ctx.font = row.font;
                const w = ctx.measureText(row.text).width;
                if (w + margin * 2 > boxW) boxW = w + margin * 2;
                contentH += row.h + row.gapAfter;
            }
            const boxH = margin + contentH + 8;
            const by = wy + 12 - boxH / 2;

            ctx.fillStyle = "rgba(18, 18, 18, 0.98)";
            ctx.strokeStyle = "rgba(0, 150, 255, 0.7)"; // Фирменный синий
            ctx.lineWidth = 1.5;
            ctx.shadowColor = "rgba(0, 0, 0, 0.8)";
            ctx.shadowBlur = 10;
            ctx.shadowOffsetY = 4;

            const r = 6, arrowW = 8, arrowH = 6, arrowTipY = boxH / 2;

            ctx.beginPath();
            ctx.moveTo(bx + r, by);
            ctx.lineTo(bx + boxW - r, by);
            ctx.arcTo(bx + boxW, by, bx + boxW, by + r, r);
            ctx.lineTo(bx + boxW, by + boxH - r);
            ctx.arcTo(bx + boxW, by + boxH, bx + boxW - r, by + boxH, r);
            ctx.lineTo(bx + r, by + boxH);
            ctx.arcTo(bx, by + boxH, bx, by + boxH - r, r);
            ctx.lineTo(bx, by + arrowTipY + arrowH);
            ctx.lineTo(bx - arrowW, by + arrowTipY);
            ctx.lineTo(bx, by + arrowTipY - arrowH);
            ctx.lineTo(bx, by + r);
            ctx.arcTo(bx, by, bx + r, by, r);
            ctx.closePath();

            ctx.fill();
            ctx.shadowColor = "transparent";
            ctx.stroke();

            ctx.textBaseline = "top";
            ctx.textAlign = "left";

            let y = by + margin;
            for (const row of rows) {
                ctx.font = row.font;
                ctx.fillStyle = row.color;
                ctx.fillText(row.text, bx + margin, y);
                y += row.h + row.gapAfter;
            }

            ctx.restore();
        };

        // v0.9.26: reserve canvas space under the last widget for the
        // Preview/Hide button row and (when shown) the preview image — they are
        // drawn in onDrawForeground by _dlss5vSetupNodeControls, so the node
        // size must include them or they get clipped[cite: 6].
        const _dlss5vPrevComputeSize = nodeType.prototype.computeSize;
        nodeType.prototype.computeSize = function (width, height) {
            const s = _dlss5vPrevComputeSize ? _dlss5vPrevComputeSize.call(this, width, height) : [320, 160];
            if (this.comfyClass === "DLSS5NeuralRenderingVideo") {
                try { s[1] += _dlss5vPreviewExtraHeight(this); } catch (_) {}
            }
            return s;
        };
}

// Per-node controls for DLSS5NeuralRenderingVideo (v0.9.26): two buttons drawn ON
// CANVAS in one row under the change_frame widget — Preview and Hide. No DOM
// element is created, so they render wherever the node itself renders (the
// v0.9.19–v0.9.21 DOM button did not appear in all builds). gpu_mode stays a
// plain native BOOLEAN widget styled by label_on/label_off in Python ("🟢 GPU
// ON" / "🔴 GPU OFF"), like the OreX LM Studio nodes — nothing here touches
// its row. Preview (v0.9.27, reworked v0.9.28) arms the server with POST
// /dlss5/preview-arm and then queues ONLY the subgraph feeding this node —
// the prompt is pruned to it + its upstream (the rgthree-comfy "Queue
// Selected Output Nodes" pattern), so downstream nodes are absent and no
// outputs are written. Everything upstream runs exactly like a full run, and
// this node processes only the change_frame frame (an out-of-range index is
// clamped to the last frame). The
// JS polls /dlss5/preview-status until the result is ready, fetches the PNG
// from /dlss5/preview-result and draws it under the buttons with the signature
// blue border and a "frame N" label; Hide removes it. Clicks are hit-tested in
// onMouseDown, hover sets a pointer cursor, and the per-type computeSize patch
// reserves the extra height so nothing is clipped[cite: 6].
function _dlss5vSetupNodeControls(node) {
    if (!node || node.comfyClass !== "DLSS5NeuralRenderingVideo") return;
    if (node.dlss5ControlsReady) return;

    const frameW = node.widgets ? node.widgets.find(w => w.name === "change_frame") : null;
    if (!frameW) {
        console.warn("[OreX_DLSS5_video] change_frame widget not found — Preview/Hide controls not set up");
        return;
    }

    // Row layout in node-local coordinates: the two button rectangles plus the
    // (null when hidden) preview image size. Recomputed on every draw so the
    // row follows node resizes and the change_frame row position[cite: 6].
    const layout = () => {
        if (node.flags && node.flags.collapsed) return null;
        if (frameW.last_y === undefined) return null;
        const rowH = frameW.computeSize ? frameW.computeSize(node.size[0])[1] : 20;
        const x = DLSS5V_PREVIEW_MARGIN;
        const y = frameW.last_y + rowH + DLSS5V_ROW_GAP;
        const w = Math.max(72, (node.size[0] - 2 * DLSS5V_PREVIEW_MARGIN - DLSS5V_BTN_GAP_X) / 2);
        return { x, y, w, h: DLSS5V_BTN_H, img: _dlss5vPreviewImageSize(node) };
    };

    // Hit-test against the rectangles stored by drawBtnRow on the last draw
    // (with a small tolerance so edge clicks still land).
    const inBtn = (px, py, id) => {
        if (typeof px !== "number" || typeof py !== "number") return false;
        const r = node.dlss5BtnRects && node.dlss5BtnRects[id];
        if (!r) return false;
        return px >= r.x - 2 && px <= r.x + r.w + 2 && py >= r.y - 2 && py <= r.y + r.h + 2;
    };

    // --- flash feedback on the button row (green = ok, red = error) ---
    let flash = null;        // {status: "ok"|"err", until: number}
    const setFlash = (status) => {
        flash = { status, until: Date.now() + 900 };
        node.setDirtyCanvas(true);
        setTimeout(() => node.setDirtyCanvas(true), 950); // one more draw to revert the color
    };

    // Re-run the patched computeSize and apply it: showing/hiding the preview
    // image changes the reserved height, so the node must resize with it.
    const refreshSize = () => {
        try {
            const s = node.computeSize(node.size);
            if (s && Array.isArray(s) && s.length >= 2) {
                node.setSize([s[0], s[1]]);
            }
        } catch (_) { /* cosmetic only */ }
        node.setDirtyCanvas(true, true);
    };

    // --- Preview (v0.9.28): arm the server, queue the PRUNED subgraph, draw the frame ---
    // No cached batch from a previous run: (1) POST /dlss5/preview-arm makes
    // the next process() of this node type process only the change_frame slice
    // and stash its PNG; (2) app.graphToPrompt() is pruned to THIS node + its
    // upstream (the rgthree-comfy "Queue Selected Output Nodes" pattern) and
    // that dict is queued through api.queuePrompt — everything upstream runs
    // exactly like a full run, downstream nodes are not in the prompt at all,
    // so no outputs are written; (3) poll /dlss5/preview-status until done/
    // error (failing fast on the execution_error websocket event), then fetch
    // the PNG from /dlss5/preview-result and draw it under the buttons[cite: 6].
    const runPreview = async () => {
        if (node.dlss5PreviewBusy) return;
        const app = _dlss5vApp;
        if (!app || !app.graphToPrompt || !app.api || typeof app.api.queuePrompt !== "function") {
            setFlash("err");
            console.error("[OreX_DLSS5_video] preview: frontend 'app' unavailable — cannot queue the graph");
            return;
        }
        node.dlss5PreviewBusy = true;
        node.setDirtyCanvas(true); // repaint with the "Preview…" label
        let url = null;
        let execFailed = null;
        let promptId = null;
        const onExecError = (e) => {
            // New frontends dispatch a CustomEvent (payload in .detail);
            // classic builds pass the payload object directly[cite: 6].
            const d = (e && e.detail) || e || {};
            if (promptId && d.prompt_id && String(d.prompt_id) !== String(promptId)) return;
            execFailed = d.exception_message || d.message || "execution error";
        };
        const removeExecError = () => {
            try {
                for (const ev of ["execution_error", "exec_error"]) {
                    if (app.api.removeEventListener) app.api.removeEventListener(ev, onExecError);
                    else if (app.api.removeEventLister) app.api.removeEventLister(ev, onExecError);
                }
            } catch (_) { /* cosmetic only */ }
        };
        try {
            // 1. Arm single-frame capture for the next run of this node type.
            const armResp = await _dlss5vFetchApi("/dlss5/preview-arm", { method: "POST" });
            if (!armResp.ok) throw new Error("preview-arm failed (HTTP " + armResp.status + ")");

            // 2. Build the prompt and prune it to this node + its upstream.
            //    New frontends return a wrapper {workflow, output}; classic
            //    builds return the node dict itself — handle both shapes[cite: 6].
            const full = await app.graphToPrompt();
            const isWrapper = !!(full && typeof full === "object"
                && full.output && typeof full.output === "object");
            const src = isWrapper ? full.output : full;
            const pruned = {};
            _dlss5vAddUpstream(String(node.id), src, pruned);
            if (!pruned[String(node.id)]) {
                throw new Error("this node is not in the prompt — is it muted or bypassed?");
            }
            const payload = isWrapper ? { output: pruned, workflow: full.workflow } : pruned;

            // 3. Queue the pruned subgraph — upstream nodes run normally, this
            //    node processes only the change_frame frame (clamped server-
            //    side), downstream nodes are absent from the prompt entirely.
            const resp = await app.api.queuePrompt(0, payload);
            promptId = resp && resp.prompt_id ? String(resp.prompt_id) : null;

            // 4. Wait for this node's result; fail fast on an execution error.
            app.api.addEventListener("execution_error", onExecError);
            try { app.api.addEventListener("exec_error", onExecError); } catch (_) {}
            const deadline = Date.now() + 10 * 60 * 1000; // matches the server arm TTL[cite: 6]
            let state = "armed";
            while (Date.now() < deadline) {
                await new Promise(r => setTimeout(r, 700));
                if (execFailed) break;
                const sResp = await _dlss5vFetchApi("/dlss5/preview-status");
                if (!sResp.ok) continue;
                state = (await sResp.json()).state || "idle";
                if (state === "done" || state === "error") break;
            }
            removeExecError();

            if (execFailed) throw new Error(execFailed);
            if (state === "error") {
                const sResp = await _dlss5vFetchApi("/dlss5/preview-status");
                const j = await sResp.json().catch(() => ({}));
                throw new Error(j.error || "the preview run failed");
            }
            if (state !== "done") {
                // The arm expired without this node producing a frame — the
                // run hung or took longer than the 10-minute arm TTL.
                throw new Error("no preview result within 10 minutes — the run may have hung or taken too long");
            }

            // 5. Fetch the PNG and draw it under the buttons[cite: 6].
            const pngResp = await _dlss5vFetchApi("/dlss5/preview-result");
            if (!pngResp.ok) {
                const data = await pngResp.json().catch(() => ({}));
                throw new Error(data.error || ("HTTP " + pngResp.status));
            }
            const usedFrame = pngResp.headers.get("X-DLSS5-Frame");
            url = URL.createObjectURL(await pngResp.blob());
            const img = await new Promise((resolve, reject) => {
                const im = new Image();
                im.onload = () => resolve(im);
                im.onerror = () => reject(new Error("failed to decode the preview image"));
                im.src = url;
            });
            if (node.dlss5PreviewUrl) URL.revokeObjectURL(node.dlss5PreviewUrl);
            node.dlss5PreviewImage = img;
            node.dlss5PreviewUrl = url;
            url = null; // ownership moved to the node
            node.dlss5PreviewFrame = usedFrame !== null ? Number(usedFrame) : null;
            node.dlss5PreviewShown = true;
            setFlash("ok");
            refreshSize();
        } catch (err) {
            if (url) URL.revokeObjectURL(url);
            // Drop a stale arm so it cannot hijack the next ordinary run[cite: 6].
            try { await _dlss5vFetchApi("/dlss5/preview-disarm", { method: "POST" }); } catch (_) {}
            setFlash("err");
            console.error("[OreX_DLSS5_video] preview failed:", err);
        } finally {
            removeExecError();
            node.dlss5PreviewBusy = false;
            node.setDirtyCanvas(true);
        }
    };

    // --- Hide: drop the current preview image and shrink the node back ---
    const hidePreview = () => {
        if (node.dlss5PreviewUrl) URL.revokeObjectURL(node.dlss5PreviewUrl);
        node.dlss5PreviewImage = null;
        node.dlss5PreviewUrl = null;
        node.dlss5PreviewFrame = null;
        node.dlss5PreviewShown = false;
        refreshSize();
    };

    // --- drawing: rounded-rect path helper, the button row, the image ---
    const roundRectPath = (ctx, x, y, w, h, r) => {
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.lineTo(x + w - r, y);
        ctx.arcTo(x + w, y, x + w, y + r, r);
        ctx.lineTo(x + w, y + h - r);
        ctx.arcTo(x + w, y + h, x + w - r, y + h, r);
        ctx.lineTo(x + r, y + h);
        ctx.arcTo(x, y + h, x, y + h - r, r);
        ctx.lineTo(x, y + r);
        ctx.arcTo(x, y, x + r, y, r);
        ctx.closePath();
    };

    const drawBtnRow = (ctx) => {
        node.dlss5BtnRects = {}; // hit-test rects for this draw pass
        const L = layout();
        if (!L) return;
        const items = [
            { id: "preview", label: node.dlss5PreviewBusy ? "Preview…" : "Preview" },
            { id: "hide", label: "Hide" }
        ];
        let x = L.x;
        ctx.save();
        for (const it of items) {
            const r = { x, y: L.y, w: L.w, h: L.h };
            node.dlss5BtnRects[it.id] = r;
            let bg = "#2a2a2a", border = "#555555", fg = "#cccccc";
            if (flash && Date.now() < flash.until) {
                if (flash.status === "ok") { bg = "#2e7d32"; border = "#4CAF50"; fg = "#ffffff"; }
                else { bg = "#b71c1c"; border = "#f44336"; fg = "#ffffff"; }
            } else if (node.dlss5BtnHover === it.id) {
                bg = "#333333"; border = "rgba(0, 150, 255, 0.9)"; fg = "#ffffff";
            }
            roundRectPath(ctx, r.x, r.y, r.w, r.h, 5);
            ctx.fillStyle = bg;
            ctx.fill();
            ctx.lineWidth = 1;
            ctx.strokeStyle = border;
            ctx.stroke();
            ctx.fillStyle = fg;
            ctx.font = "12px Arial, sans-serif";
            ctx.textAlign = "center";
            ctx.textBaseline = "middle";
            ctx.fillText(it.label, r.x + r.w / 2, r.y + r.h / 2 + 1);
            x += L.w + DLSS5V_BTN_GAP_X;
        }
        ctx.restore();
    };

    const drawPreviewImage = (ctx) => {
        const L = layout();
        if (!L || !L.img) return;
        const img = node.dlss5PreviewImage;
        const x = L.x;
        const y = L.y + L.h + DLSS5V_IMG_GAP;
        ctx.save();
        ctx.fillStyle = "#000000";
        ctx.fillRect(x, y, L.img.w, L.img.h);
        try {
            ctx.drawImage(img, x, y, L.img.w, L.img.h);
        } catch (_) { /* image not ready — the black box stays */ }
        ctx.lineWidth = 1.5;
        ctx.strokeStyle = "rgba(0, 150, 255, 0.7)"; // signature blue, same as the tooltip box
        ctx.strokeRect(x + 0.5, y + 0.5, L.img.w - 1, L.img.h - 1);
        if (node.dlss5PreviewFrame !== null && node.dlss5PreviewFrame !== undefined) {
            const label = "frame " + node.dlss5PreviewFrame;
            ctx.font = "11px Arial, sans-serif";
            const tw = ctx.measureText(label).width;
            ctx.fillStyle = "rgba(0, 0, 0, 0.65)";
            ctx.fillRect(x + 4, y + 4, tw + 10, 18);
            ctx.fillStyle = "#ffffff";
            ctx.textAlign = "left";
            ctx.textBaseline = "top";
            ctx.fillText(label, x + 9, y + 8);
        }
        ctx.restore();
    };

    // --- mouse: pointer cursor on hover, click consumes and saves ---
    const setCursor = (cursor) => {
        try {
            const c = node._dlss5vGetCanvasEl ? node._dlss5vGetCanvasEl() : null;
            if (c && c.style) c.style.cursor = cursor || "";
        } catch (_) { /* cosmetic only */ }
    };

    // LiteGraph calls onMouseDown(e, pos, canvas) with node-local coords in pos[x, y].
    const originalOnMouseDown = node.onMouseDown;
    node.onMouseDown = function (e, pos, canvas) {
        const px = pos ? pos[0] : NaN;
        const py = pos ? pos[1] : NaN;
        
        if (inBtn(px, py, "preview")) {
            setCursor("");
            runPreview();
            return true; // consumed — no widget/graph handling for this click
        }
        if (inBtn(px, py, "hide")) {
            setCursor("");
            hidePreview();
            return true;
        }
        return originalOnMouseDown ? originalOnMouseDown.apply(this, arguments) : undefined;
    };

    const originalOnMouseMove = node.onMouseMove;
    node.onMouseMove = function (e, pos) {
        const px = pos ? pos[0] : NaN;
        const py = pos ? pos[1] : NaN;
        let over = null;
        if (inBtn(px, py, "preview")) over = "preview";
        else if (inBtn(px, py, "hide")) over = "hide";
        if (over !== node.dlss5BtnHover) {
            node.dlss5BtnHover = over;
            setCursor(over ? "pointer" : "");
            node.setDirtyCanvas(true);
            // Leaving the buttons: drop the remembered widget so the tooltip
            // re-arms when the pointer goes back onto a widget row.
            if (!over) this.dlss5HoverWidget = null;
        }
        if (over) {
            // The buttons are not widgets — suppress the tooltip hit-test here.
            this.dlss5ClearTooltipState?.();
            return undefined;
        }
        return originalOnMouseMove ? originalOnMouseMove.call(this, e, pos) : undefined;
    };

    const originalOnMouseLeave = node.onMouseLeave;
    node.onMouseLeave = function (e) {
        if (node.dlss5BtnHover) {
            node.dlss5BtnHover = null;
            setCursor("");
            node.setDirtyCanvas(true);
        }
        return originalOnMouseLeave ? originalOnMouseLeave.call(this, e) : undefined;
    };

    // Draw the buttons and (when shown) the preview image on every foreground
    // pass, after the tooltip pass.
    const originalOnDrawForeground = node.onDrawForeground;
    node.onDrawForeground = function (ctx) {
        const result = originalOnDrawForeground ? originalOnDrawForeground.call(this, ctx) : undefined;
        try {
            drawBtnRow(ctx);
            drawPreviewImage(ctx);
        } catch (_) { /* never break the draw */ }
        return result;
    };

    // --- node removed: release the object URL and reset the state ---
    const originalOnRemoved = node.onRemoved;
    node.onRemoved = function () {
        if (node.dlss5PreviewUrl) URL.revokeObjectURL(node.dlss5PreviewUrl);
        node.dlss5PreviewImage = null;
        node.dlss5PreviewUrl = null;
        node.dlss5PreviewFrame = null;
        node.dlss5PreviewShown = false;
        return originalOnRemoved ? originalOnRemoved.call(this) : undefined;
    };

    node.dlss5ControlsReady = true;
    console.info(`[OreX_DLSS5_video] Preview/Hide buttons ready (canvas-drawn) on node #${node.id}`);
}

// Регистрация расширения. В некоторых сборках ComfyUI глобальный `app` может
// появиться позже выполнения скрипта — тогда ждём его, а не молча отказываемся;
// если типы узлов уже зарегистрированы до загрузки скрипта, патчим их напрямую
// из реестра LiteGraph. Маркер в консоли (F12) подтверждает загрузку файла.
function _dlss5vRegister(app) {
    _dlss5vApp = app; // runPreview needs graphToPrompt + api.queuePrompt (v0.9.27)
    app.registerExtension({
        name: "DLSS5.NeuralRenderingVideo",
        beforeRegisterNodeDef(nodeType, nodeData) {
            if (nodeData && nodeData.name === "DLSS5NeuralRenderingVideo") {
                _dlss5vApplyTooltipToNodeType(nodeType);
            }
        },
        nodeCreated(node) {
            try {
                _dlss5vSetupNodeControls(node);
            } catch (e) {
                console.warn("[OreX_DLSS5_video] controls setup failed:", e);
            }
        }
    });
    const registry = (typeof LiteGraph !== "undefined") ? LiteGraph.registered_node_types : null;
    if (registry) {
        for (const key of ["DLSS5NeuralRenderingVideo"]) {
            if (registry[key]) _dlss5vApplyTooltipToNodeType(registry[key]);
        }
    }
}

if (typeof app !== "undefined" && app && typeof app.registerExtension === "function") {
    _dlss5vRegister(app);
    console.info("[OreX_DLSS5_video] extension registered (tooltips + Preview/Hide controls)");
} else {
    let _dlss5vTries = 0;
    const _dlss5vTimer = setInterval(() => {
        _dlss5vTries += 1;
        if (typeof app !== "undefined" && app && typeof app.registerExtension === "function") {
            clearInterval(_dlss5vTimer);
            _dlss5vRegister(app);
            console.info("[OreX_DLSS5_video] extension registered (deferred; tooltips + Preview/Hide controls)");
        } else if (_dlss5vTries >= 200) {
            clearInterval(_dlss5vTimer);
            console.warn("[OreX_DLSS5_video] 'app' global not found; extension NOT registered.");
        }
    }, 50);
}