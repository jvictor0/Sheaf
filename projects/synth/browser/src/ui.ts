import { Action, CommandBufferError, CommandBufferFrame, Color, DrawCommand, DrawKind, Node, NodeKind, decodeCommandBuffer } from "./protocol.js";
import type { Bounds } from "./protocol.js";

// sru-59: height of the slider's value strip, carved out of the bottom of the
// node's own wire-set bounds so the number never overlaps the track. 14px is
// this backend's 9px glyph plus breathing room; JUCE reserves 18px for the
// same purpose at its own default text size (PortableJuceBackend.hpp:1162).
const SLIDER_READOUT_HEIGHT_PX = 14;
// Below this the strip cannot letter a 9px glyph at all, so the readout is
// dropped rather than rendered illegibly or spilling past the node's bounds.
const SLIDER_READOUT_MIN_PX = 8;
// The track always keeps the majority of the box: the strip may take at most
// this share of a short node's height before it is dropped entirely.
const SLIDER_READOUT_MAX_SHARE = 0.4;

// Height of the value strip for a node of `boundsHeight`, or 0 when the node
// is too short to carry one. Postflight-driven (2026-08-19): the first cut
// hardcoded 14px, which overflowed the wire-set bounds and zeroed the track's
// hit area on any slider ≤14px tall.
function sliderReadoutStrip(boundsHeight: number): number {
  if (!Number.isFinite(boundsHeight) || boundsHeight <= 0) return 0;
  const strip = Math.min(SLIDER_READOUT_HEIGHT_PX, Math.floor(boundsHeight * SLIDER_READOUT_MAX_SHARE));
  return strip >= SLIDER_READOUT_MIN_PX ? strip : 0;
}

export { CommandBufferError, decodeCommandBuffer } from "./protocol.js";
export type { Action, CommandBufferFrame } from "./protocol.js";

type PointerHandlers = {
  down: (event: PointerEvent) => void;
  move: (event: PointerEvent) => void;
  up: (event: PointerEvent) => void;
  cancel: (event: PointerEvent) => void;
  lostCapture: (event: PointerEvent) => void;
};
type NodeElement = HTMLElement & { synthNode?: Node; scrollContent?: HTMLElement; pointerHandlers?: PointerHandlers; draggedSincePointerDown?: boolean };
type CapturedPointer = { element: NodeElement; action: Action; anchorClientX: number; anchorClientY: number };
type PendingDrag = { action: Action; delta: number };
// The surface the frame resolves to: which node is the parentless root, and the
// extent the host is sized to. Nothing else needs resolving — every node's
// bounds are already in its parent's space (sru-46), so the DOM's own
// absolute-positioning-within-a-positioned-parent does the fold.
type ResolvedSurface = { rootId?: string; width: number; height: number };
export type ActionDispatcher = (action: Action) => void;

export class BrowserUiBackend {
  private readonly elements = new Map<string, NodeElement>();
  private readonly capturedPointers = new Map<number, CapturedPointer>();
  private readonly pendingDrags = new Map<string, PendingDrag>();
  private readonly resizeObserver: ResizeObserver;
  private dragFrame = 0;
  private surfaceRootId?: string;
  private surfaceWidth = 0;
  private surfaceHeight = 0;
  private surfaceScale = 1;
  private disposed = false;
  constructor(private readonly root: HTMLElement, private readonly dispatchBrowserAction: ActionDispatcher = () => {}) {
    this.root.style.position = "relative";
    this.resizeObserver = new ResizeObserver(() => this.fitSurface());
    this.resizeObserver.observe(root);
  }

  renderFrame(buffer: ArrayBuffer | CommandBufferFrame) {
    if (this.disposed) throw new Error("cannot render a disposed browser UI backend");
    const frame = buffer instanceof ArrayBuffer ? decodeCommandBuffer(buffer) : buffer;
    const nodes = new Map(frame.nodes.map((node) => [node.id, node]));
    const surface = resolveFrameSurface(frame.nodes, nodes);
    for (const node of frame.nodes) this.updateNode(node);
    for (const [id, element] of this.elements) {
      if (nodes.has(id)) continue;
      this.removePointerGesture(element);
      element.remove();
      this.elements.delete(id);
    }
    const rootNode = surface.rootId ? nodes.get(surface.rootId) : undefined;
    this.replaceChildrenIfChanged(this.root, rootNode ? [this.elementFor(rootNode)] : []);
    for (const node of frame.nodes)
      if (node.kind === NodeKind.Root || node.kind === NodeKind.Row || node.kind === NodeKind.Section || node.kind === NodeKind.ScrollArea)
        this.attachChildren(node, nodes);
    for (const node of frame.nodes) {
      if (node.kind !== NodeKind.Draw) continue;
      this.paint(this.elementFor(node).querySelector("canvas")!, frame.drawCommands.slice(node.drawStart, node.drawStart + node.drawCount), node.bounds);
    }
    this.surfaceRootId = surface.rootId;
    this.surfaceWidth = surface.width;
    this.surfaceHeight = surface.height;
    this.fitSurface();
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    if (this.dragFrame !== 0) cancelAnimationFrame(this.dragFrame);
    this.dragFrame = 0;
    this.pendingDrags.clear();
    this.resizeObserver.disconnect();
    for (const pointerId of [...this.capturedPointers.keys()]) this.clearPointer(pointerId, true);
    for (const element of this.elements.values()) this.removePointerGesture(element);
  }

  private updateNode(node: Node) {
    const element = this.elementFor(node);
    const kind = kindAttribute(node.kind);
    element.synthNode = node;
    element.dataset.synthNodeId = node.id;
    element.dataset.synthNodeKind = kind;
    element.dataset.nodeId = node.id;
    element.dataset.nodeKind = kind;
    element.style.position = "absolute";
    // Bounds are parent-relative (sru-46) and this element is an absolutely
    // positioned child of its parent's element, which is what parent-relative
    // means in the DOM. So the CSS offset is the wire value itself: no origin
    // subtraction, no coordinate-space classification.
    element.style.left = `${node.bounds.x}px`;
    element.style.top = `${node.bounds.y}px`;
    element.style.transform = "";
    element.style.transformOrigin = "";
    // A node arriving with no resolved bounds renders at its parent's origin
    // with zero extent (sprs-6). The backend never flows or sizes it.
    element.style.width = `${node.bounds.width}px`;
    element.style.height = `${node.bounds.height}px`;
    // The resolved width, published so a control's own chrome can be capped
    // against it in the stylesheet rather than expanding the element past the
    // extent the library resolved. This carries no appearance decision: the
    // padding and border values stay in the stylesheet, and this is the number
    // the line above already wrote.
    element.style.setProperty("--synth-node-width", `${node.bounds.width}px`);
    // `box-sizing: border-box` floors a used size at border plus padding, so a
    // zero-extent `<button>` would still render 26x2 pixels of its own chrome,
    // and an unsized `<select>` child would spill out of a zero-extent box.
    // Dropping the decorations and clipping makes the rendered extent the
    // resolved extent. This grows nothing and sizes nothing.
    const zeroExtent = node.bounds.width <= 0 || node.bounds.height <= 0;
    element.style.borderWidth = zeroExtent ? "0" : "";
    element.style.padding = zeroExtent ? "0" : "";
    element.style.overflow = zeroExtent ? "hidden" : node.kind === NodeKind.ScrollArea ? "auto" : "";
    const acceptsPointer = acceptsPointerEvents(node);
    element.style.pointerEvents = acceptsPointer ? "auto" : "none";
    element.style.zIndex = acceptsPointer ? "1" : "0";
    if (node.enabled) element.removeAttribute("aria-disabled");
    else element.setAttribute("aria-disabled", "true");
    applyCarriedStyle(element, node);
    this.updateControl(element, node);
    this.updatePointerGesture(element, node);
  }

  private elementFor(node: Node): NodeElement {
    const existing = this.elements.get(node.id);
    if (existing) return existing;
    const element = this.createElement(node);
    this.elements.set(node.id, element);
    return element;
  }

  private createElement(node: Node): NodeElement {
    const element = document.createElement(node.kind === NodeKind.Button ? "button" : node.kind === NodeKind.Section ? "section" : "div") as NodeElement;
    if (node.kind === NodeKind.Toggle) { const input = document.createElement("input"); input.type = "checkbox"; element.append(input, document.createElement("span")); input.addEventListener("change", () => this.dispatchValue(element, input.checked ? "1" : "0")); }
    if (node.kind === NodeKind.Slider) {
      const input = document.createElement("input"); input.type = "range";
      // sru-59: the readout is a sibling `<output>`, not a wrapper, appended
      // after the input. It is absolutely positioned within the node's own
      // element -- already `position: absolute` (updateNode, above), so it
      // is already a valid containing block -- which keeps the readout
      // inside the wire-set bounds instead of growing them (design.md
      // Layout/metrics). `pointer-events: none` keeps it out of hit-testing
      // so it never steals a press meant for the input underneath it.
      // Colour reads the same `--synth-glyph` custom property every other
      // carried-text-style surface reads (applyCarriedStyle sets it on this
      // same element), falling back to `inherit` -- the toggle label's own
      // fallback, not a new hardcoded colour.
      // The readout gets its OWN strip at the bottom of the node box and the
      // track is shortened to make room, exactly as JUCE's TextBoxBelow
      // splits its own bounds (PortableJuceBackend.hpp:1162 asks for an 18px
      // box below the slider). Overlaying the number ON the track instead --
      // the first cut of this -- put the digits across the filled track and
      // under the thumb, unreadable at every value.
      const output = document.createElement("output");
      output.style.position = "absolute";
      output.style.left = "0"; output.style.right = "0"; output.style.bottom = "0";
      output.style.height = `${SLIDER_READOUT_HEIGHT_PX}px`;
      output.style.lineHeight = `${SLIDER_READOUT_HEIGHT_PX}px`;
      output.style.textAlign = "center";
      output.style.font = `9px/${SLIDER_READOUT_HEIGHT_PX}px inherit`;
      output.style.color = "var(--synth-glyph, inherit)";
      output.style.pointerEvents = "none";
      // Track occupies everything above the readout strip. `absolute` (not
      // the browser default static) so the two never fight over flow, and so
      // a short node box shrinks the track rather than pushing the readout
      // out of the wire-set bounds.
      input.style.position = "absolute";
      input.style.left = "0"; input.style.right = "0"; input.style.top = "0";
      input.style.width = "100%";
      input.style.height = `calc(100% - ${SLIDER_READOUT_HEIGHT_PX}px)`;
      input.style.margin = "0";
      element.append(input, output);
      // The `input` event is the only seam a drag updates on BETWEEN frames
      // (design.md): `updateControl` only reaches the readout once the next
      // frame lands, which would otherwise leave it stale mid-drag.
      input.addEventListener("input", () => {
        output.textContent = formatSliderValue(Number(input.value), element.synthNode?.step ?? node.step);
        this.dispatchValue(element, input.value);
      });
    }
    if (node.kind === NodeKind.ComboBox) { const select = document.createElement("select"); element.append(select); select.addEventListener("change", () => this.dispatchValue(element, select.value)); }
    if (node.kind === NodeKind.TextField) { const input = document.createElement("input"); input.type = "text"; element.append(input); input.addEventListener("input", () => this.dispatchValue(element, input.value)); }
    if (node.kind === NodeKind.Draw) { const canvas = document.createElement("canvas"); element.append(canvas); }
    if (node.kind === NodeKind.ScrollArea) { const content = document.createElement("div"); content.style.position = "relative"; element.scrollContent = content; element.append(content); }
    // sru-52 adds `Draw` here: a `Draw` node carrying a plain click action
    // dispatches it through the same `dispatchValue` a `Button` does, on one
    // shared listener so the two kinds cannot diverge. `acceptsPointerEvents`
    // already lets a node with an action take pointer input, so a `Draw` node
    // carrying no action at all still intercepts nothing.
    if (node.kind === NodeKind.Button || node.kind === NodeKind.Draw)
      element.addEventListener("click", () => { if (!this.consumeDragSuppression(element)) this.dispatchValue(element); });
    element.addEventListener("dblclick", () => this.dispatchDoubleClick(element));
    return element;
  }

  private updatePointerGesture(element: NodeElement, node: Node) {
    if (node.pointerDragAction && !element.pointerHandlers) {
      const handlers: PointerHandlers = {
        down: (event) => this.beginPointerDrag(element, event),
        move: (event) => this.continuePointerDrag(element, event),
        up: (event) => this.clearPointer(event.pointerId, true),
        cancel: (event) => this.clearPointer(event.pointerId, true),
        lostCapture: (event) => this.clearPointer(event.pointerId, false),
      };
      element.addEventListener("pointerdown", handlers.down);
      element.addEventListener("pointermove", handlers.move);
      element.addEventListener("pointerup", handlers.up);
      element.addEventListener("pointercancel", handlers.cancel);
      element.addEventListener("lostpointercapture", handlers.lostCapture);
      element.pointerHandlers = handlers;
    } else if (!node.pointerDragAction && element.pointerHandlers) {
      this.removePointerGesture(element);
    }
  }

  // sru-52: the DOM fires a native `click` for a press and release inside one
  // element however far the pointer travelled between them, so a gesture that
  // has already dispatched a drag consumes the click it would otherwise also be
  // read as. The drag threshold is `continuePointerDrag`'s and only its — this
  // asks whether that threshold was crossed, never how far the pointer moved.
  private consumeDragSuppression(element: NodeElement) {
    if (!element.draggedSincePointerDown) return false;
    element.draggedSincePointerDown = false;
    return true;
  }

  private beginPointerDrag(element: NodeElement, event: PointerEvent) {
    element.draggedSincePointerDown = false;
    const action = enabledNodeOf(element)?.pointerDragAction;
    if (!action) return;
    for (const captured of this.capturedPointers.values())
      if (captured.element === element) return;
    this.clearPointer(event.pointerId, true);
    try { element.setPointerCapture(event.pointerId); } catch { return; }
    this.capturedPointers.set(event.pointerId, {
      element,
      action: { ...action },
      anchorClientX: event.clientX,
      anchorClientY: event.clientY,
    });
  }

  // The element's own current on-screen scale, derived from its live rect
  // vs its wire-set (design-space) width, instead of the single root-wide
  // `surfaceScale`. `surfaceScale` only reflects the ONE transform
  // `fitSurface` puts on the surface root -- it says nothing about any
  // additional transform a host page may put on an ancestor further down
  // the tree (e.g. a per-block container transform), and composing
  // transforms multiply, so `surfaceScale` alone silently under/over-counts
  // the true effective scale as soon as such a transform exists. Reading
  // the rect directly is transform-robust by construction: whatever
  // ancestor transforms are in play, `getBoundingClientRect()` already
  // reflects their combined effect.
  // Recomputed on every call rather than cached at drag start, matching
  // the existing "uses the current surface scale for each accepted drag
  // increment" contract (a resize mid-drag must retarget sensitivity
  // immediately, not just at the next drag's start) -- ui-backend.spec.ts's
  // "uses the current surface scale for each accepted drag increment" test
  // pins this. Falls back to `surfaceScale` (today's behavior, so the
  // untransformed case is unchanged) whenever the rect or wire width is
  // degenerate (not yet laid out, zero-extent node, etc).
  private effectiveScaleFor(element: NodeElement): number {
    const wireWidth = element.synthNode?.bounds.width;
    if (!wireWidth || wireWidth <= 0) return this.surfaceScale;
    const rectWidth = element.getBoundingClientRect().width;
    if (!(rectWidth > 0)) return this.surfaceScale;
    return rectWidth / wireWidth;
  }

  private continuePointerDrag(element: NodeElement, event: PointerEvent) {
    const captured = this.capturedPointers.get(event.pointerId);
    if (!captured || captured.element !== element) return;
    if (!enabledNodeOf(element)) return this.clearPointer(event.pointerId, true);
    const scale = this.effectiveScaleFor(element);
    const delta = (((event.clientX - captured.anchorClientX) / scale) -
      ((event.clientY - captured.anchorClientY) / scale)) * 0.0025;
    if (Math.abs(delta) < 0.001) return;
    element.draggedSincePointerDown = true;
    this.dispatchDrag(captured.action, delta);
    captured.anchorClientX = event.clientX;
    captured.anchorClientY = event.clientY;
  }

  private clearPointer(pointerId: number, releaseCapture: boolean) {
    const captured = this.capturedPointers.get(pointerId);
    if (!captured) return;
    this.capturedPointers.delete(pointerId);
    if (releaseCapture) {
      try { captured.element.releasePointerCapture(pointerId); } catch { /* Capture may already have been released by the browser. */ }
    }
    this.flushDrags();
  }

  private removePointerGesture(element: NodeElement) {
    const handlers = element.pointerHandlers;
    if (!handlers) return;
    for (const [pointerId, captured] of this.capturedPointers)
      if (captured.element === element) this.clearPointer(pointerId, true);
    element.removeEventListener("pointerdown", handlers.down);
    element.removeEventListener("pointermove", handlers.move);
    element.removeEventListener("pointerup", handlers.up);
    element.removeEventListener("pointercancel", handlers.cancel);
    element.removeEventListener("lostpointercapture", handlers.lostCapture);
    delete element.pointerHandlers;
    // A node that has just lost its drag action must not carry a suppression
    // into its next click.
    delete element.draggedSincePointerDown;
  }

  private updateControl(element: NodeElement, node: Node) {
    if (node.kind === NodeKind.Button) { element.textContent = node.label; element.toggleAttribute("disabled", !node.enabled); }
    if (node.kind === NodeKind.Label || node.kind === NodeKind.StatusText) element.textContent = node.text || node.label;
    if (node.kind === NodeKind.Toggle) { const input = element.querySelector("input")!; input.checked = node.checked; input.disabled = !node.enabled; element.querySelector("span")!.textContent = node.label; }
    if (node.kind === NodeKind.Slider) {
      const input = element.querySelector("input")!;
      input.min = String(node.minValue); input.max = String(node.maxValue); input.step = String(node.step);
      if (document.activeElement !== input) input.value = String(node.value);
      // Deliberately NOT gated by the focused-input guard above (design.md
      // Risks): the readout has no focus semantics of its own, so gating it
      // the same way would freeze it mid-drag whenever the input is focused.
      const output = element.querySelector("output")!;
      output.textContent = formatSliderValue(node.value, node.step);
      // The strip is carved out of the node's OWN height, so it must adapt to
      // it: a fixed 14px would overflow the wire-set bounds (breaking sru-59)
      // on a node ≤14px tall, and `calc(100% - 14px)` would clamp the track to
      // a 0-height, silently un-draggable control. Below the legibility floor
      // the readout is dropped entirely and the track keeps the whole box —
      // a slider too short to letter is still a slider.
      const strip = sliderReadoutStrip(node.bounds.height);
      output.style.display = strip > 0 ? "" : "none";
      output.style.height = `${strip}px`;
      output.style.lineHeight = `${strip}px`;
      output.style.font = `9px/${strip}px inherit`;
      input.style.height = strip > 0 ? `calc(100% - ${strip}px)` : "100%";
      input.disabled = !node.enabled;
    }
    if (node.kind === NodeKind.ComboBox) {
      const select = element.querySelector("select")!;
      const optionsChanged = select.options.length !== node.options.length ||
        node.options.some((option, index) => select.options[index]?.value !== option.id || select.options[index]?.textContent !== option.label);
      if (optionsChanged)
        select.replaceChildren(...node.options.map((option) => new Option(option.label, option.id)));
      if (document.activeElement !== select) select.value = node.selectedOption;
      select.disabled = !node.enabled;
    }
    if (node.kind === NodeKind.TextField) {
      const input = element.querySelector("input")!;
      if (document.activeElement !== input) input.value = node.text;
      input.disabled = !node.enabled;
    }
    if (node.kind === NodeKind.ScrollArea && element.scrollContent) { element.scrollContent.style.width = `${Math.max(node.bounds.width, node.scrollContentWidth)}px`; element.scrollContent.style.height = `${Math.max(node.bounds.height, node.scrollContentHeight)}px`; }
  }

  private attachChildren(node: Node, nodes: Map<string, Node>) {
    const element = this.elementFor(node);
    const parent = element.scrollContent ?? element;
    this.replaceChildrenIfChanged(parent, node.children.map((child) => this.elementFor(nodes.get(child)!)));
  }

  private replaceChildrenIfChanged(parent: HTMLElement, children: HTMLElement[]) {
    const current = Array.from(parent.children);
    if (current.length === children.length && current.every((child, index) => child === children[index])) return;
    parent.replaceChildren(...children);
  }

  private dispatchValue(element: NodeElement, value?: string) {
    const action = enabledNodeOf(element)?.action;
    if (!action) return;
    this.dispatchBrowserAction({ name: action.name, value: value === undefined ? action.value : appendActionValue(action.value, value) });
  }
  private dispatchDoubleClick(element: NodeElement) { const action = enabledNodeOf(element)?.doubleClickAction; if (action) this.dispatchBrowserAction(action); }
  private dispatchDrag(action: Action, delta: number) {
    const separator = action.value.lastIndexOf(":");
    const prefix = separator < 0 ? "" : action.value.slice(0, separator + 1);
    const key = `${action.name}\0${prefix}`;
    const pending = this.pendingDrags.get(key);
    if (pending) pending.delta += delta;
    else this.pendingDrags.set(key, { action: { name: action.name, value: prefix }, delta });
    if (this.dragFrame !== 0) return;
    this.dragFrame = requestAnimationFrame(() => this.flushDrags());
  }

  private flushDrags() {
    if (this.dragFrame !== 0) {
      cancelAnimationFrame(this.dragFrame);
      this.dragFrame = 0;
    }
    this.dragFrame = 0;
    const drags = [...this.pendingDrags.values()];
    this.pendingDrags.clear();
    for (const drag of drags) {
      if (Math.abs(drag.delta) < 0.001) continue;
      this.dispatchBrowserAction({ name: drag.action.name, value: `${drag.action.value}${drag.delta}` });
    }
  }

  private fitSurface() {
    const availableWidth = this.root.clientWidth;
    this.surfaceScale = availableWidth > 0 && this.surfaceWidth > 0 ? Math.min(1, availableWidth / this.surfaceWidth) : 1;
    if (this.surfaceRootId) {
      const element = this.elements.get(this.surfaceRootId);
      if (element) {
        element.style.transformOrigin = "0 0";
        element.style.transform = `scale(${this.surfaceScale})`;
      }
    }
    this.root.style.height = `${this.surfaceHeight * this.surfaceScale}px`;
  }

  private paint(canvas: HTMLCanvasElement, commands: DrawCommand[], bounds: Bounds) {
    canvas.width = Math.max(1, Math.round(bounds.width)); canvas.height = Math.max(1, Math.round(bounds.height));
    canvas.style.width = "100%"; canvas.style.height = "100%";
    const context = canvas.getContext("2d")!;
    // Draw geometry is relative to the owning node's own origin (sru-46), and
    // the canvas already spans exactly that node, so the canvas origin is the
    // node origin. No translation and no classification of the commands.
    const nodeExtent: Bounds = { x: 0, y: 0, width: bounds.width, height: bounds.height };
    for (const command of commands) this.draw(context, command, nodeExtent);
  }

  private draw(context: CanvasRenderingContext2D, command: DrawCommand, nodeExtent: Bounds) {
    const fill = colorCss(command.color); const stroke = colorCss(command.color); const b = command.bounds;
    context.fillStyle = fill; context.strokeStyle = stroke; context.lineWidth = command.strokeWidth;
    switch (command.kind) {
      case DrawKind.Fill: {
        // A fill with no geometry of its own covers the whole node.
        const area = hasExplicitBounds(b) ? b : nodeExtent;
        context.fillRect(area.x, area.y, area.width, area.height); break;
      }
      case DrawKind.StrokeRect: context.strokeRect(b.x, b.y, b.width, b.height); break;
      case DrawKind.Line:
        context.beginPath(); context.moveTo(command.from.x, command.from.y); context.lineTo(command.to.x, command.to.y); context.stroke(); break;
      case DrawKind.Arc:
        context.save();
        context.lineCap = "round";
        context.lineJoin = "round";
        context.beginPath();
        context.arc(b.x + b.width / 2, b.y + b.height / 2, Math.min(b.width, b.height) / 2, portableAngleToCanvas(command.startRadians), portableAngleToCanvas(command.endRadians));
        context.stroke();
        context.restore();
        break;
      case DrawKind.Text: context.fillStyle = colorCss(command.textColor); context.font = `${command.textSize}px sans-serif`; context.textAlign = command.align === 1 ? "center" : command.align === 2 ? "right" : "left"; context.fillText(command.text, command.align === 1 ? b.x + b.width / 2 : command.align === 2 ? b.x + b.width : b.x, b.y + command.textSize); break;
      case DrawKind.FillEllipse: context.beginPath(); context.ellipse(b.x + b.width / 2, b.y + b.height / 2, b.width / 2, b.height / 2, 0, 0, Math.PI * 2); context.fill(); break;
      case DrawKind.StrokeEllipse: context.beginPath(); context.ellipse(b.x + b.width / 2, b.y + b.height / 2, b.width / 2, b.height / 2, 0, 0, Math.PI * 2); context.stroke(); break;
      case DrawKind.FillRoundedRect: roundedRect(context, b, command.cornerRadius); context.fill(); break;
      case DrawKind.StrokeRoundedRect: roundedRect(context, b, command.cornerRadius); context.stroke(); break;
      case DrawKind.Polyline: path(context, command.points); context.stroke(); break;
      case DrawKind.FillPolygon: path(context, command.points); context.fill(); break;
    }
  }
}

// Validates the frame's node graph and reports the surface it resolves to. No
// bounds arithmetic: every node's bounds are already the coordinates its DOM
// element needs, and the host extent is the root's own resolved extent.
function resolveFrameSurface(nodesInOrder: Node[], nodes: Map<string, Node>): ResolvedSurface {
  if (nodes.size !== nodesInOrder.length) throw new Error("duplicate node id in browser UI frame");
  const parents = new Map<string, string>();
  let multipleParentError: string | undefined;
  for (const node of nodesInOrder) {
    for (const childId of node.children) {
      if (!nodes.has(childId)) throw new Error(`unknown child node ${childId}`);
      const existing = parents.get(childId);
      if (existing && existing !== node.id) multipleParentError ??= `node ${childId} has multiple parents`;
      else parents.set(childId, node.id);
    }
  }
  const roots = nodesInOrder.filter((node) => !parents.has(node.id));
  if (nodesInOrder.length > 0 && roots.length !== 1) throw new Error(`browser UI frame requires one parentless root, found ${roots.length}`);
  if (roots[0] && roots[0].kind !== NodeKind.Root) throw new Error("parentless browser UI node must be a root");

  const states = new Map<string, "visiting" | "visited">();
  const visit = (node: Node) => {
    const state = states.get(node.id);
    if (state === "visiting") throw new Error(`cycle in browser UI frame at ${node.id}`);
    if (state === "visited") return;
    states.set(node.id, "visiting");
    for (const childId of node.children) {
      const child = nodes.get(childId);
      if (!child) throw new Error(`unknown child node ${childId}`);
      visit(child);
    }
    states.set(node.id, "visited");
  };
  for (const node of nodesInOrder) visit(node);
  if (multipleParentError) throw new Error(multipleParentError);

  // The host extent is the resolved root extent, never the union of flowed
  // content (sprs-6). Nothing the backend does can place content below it.
  const root = roots[0];
  return { rootId: root?.id, width: root?.bounds.width ?? 0, height: root?.bounds.height ?? 0 };
}

function hasExplicitBounds(bounds: Bounds) { return bounds.width > 0 && bounds.height > 0; }

function kindAttribute(kind: NodeKind) {
  return NodeKind[kind].replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`).replace(/^-/, "");
}

// Per-node custom properties rather than per-surface inline styles: one carried
// value, and `synth-browser.css` decides which surface it paints for each kind
// (sru-45's per-kind table). The properties are set on *every* node, absent
// ones to `initial`, because custom properties inherit — otherwise a container's
// carried fill would leak into an unstyled descendant.
function applyCarriedStyle(element: NodeElement, node: Node) {
  // `Draw` carries no node colour: its commands carry their own (sru-45).
  const carried = node.kind === NodeKind.Draw ? undefined : node.color;
  // A checked toggle reads as selected, as it does in the JUCE backend.
  const selected = node.selected || (node.kind === NodeKind.Toggle && node.checked);
  const fill = carried && stateColor(carried, selected, node.enabled);
  setCarriedProperty(element, "--synth-fill", fill && colorCss(fill));
  setCarriedProperty(element, "--synth-fill-hover", fill && colorCss(brighter(fill, 0.14)));
  // Pressed brightens the carried colour itself, matching the JUCE backend's
  // `buttonOnColourId`, so it does not compound with the selected fold.
  setCarriedProperty(element, "--synth-fill-active", carried && colorCss(brighter(carried, 0.24)));
  setCarriedProperty(element, "--synth-glyph", node.textStyle && colorCss(node.textStyle.color));
  setCarriedProperty(element, "--synth-text-size", node.textStyle && `${node.textStyle.size}px`);
  setCarriedProperty(element, "--synth-text-align", node.textStyle && flexAlignment(node.textStyle.align));
  const container = node.kind === NodeKind.Root || node.kind === NodeKind.Row ||
                    node.kind === NodeKind.Section || node.kind === NodeKind.ScrollArea;
  setCarriedProperty(element, "--synth-border-color", container && node.borderColor ? colorCss(node.borderColor) : undefined);
  setCarriedProperty(element, "--synth-border-width", container && node.borderWidth !== undefined ? `${node.borderWidth}px` : undefined);
  setCarriedProperty(element, "--synth-corner-radius", container && node.cornerRadius !== undefined ? `${node.cornerRadius}px` : undefined);
}

// `initial` on a custom property is the guaranteed-invalid value, so every
// `var(--synth-*, default)` in the stylesheet falls back to the backend's own
// default look for a node that carries nothing.
function setCarriedProperty(element: NodeElement, name: string, value?: string) {
  element.style.setProperty(name, value ?? "initial");
}

function flexAlignment(align: number) { return align === 1 ? "center" : align === 2 ? "flex-end" : "flex-start"; }

// Selected and disabled presentation is derived from the carried colour, never
// substituted from a palette (sru-45). Fold for fold the same as `StateColourFor`
// in `PortableJuceBackend.hpp` — `darker(0.35f)` disabled, `brighter(0.14f)`
// selected, alpha carried through untouched — so both backends land on the same
// bytes for the same carried colour.
function stateColor(color: Color, selected: boolean, enabled: boolean): Color {
  if (!enabled) return darker(color, 0.35);
  return selected ? brighter(color, 0.14) : color;
}
// `juce::Colour::brighter`/`darker`: one factor over each channel's distance
// from its limit, truncated to a byte.
function brighter(color: Color, amount: number): Color {
  const factor = 1 / (1 + amount);
  const channel = (value: number) => Math.trunc(255 - factor * (255 - value));
  return { r: channel(color.r), g: channel(color.g), b: channel(color.b), a: color.a };
}
function darker(color: Color, amount: number): Color {
  const factor = 1 / (1 + amount);
  const channel = (value: number) => Math.trunc(factor * value);
  return { r: channel(color.r), g: channel(color.g), b: channel(color.b), a: color.a };
}

// sru-59 shared formatter, called from both the Slider create-path `input`
// listener and the Slider `updateControl` branch (design.md's "ONE shared
// formatter" requirement, §8) so the two update sites cannot drift apart.
//
// JUCE parity, traced to the vendored source (not re-derived from memory):
// `juce_Slider.cpp:145-162 updateRange()` -- the private `numDecimalPlaces`
// default-path branch, ported literally below. `step === 0` (continuous)
// never enters the trailing-zero loop in JUCE either: it is guarded by
// `if (! approximatelyEqual (interval, 0.0))`, so it is pinned to the
// untouched default of 7 rather than folding through the loop (which would
// otherwise collapse a literal 0 to N=0). No current producer sends step 0
// today, but the function must be total.
function sliderDecimalPlaces(step: number): number {
  if (step === 0) return 7;
  let places = 7;
  let scaled = Math.round(Math.abs(step) * 1e7);
  while (scaled % 10 === 0 && places > 0) {
    places--;
    scaled = Math.floor(scaled / 10);
  }
  return places;
}

// JUCE parity, traced to `juce_String.cpp:472-503 StackArrayStream::writeDouble`
// (the mechanism behind `getTextFromValue`'s `String (val, N)` at
// `juce_Slider.cpp:1655`): fixed-point formatting only applies for N > 0 --
// `if (numDecPlaces > 0) { o.setf(fixed); o.precision(N); }` -- so `toFixed`
// reproduces that branch exactly (confirmed by the byte-parity table in
// ui-backend.spec.ts, including the classic 2.675/0.01 toFixed rounding case,
// which happens to already match JUCE for the same binary double).
// For N === 0 the stream keeps iostream's OWN default (non-fixed) formatting
// instead of rounding to an integer, so a value that is not itself on a
// whole-number step still renders its fraction. `String(Math.round(value))`
// was the first candidate (design.md's literal text) and was REJECTED by the
// byte-parity table: for value 0.5 / step 1 it collapsed to "1" (and -0.5 to
// "0", losing the sign and the value entirely) where the traced JUCE
// mechanism -- compiled and run standalone to confirm, not assumed -- prints
// "0.5" / "-0.5". `String(value)` reproduces JUCE exactly for every value
// this app's producers carry (whole numbers when N is 0, matching JUCE's own
// trailing-zero-free output for them; see the byte-parity table for the
// measured rows). This is the JUCE-authority adjustment design.md's own
// contingency clause calls for.
function formatSliderValue(value: number, step: number): string {
  const places = sliderDecimalPlaces(step);
  return places > 0 ? value.toFixed(places) : String(value);
}

function enabledNodeOf(element: NodeElement) { const node = element.synthNode; return node?.enabled ? node : undefined; }
function colorCss(color: Color) { return `rgba(${color.r}, ${color.g}, ${color.b}, ${color.a / 255})`; }
function appendActionValue(prefix: string, value: string) { return prefix.length > 0 ? `${prefix}:${value}` : value; }
function portableAngleToCanvas(radians: number) { return radians - Math.PI / 2; }
function acceptsPointerEvents(node: Node) {
  return node.kind === NodeKind.Button || node.kind === NodeKind.Toggle || node.kind === NodeKind.Slider ||
    node.kind === NodeKind.ComboBox || node.kind === NodeKind.TextField || node.kind === NodeKind.ScrollArea ||
    Boolean(node.action || node.pointerDragAction || node.doubleClickAction);
}
function path(context: CanvasRenderingContext2D, points: Array<{ x: number; y: number }>) { context.beginPath(); if (points[0]) { context.moveTo(points[0].x, points[0].y); for (const point of points.slice(1)) context.lineTo(point.x, point.y); } }
function roundedRect(context: CanvasRenderingContext2D, bounds: { x: number; y: number; width: number; height: number }, radius: number) { context.beginPath(); context.roundRect(bounds.x, bounds.y, bounds.width, bounds.height, radius); }
