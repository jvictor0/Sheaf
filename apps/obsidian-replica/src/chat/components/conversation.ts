import { setIcon } from "obsidian";

import type { ChatService } from "../service.js";
import type { ChatThreadSessionState } from "../../types.js";
import { itemSignature, renderTranscriptItem, updateTranscriptItem } from "./messageRenderer.js";

function formatWhen(value: string | null): string | null {
  if (!value) {
    return null;
  }
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    return value;
  }
  return parsed.toLocaleString();
}

export class ConversationComponent {
  private container: HTMLElement | null = null;
  private titleEl: HTMLElement | null = null;
  private subtitleEl: HTMLElement | null = null;
  private transcriptEl: HTMLElement | null = null;
  private statusEl: HTMLElement | null = null;
  private composerEl: HTMLTextAreaElement | null = null;
  private sendBtn: HTMLButtonElement | null = null;
  private composerValue = "";
  private transcriptRowMap = new Map<string, HTMLElement>();
  private lastTranscriptSnapshot = new Map<string, string>();
  private lastStatusText = "";
  private mountedThreadID: string | null = null;

  mount(parent: HTMLElement, service: ChatService): void {
    parent.empty();
    parent.addClass("sheaf-root");
    this.container = parent;

    const conversation = this.container.createDiv({ cls: "sheaf-conversation" });

    const header = conversation.createDiv({ cls: "sheaf-conv-header" });

    const backBtn = header.createEl("button", { cls: "sheaf-icon-btn" });
    setIcon(backBtn, "arrow-left");
    backBtn.setAttribute("aria-label", "Back to threads");
    backBtn.addEventListener("click", () => {
      void service.showThreadList();
    });

    const info = header.createDiv({ cls: "sheaf-conv-header-info" });
    this.titleEl = info.createDiv({ cls: "sheaf-conv-title" });
    this.subtitleEl = info.createDiv({ cls: "sheaf-conv-subtitle" });

    const settingsBtn = header.createEl("button", { cls: "sheaf-icon-btn" });
    setIcon(settingsBtn, "settings");
    settingsBtn.setAttribute("aria-label", "Open settings");
    settingsBtn.addEventListener("click", () => {
      service.openSettings();
    });

    this.transcriptEl = conversation.createDiv({ cls: "sheaf-transcript" });
    this.transcriptEl.setAttribute("role", "log");
    this.transcriptEl.setAttribute("aria-label", "Chat messages");

    this.statusEl = conversation.createDiv({ cls: "sheaf-status" });
    this.statusEl.setAttribute("aria-live", "polite");

    const composer = conversation.createDiv({ cls: "sheaf-composer" });

    const field = composer.createDiv({ cls: "sheaf-composer-field" });

    this.composerEl = field.createEl("textarea", {
      cls: "sheaf-composer-textarea",
      attr: { placeholder: "Message Sheaf\u2026", rows: "1" },
    });
    this.composerEl.setAttribute("aria-label", "Message input");
    this.composerEl.setAttribute("enterkeyhint", "newline");

    this.composerEl.addEventListener("input", () => {
      this.composerValue = this.composerEl?.value ?? "";
      this.autosizeComposer();
    });

    this.sendBtn = field.createEl("button", { cls: "sheaf-send-btn" });
    this.sendBtn.type = "button";
    this.sendBtn.setAttribute("aria-label", "Send message");
    setIcon(this.sendBtn, "arrow-right");
    this.sendBtn.addEventListener("click", () => {
      void this.submitComposer(service);
    });

    composer.createDiv({
      text: "Return adds a new line. Use the arrow to send.",
      cls: "sheaf-composer-hint sheaf-composer-hint--desktop",
    });
  }

  update(session: ChatThreadSessionState | null): void {
    if (!session) {
      this.transcriptEl?.empty();
      this.titleEl?.setText("");
      this.subtitleEl?.setText("");
      return;
    }

    if (this.mountedThreadID !== session.thread.thread_id) {
      this.resetTranscript();
      this.mountedThreadID = session.thread.thread_id;
    }

    this.titleEl?.setText(session.thread.name || session.thread.thread_id);
    const updated = formatWhen(session.thread.updated_at);
    this.subtitleEl?.setText(updated ? `Updated ${updated}` : "");

    this.syncTranscript(session);
    this.syncStatus(session);
    this.syncComposer(session);
  }

  scrollToBottomIfNeeded(): void {
    if (!this.transcriptEl) {
      return;
    }
    if (this.isNearBottom(this.transcriptEl)) {
      this.transcriptEl.scrollTop = this.transcriptEl.scrollHeight;
    }
  }

  destroy(): void {
    this.container = null;
    this.titleEl = null;
    this.subtitleEl = null;
    this.transcriptEl = null;
    this.statusEl = null;
    this.composerEl = null;
    this.sendBtn = null;
    this.transcriptRowMap.clear();
    this.lastTranscriptSnapshot.clear();
    this.lastStatusText = "";
    this.mountedThreadID = null;
    this.composerValue = "";
  }

  private syncTranscript(session: ChatThreadSessionState): void {
    if (!this.transcriptEl) {
      return;
    }

    const visibleSnapshot = new Map<string, string>();
    for (const item of session.transcriptItems) {
      visibleSnapshot.set(item.id, itemSignature(item));
    }

    let changed = visibleSnapshot.size !== this.lastTranscriptSnapshot.size;
    if (!changed) {
      for (const [id, sig] of visibleSnapshot.entries()) {
        if (this.lastTranscriptSnapshot.get(id) !== sig) {
          changed = true;
          break;
        }
      }
    }

    if (!changed) {
      return;
    }

    const wasNearBottom = this.isNearBottom(this.transcriptEl);

    const nextIds = new Set(session.transcriptItems.map((item) => item.id));
    for (const [id, node] of this.transcriptRowMap.entries()) {
      if (!nextIds.has(id)) {
        node.remove();
        this.transcriptRowMap.delete(id);
      }
    }

    for (const item of session.transcriptItems) {
      const existing = this.transcriptRowMap.get(item.id);
      if (existing) {
        updateTranscriptItem(existing, item);
        this.transcriptEl.appendChild(existing);
        continue;
      }
      const node = renderTranscriptItem(item);
      this.transcriptRowMap.set(item.id, node);
      this.transcriptEl.appendChild(node);
    }

    this.lastTranscriptSnapshot = visibleSnapshot;

    if (wasNearBottom) {
      this.transcriptEl.scrollTop = this.transcriptEl.scrollHeight;
    }
  }

  private syncStatus(session: ChatThreadSessionState): void {
    if (!this.statusEl) {
      return;
    }

    const text =
      session.errorMessage ??
      session.statusMessage ??
      (session.thinkingActive
        ? "Thinking\u2026"
        : session.connectionState === "connecting"
          ? "Connecting\u2026"
          : "");

    if (text === this.lastStatusText) {
      return;
    }

    this.lastStatusText = text;
    this.statusEl.setText(text);
    this.statusEl.className = session.errorMessage ? "sheaf-status sheaf-status--error" : "sheaf-status";
  }

  private syncComposer(session: ChatThreadSessionState): void {
    if (!this.composerEl) {
      return;
    }

    const canSend = session.connectionState === "live";
    this.composerEl.disabled = !canSend;
    this.composerEl.placeholder = canSend ? "Message Sheaf\u2026" : "Loading chat history\u2026";

    if (this.sendBtn) {
      this.sendBtn.disabled = !canSend;
    }

    if (this.composerEl.value !== this.composerValue && document.activeElement !== this.composerEl) {
      this.composerEl.value = this.composerValue;
    }

    this.autosizeComposer();
  }

  private async submitComposer(service: ChatService): Promise<void> {
    const value = this.composerEl?.value ?? "";
    const sent = await service.sendMessage(value);
    if (sent) {
      this.clearComposer();
    }
  }

  private clearComposer(): void {
    this.composerValue = "";
    if (!this.composerEl) {
      return;
    }
    this.composerEl.value = "";
    this.composerEl.blur();
    this.autosizeComposer();
  }

  private autosizeComposer(): void {
    if (!this.composerEl) {
      return;
    }

    const baseHeight = 44;
    const maxHeight = Math.max(88, Math.floor(window.innerHeight * 0.35));

    this.composerEl.style.height = `${baseHeight}px`;
    const scrollH = this.composerEl.scrollHeight;
    const nextHeight = Math.min(Math.max(scrollH, baseHeight), maxHeight);
    this.composerEl.style.height = `${nextHeight}px`;
    this.composerEl.style.overflowY = scrollH > maxHeight ? "auto" : "hidden";
  }

  private resetTranscript(): void {
    this.transcriptEl?.empty();
    this.transcriptRowMap.clear();
    this.lastTranscriptSnapshot.clear();
    this.lastStatusText = "";
  }

  private isNearBottom(el: HTMLElement): boolean {
    return el.scrollHeight - el.scrollTop - el.clientHeight < 24;
  }
}
