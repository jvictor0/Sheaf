import type { ChatTranscriptItem } from "../../types.js";

export function itemSignature(item: ChatTranscriptItem): string {
  switch (item.kind) {
    case "tool_call":
      return `${item.kind}:${item.text}:${item.tone}`;
    case "streaming":
      return `${item.kind}:${item.text}:${item.queueID}`;
    default:
      return `${item.kind}:${item.role}:${item.text}`;
  }
}

export function renderTranscriptItem(item: ChatTranscriptItem): HTMLElement {
  const element = document.createElement("div");
  element.dataset.signature = itemSignature(item);
  element.setAttribute("role", "listitem");

  if (item.kind === "tool_call") {
    element.addClasses(["sheaf-message", "sheaf-message--tool"]);
    if (item.tone === "error") {
      element.addClass("sheaf-message--tool-error");
    }
    element.setText(item.text);
    return element;
  }

  element.addClass("sheaf-message");

  if (item.role === "user") {
    element.addClass("sheaf-message--user");
  }
  else if (item.role === "assistant") {
    element.addClass("sheaf-message--assistant");
  }
  else {
    element.addClass("sheaf-message--system");
  }

  if (item.kind === "pending") {
    element.addClass("sheaf-message--pending");
  }

  if (item.kind === "streaming") {
    element.addClass("sheaf-message--streaming");
  }

  element.setText(item.text);
  return element;
}

export function updateTranscriptItem(element: HTMLElement, item: ChatTranscriptItem): void {
  const newSig = itemSignature(item);
  if (element.dataset.signature === newSig) {
    return;
  }

  if (item.kind === "streaming" && element.hasClass("sheaf-message--streaming")) {
    element.textContent = item.text;
    element.dataset.signature = newSig;
    return;
  }

  const replacement = renderTranscriptItem(item);
  element.className = replacement.className;
  element.textContent = replacement.textContent;
  element.dataset.signature = replacement.dataset.signature;
}
