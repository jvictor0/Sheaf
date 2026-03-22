import { ItemView, WorkspaceLeaf } from "obsidian";

import type { ChatService } from "./service.js";
import type { ChatViewState } from "../types.js";
import { ThreadListComponent } from "./components/threadList.js";
import { ConversationComponent } from "./components/conversation.js";

export const SHEAF_CHAT_VIEW_TYPE = "sheaf-chat-view";

export class SheafChatView extends ItemView {
  private unsubscribe: (() => void) | null = null;
  private resizeObserver: ResizeObserver | null = null;
  private mountedScreen: ChatViewState["screen"] | null = null;
  private threadList = new ThreadListComponent();
  private conversation = new ConversationComponent();

  constructor(
    leaf: WorkspaceLeaf,
    private readonly chatService: ChatService,
  ) {
    super(leaf);
  }

  getViewType(): string {
    return SHEAF_CHAT_VIEW_TYPE;
  }

  getDisplayText(): string {
    return "Sheaf Chat";
  }

  async onOpen(): Promise<void> {
    this.contentEl.style.display = "flex";
    this.contentEl.style.flexDirection = "column";

    this.observeParentSize();

    this.unsubscribe = this.chatService.subscribe(() => {
      this.render(this.chatService.getSnapshot());
    });
    await this.chatService.activateView();
    this.render(this.chatService.getSnapshot());
  }

  async onClose(): Promise<void> {
    this.unsubscribe?.();
    this.unsubscribe = null;
    this.resizeObserver?.disconnect();
    this.resizeObserver = null;
    this.contentEl.style.display = "";
    this.contentEl.style.flexDirection = "";
    this.contentEl.style.height = "";
    this.threadList.destroy();
    this.conversation.destroy();
    this.mountedScreen = null;
    await this.chatService.deactivateView();
  }

  private render(state: ChatViewState): void {
    if (state.screen === "threads")
    {
      if (this.mountedScreen !== "threads")
      {
        this.conversation.destroy();
        this.contentEl.empty();
        this.contentEl.removeClass("sheaf-root");
        this.threadList.mount(this.contentEl);
        this.mountedScreen = "threads";
      }

      this.threadList.update(state, this.chatService, this.app);
      return;
    }

    if (this.mountedScreen !== "conversation")
    {
      this.threadList.destroy();
      this.contentEl.empty();
      this.contentEl.removeClass("sheaf-root");
      this.conversation.mount(this.contentEl, this.chatService);
      this.mountedScreen = "conversation";
    }

    this.conversation.update(state.activeSession);
  }

  // Obsidian's parent container doesn't give contentEl a CSS-definite
  // height (it's sized by flex layout, not an explicit height property).
  // We observe the parent's actual size and set an explicit pixel height
  // on contentEl so inner flex children can resolve flex-grow.
  //
  private observeParentSize(): void {
    const parent = this.contentEl.parentElement;
    if (!parent)
    {
      return;
    }

    let lastHeight = "";

    const sync = () => {
      const parentRect = parent.getBoundingClientRect();
      const elRect = this.contentEl.getBoundingClientRect();
      const offset = elRect.top - parentRect.top;
      const available = parentRect.height - offset;
      const h = `${Math.max(200, Math.round(available))}px`;
      if (h !== lastHeight)
      {
        this.contentEl.style.height = h;
        lastHeight = h;
      }
    };

    this.resizeObserver = new ResizeObserver(sync);
    this.resizeObserver.observe(parent);
    sync();
  }
}
