// Optional client debug: POST layout + document HTML to sheaf server POST /debug/log.
// Wire from SheafChatView (or a command) via BuildDomDebugPayload + PostDebugLog + ChatService.getServerBaseUrl().
//
import { requestUrl } from "obsidian";

const x_maxOuterHtmlChars = 1_200_000;

function ElementClassString(el: Element): string {
  if (el instanceof HTMLElement)
  {
    return typeof el.className === "string" ? el.className : "";
  }

  return el.getAttribute("class") ?? "";
}

export function TruncateForDebug(text: string, maxChars: number): string {
  if (text.length <= maxChars)
  {
    return text;
  }

  return `${text.slice(0, maxChars)}\n\n...[truncated ${text.length - maxChars} chars]`;
}

export function BuildViewportSummary(): string {
  const vv = window.visualViewport;
  const lines = [
    `window.inner=${window.innerWidth}x${window.innerHeight}`,
    `visualViewport=${vv?.width ?? "?"}x${vv?.height ?? "?"} offsetTop=${vv?.offsetTop ?? "?"} offsetLeft=${vv?.offsetLeft ?? "?"} scale=${vv?.scale ?? "?"}`,
    `documentElement.clientHeight=${document.documentElement.clientHeight} scrollTop=${document.documentElement.scrollTop}`,
    `body.className=${document.body?.className ?? ""}`,
  ];
  return lines.join("\n");
}

export function DumpAncestorLayoutChain(start: HTMLElement | null): string {
  const lines: string[] = ["=== ancestor layout (start -> body) ==="];

  let el: HTMLElement | null = start;
  while (el)
  {
    const r = el.getBoundingClientRect();
    const cs = getComputedStyle(el);
    const cls = ElementClassString(el);
    const id = el.id ? `#${el.id}` : "";
    lines.push(
      `${el.tagName}${id}${cls ? `.${cls.split(/\s+/).join(".")}` : ""}`,
      `  rect x=${r.x.toFixed(1)} y=${r.y.toFixed(1)} w=${r.width.toFixed(1)} h=${r.height.toFixed(1)}`,
      `  display=${cs.display} flexDir=${cs.flexDirection} gap=${cs.gap}`,
      `  padding=${cs.padding} margin=${cs.margin}`,
      `  borderTop=${cs.borderTopWidth} borderBottom=${cs.borderBottomWidth}`,
      `  overflow=${cs.overflow} overflowY=${cs.overflowY}`,
      `  height=${cs.height} minH=${cs.minHeight} maxH=${cs.maxHeight}`,
      `  inlineStyle=${(el as HTMLElement).style?.cssText || "(none)"}`,
      "",
    );

    if (el === document.body)
    {
      break;
    }

    el = el.parentElement;
  }

  return lines.join("\n");
}

export function BuildFullDocumentHtmlDump(): string {
  const root = document.documentElement;
  const html = root?.outerHTML ?? "";
  return `=== document.documentElement.outerHTML (${html.length} chars) ===\n${TruncateForDebug(html, x_maxOuterHtmlChars)}`;
}

export function BuildDomDebugPayload(trigger: string, contentEl: HTMLElement): string {
  const parts = [
    `=== Sheaf DOM debug: ${trigger} ===`,
    BuildViewportSummary(),
    "",
    DumpAncestorLayoutChain(contentEl),
    "",
    "=== plugin subtree (contentEl) simplified ===",
    DumpSubtreeSummary(contentEl, 0, 6),
    "",
    BuildFullDocumentHtmlDump(),
  ];
  return parts.join("\n");
}

function DumpSubtreeSummary(el: Element, depth: number, maxDepth: number): string {
  if (depth > maxDepth)
  {
    return "";
  }

  const indent = "  ".repeat(depth);
  const tag = el.tagName.toLowerCase();
  const rawClass = ElementClassString(el);
  const cls = rawClass ? `.${rawClass.split(/\s+/).slice(0, 4).join(".")}` : "";
  const r = el.getBoundingClientRect();
  const cs = getComputedStyle(el);
  let line = `${indent}<${tag}${cls}> ${r.width.toFixed(0)}x${r.height.toFixed(0)} pad=${cs.padding} gap=${cs.gap}\n`;

  for (let i = 0; i < el.children.length; i++)
  {
    line += DumpSubtreeSummary(el.children[i], depth + 1, maxDepth);
  }

  return line;
}

export async function PostDebugLog(baseUrl: string, message: string): Promise<void> {
  await requestUrl({
    url: `${baseUrl.replace(/\/$/, "")}/debug/log`,
    method: "POST",
    contentType: "application/json",
    body: JSON.stringify({ message }),
  });
}
