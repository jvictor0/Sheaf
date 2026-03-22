import { App, Notice, setIcon } from "obsidian";

import type { ChatService } from "../service.js";
import type { ChatViewState } from "../../types.js";
import { NewThreadModal } from "../modals.js";

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

export class ThreadListComponent {
  private container: HTMLElement | null = null;

  mount(parent: HTMLElement): void {
    parent.empty();
    parent.addClass("sheaf-root");
    this.container = parent;
  }

  update(state: ChatViewState, service: ChatService, app: App): void {
    if (!this.container) {
      return;
    }
    this.container.empty();

    const header = this.container.createDiv({ cls: "sheaf-header" });
    header.createDiv({ text: "Sheaf Chat", cls: "sheaf-header-title" });

    const actions = header.createDiv({ cls: "sheaf-header-actions" });

    const refreshBtn = actions.createEl("button", { cls: "sheaf-icon-btn" });
    setIcon(refreshBtn, "refresh-cw");
    refreshBtn.setAttribute("aria-label", "Refresh threads");
    refreshBtn.addEventListener("click", () => {
      void service.refreshThreads();
    });

    const settingsBtn = actions.createEl("button", { cls: "sheaf-icon-btn" });
    setIcon(settingsBtn, "settings");
    settingsBtn.setAttribute("aria-label", "Open settings");
    settingsBtn.addEventListener("click", () => {
      service.openSettings();
    });

    const newBtn = this.container.createEl("button", { cls: "sheaf-new-thread-btn" });
    const plusIcon = newBtn.createSpan();
    setIcon(plusIcon, "plus");
    newBtn.createSpan({ text: "New thread" });
    newBtn.disabled = state.threadList.creating;
    newBtn.setAttribute("aria-label", "Create a new thread");
    newBtn.addEventListener("click", () => {
      new NewThreadModal(app, (name) => {
        void service.createThread(name).catch((error) => {
          new Notice(error instanceof Error ? error.message : String(error));
        });
      }).open();
    });

    if (state.threadList.loading) {
      this.container.createDiv({ text: "Loading threads\u2026", cls: "sheaf-empty-state" });
      return;
    }

    if (state.threadList.errorMessage) {
      this.container.createDiv({ text: state.threadList.errorMessage, cls: "sheaf-error-text" });
    }

    if (state.threadList.threads.length === 0) {
      this.container.createDiv({
        text: "No threads yet. Create one to start chatting.",
        cls: "sheaf-empty-state",
      });
      return;
    }

    const list = this.container.createDiv({ cls: "sheaf-thread-list" });
    list.setAttribute("role", "list");
    list.setAttribute("aria-label", "Chat threads");

    for (const thread of state.threadList.threads) {
      const card = list.createEl("button", { cls: "sheaf-thread-card" });
      card.setAttribute("role", "listitem");
      card.createSpan({ text: thread.name || thread.thread_id, cls: "sheaf-thread-card-title" });

      const updated = formatWhen(thread.updated_at);
      if (updated) {
        card.createSpan({ text: `Updated ${updated}`, cls: "sheaf-thread-card-meta" });
      }

      card.addEventListener("click", () => {
        void service.openThread(thread.thread_id).catch((error) => {
          new Notice(error instanceof Error ? error.message : String(error));
        });
      });
    }
  }

  destroy(): void {
    this.container = null;
  }
}
