import { ItemView, Modal, Notice, Setting } from "obsidian";
export const SHEAF_CHAT_VIEW_TYPE = "sheaf-chat-view";
const DEFAULT_NEW_THREAD_NAME = "New thread";
class NewThreadModal extends Modal {
    onSubmit;
    name = DEFAULT_NEW_THREAD_NAME;
    constructor(app, onSubmit) {
        super(app);
        this.onSubmit = onSubmit;
    }
    onOpen() {
        const { contentEl } = this;
        contentEl.empty();
        contentEl.createEl("h3", { text: "New thread" });
        let inputEl = null;
        new Setting(contentEl)
            .setName("Thread name")
            .addText((text) => {
            inputEl = text.inputEl;
            text.setValue(this.name).onChange((value) => {
                this.name = value;
            });
        });
        const actions = contentEl.createDiv({ cls: "sheaf-chat-actions" });
        const cancel = actions.createEl("button", { text: "Cancel", cls: "sheaf-chat-icon-button" });
        cancel.addEventListener("click", () => this.close());
        const create = actions.createEl("button", { text: "Create", cls: "sheaf-chat-action" });
        create.addEventListener("click", () => {
            this.onSubmit(this.name);
            this.close();
        });
        window.setTimeout(() => inputEl?.focus(), 0);
    }
}
function ensureStyles(doc) {
    if (doc.getElementById("sheaf-chat-styles")) {
        return;
    }
    const style = doc.createElement("style");
    style.id = "sheaf-chat-styles";
    style.textContent = `
    .sheaf-chat-view { display: flex; flex-direction: column; height: 100%; gap: 12px; padding: 12px; }
    .sheaf-chat-header, .sheaf-chat-conversation-header { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
    .sheaf-chat-title { font-weight: 700; }
    .sheaf-chat-subtle { color: var(--text-muted); font-size: 0.9em; }
    .sheaf-chat-thread-list, .sheaf-chat-transcript { display: flex; flex-direction: column; gap: 10px; overflow-y: auto; min-height: 0; }
    .sheaf-chat-thread-item, .sheaf-chat-action, .sheaf-chat-icon-button, .sheaf-chat-back { border: 1px solid var(--background-modifier-border); background: var(--background-secondary); border-radius: 10px; padding: 12px 12px; cursor: pointer; }
    .sheaf-chat-thread-item { display: flex; flex-direction: column; align-items: flex-start; justify-content: center; text-align: left; width: 100%; gap: 5px; white-space: normal; min-height: 56px; }
    .sheaf-chat-thread-title { display: block; font-weight: 700; }
    .sheaf-chat-thread-item:hover, .sheaf-chat-action:hover, .sheaf-chat-icon-button:hover, .sheaf-chat-back:hover { background: var(--background-modifier-hover); }
    .sheaf-chat-actions { display: flex; gap: 8px; }
    .sheaf-chat-thread-meta { display: block; font-size: 0.82em; }
    .sheaf-chat-bubble { border-radius: 14px; padding: 10px 12px; white-space: pre-wrap; line-height: 1.45; }
    .sheaf-chat-bubble-user { align-self: flex-end; background: var(--interactive-accent); color: var(--text-on-accent); max-width: 85%; }
    .sheaf-chat-bubble-assistant { align-self: flex-start; background: var(--background-secondary); max-width: 92%; }
    .sheaf-chat-bubble-system { align-self: stretch; background: var(--background-modifier-hover); }
    .sheaf-chat-tool { align-self: flex-start; background: var(--background-primary-alt); border: 1px solid var(--background-modifier-border); color: var(--text-muted); font-size: 0.9em; }
    .sheaf-chat-tool-error { color: var(--text-error); }
    .sheaf-chat-streaming::after { content: " "; display: inline-block; width: 0.75em; height: 0.75em; margin-left: 6px; border-radius: 999px; background: var(--interactive-accent); animation: sheaf-chat-pulse 1s infinite ease-in-out; vertical-align: middle; }
    .sheaf-chat-status { color: var(--text-muted); font-size: 0.9em; min-height: 1.2em; }
    .sheaf-chat-error { color: var(--text-error); }
    .sheaf-chat-composer { display: flex; flex-direction: column; gap: 8px; }
    .sheaf-chat-textarea { width: 100%; min-height: 72px; resize: vertical; border-radius: 12px; padding: 10px 12px; border: 1px solid var(--background-modifier-border); background: var(--background-primary); }
    .sheaf-chat-send-row { display: flex; justify-content: space-between; align-items: center; gap: 8px; }
    @keyframes sheaf-chat-pulse { 0% { opacity: 0.3; transform: scale(0.8);} 50% { opacity: 1; transform: scale(1);} 100% { opacity: 0.3; transform: scale(0.8);} }
  `;
    doc.head.appendChild(style);
}
function formatWhen(value) {
    if (!value) {
        return null;
    }
    const parsed = new Date(value);
    if (Number.isNaN(parsed.getTime())) {
        return value;
    }
    return parsed.toLocaleString();
}
export class SheafChatView extends ItemView {
    chatService;
    unsubscribe = null;
    composerValue = "";
    mountedScreen = null;
    mountedThreadID = null;
    threadListEl = null;
    conversationEl = null;
    conversationTitleEl = null;
    conversationSubtitleEl = null;
    transcriptEl = null;
    statusEl = null;
    composerEl = null;
    sendButtonEl = null;
    transcriptRowMap = new Map();
    lastTranscriptSnapshot = new Map();
    lastStatusText = "";
    constructor(leaf, chatService) {
        super(leaf);
        this.chatService = chatService;
    }
    getViewType() {
        return SHEAF_CHAT_VIEW_TYPE;
    }
    getDisplayText() {
        return "Sheaf Chat";
    }
    async onOpen() {
        ensureStyles(document);
        this.unsubscribe = this.chatService.subscribe(() => {
            this.render(this.chatService.getSnapshot());
        });
        await this.chatService.activateView();
        this.render(this.chatService.getSnapshot());
    }
    async onClose() {
        this.unsubscribe?.();
        this.unsubscribe = null;
        await this.chatService.deactivateView();
    }
    render(state) {
        if (state.screen === "threads") {
            this.renderThreadList(state);
            return;
        }
        this.renderConversation(state);
    }
    renderThreadList(state) {
        const { contentEl } = this;
        if (this.mountedScreen !== "threads") {
            contentEl.empty();
            contentEl.addClass("sheaf-chat-view");
            this.threadListEl = contentEl.createDiv({ cls: "sheaf-chat-view" });
            this.conversationEl = null;
            this.mountedScreen = "threads";
            this.mountedThreadID = null;
            this.transcriptRowMap.clear();
            this.lastTranscriptSnapshot.clear();
            this.lastStatusText = "";
        }
        const container = this.threadListEl;
        if (!container) {
            return;
        }
        container.empty();
        const header = container.createDiv({ cls: "sheaf-chat-header" });
        header.createDiv({ text: "Sheaf Chat", cls: "sheaf-chat-title" });
        const actions = header.createDiv({ cls: "sheaf-chat-actions" });
        const refreshButton = actions.createEl("button", { text: "Refresh", cls: "sheaf-chat-icon-button" });
        refreshButton.addEventListener("click", () => {
            void this.chatService.refreshThreads();
        });
        const settingsButton = actions.createEl("button", { text: "Gear", cls: "sheaf-chat-icon-button" });
        settingsButton.setAttribute("aria-label", "Open chat settings");
        settingsButton.addEventListener("click", () => {
            this.chatService.openSettings();
        });
        const newThreadButton = container.createEl("button", { text: "New thread", cls: "sheaf-chat-action" });
        newThreadButton.disabled = state.threadList.creating;
        newThreadButton.addEventListener("click", () => {
            new NewThreadModal(this.app, (name) => {
                void this.chatService.createThread(name).catch((error) => {
                    new Notice(error instanceof Error ? error.message : String(error));
                });
            }).open();
        });
        if (state.threadList.loading) {
            container.createDiv({ text: "Loading threads…", cls: "sheaf-chat-subtle" });
            return;
        }
        if (state.threadList.errorMessage) {
            container.createDiv({ text: state.threadList.errorMessage, cls: "sheaf-chat-error" });
        }
        if (state.threadList.threads.length === 0) {
            container.createDiv({ text: "No threads yet. Create one to start chatting.", cls: "sheaf-chat-subtle" });
            return;
        }
        const list = container.createDiv({ cls: "sheaf-chat-thread-list" });
        for (const thread of state.threadList.threads) {
            const button = list.createEl("button", { cls: "sheaf-chat-thread-item" });
            button.createSpan({ text: thread.name || thread.thread_id, cls: "sheaf-chat-thread-title" });
            const updated = formatWhen(thread.updated_at);
            if (updated) {
                button.createSpan({ text: `Updated ${updated}`, cls: "sheaf-chat-subtle sheaf-chat-thread-meta" });
            }
            button.addEventListener("click", () => {
                void this.chatService.openThread(thread.thread_id).catch((error) => {
                    new Notice(error instanceof Error ? error.message : String(error));
                });
            });
        }
    }
    renderConversation(state) {
        const session = state.activeSession;
        const { contentEl } = this;
        if (this.mountedScreen !== "conversation") {
            contentEl.empty();
            contentEl.addClass("sheaf-chat-view");
            this.buildConversationShell(contentEl);
            this.threadListEl = null;
            this.mountedScreen = "conversation";
            this.mountedThreadID = null;
            this.transcriptRowMap.clear();
            this.lastTranscriptSnapshot.clear();
            this.lastStatusText = "";
        }
        if (!session) {
            this.conversationEl?.empty();
            this.conversationEl?.createDiv({ text: "No thread selected.", cls: "sheaf-chat-subtle" });
            return;
        }
        if (this.mountedThreadID !== session.thread.thread_id) {
            this.resetTranscript();
            this.mountedThreadID = session.thread.thread_id;
        }
        if (this.conversationTitleEl) {
            this.conversationTitleEl.setText(session.thread.name || session.thread.thread_id);
        }
        if (this.conversationSubtitleEl) {
            const updated = formatWhen(session.thread.updated_at);
            this.conversationSubtitleEl.setText(updated ? `Updated ${updated}` : "");
        }
        this.syncTranscript(session);
        this.syncStatus(session);
        this.syncComposer(session);
    }
    buildConversationShell(container) {
        const conversation = container.createDiv({ cls: "sheaf-chat-view" });
        this.conversationEl = conversation;
        const header = conversation.createDiv({ cls: "sheaf-chat-conversation-header" });
        const back = header.createEl("button", { text: "Back", cls: "sheaf-chat-back" });
        back.addEventListener("click", () => {
            void this.chatService.showThreadList();
        });
        const titleWrap = header.createDiv();
        this.conversationTitleEl = titleWrap.createDiv({ cls: "sheaf-chat-title" });
        this.conversationSubtitleEl = titleWrap.createDiv({ cls: "sheaf-chat-subtle" });
        const settingsButton = header.createEl("button", { text: "Gear", cls: "sheaf-chat-icon-button" });
        settingsButton.setAttribute("aria-label", "Open chat settings");
        settingsButton.addEventListener("click", () => {
            this.chatService.openSettings();
        });
        this.transcriptEl = conversation.createDiv({ cls: "sheaf-chat-transcript" });
        this.statusEl = conversation.createDiv({ cls: "sheaf-chat-status" });
        const composer = conversation.createDiv({ cls: "sheaf-chat-composer" });
        this.composerEl = composer.createEl("textarea", {
            cls: "sheaf-chat-textarea",
            attr: { placeholder: "Message Sheaf…" },
        });
        this.composerEl.addEventListener("input", () => {
            this.composerValue = this.composerEl?.value ?? "";
        });
        this.composerEl.addEventListener("keydown", (event) => {
            if (event.key === "Enter" && !event.shiftKey) {
                event.preventDefault();
                void this.submitComposer();
            }
        });
        const sendRow = composer.createDiv({ cls: "sheaf-chat-send-row" });
        sendRow.createDiv({ text: "Enter sends, Shift+Enter adds a new line.", cls: "sheaf-chat-subtle" });
        this.sendButtonEl = sendRow.createEl("button", { text: "Send", cls: "sheaf-chat-action" });
        this.sendButtonEl.addEventListener("click", () => {
            void this.submitComposer();
        });
    }
    syncComposer(session) {
        if (!this.composerEl) {
            return;
        }
        const canSend = session.connectionState === "live";
        this.composerEl.disabled = !canSend;
        this.composerEl.placeholder = canSend ? "Message Sheaf…" : "Loading chat history…";
        if (this.sendButtonEl) {
            this.sendButtonEl.disabled = !canSend;
        }
        if (this.composerEl.value !== this.composerValue && document.activeElement !== this.composerEl) {
            this.composerEl.value = this.composerValue;
        }
    }
    async submitComposer() {
        const value = this.composerEl?.value ?? "";
        const sent = await this.chatService.sendMessage(value);
        if (sent) {
            this.clearComposer();
        }
    }
    clearComposer() {
        this.composerValue = "";
        if (!this.composerEl) {
            return;
        }
        this.composerEl.value = "";
        this.composerEl.focus();
    }
    syncStatus(session) {
        if (!this.statusEl) {
            return;
        }
        const statusText = session.errorMessage ??
            session.statusMessage ??
            (session.thinkingActive ? "Thinking…" : session.connectionState === "connecting" ? "Connecting…" : "");
        if (statusText === this.lastStatusText) {
            return;
        }
        this.lastStatusText = statusText;
        this.statusEl.setText(statusText);
        this.statusEl.className = `sheaf-chat-status${session.errorMessage ? " sheaf-chat-error" : ""}`;
    }
    syncTranscript(session) {
        if (!this.transcriptEl) {
            return;
        }
        const visibleSnapshot = new Map();
        for (const item of session.transcriptItems) {
            visibleSnapshot.set(item.id, this.itemSignature(item));
        }
        let transcriptChanged = visibleSnapshot.size !== this.lastTranscriptSnapshot.size;
        if (!transcriptChanged) {
            for (const [id, signature] of visibleSnapshot.entries()) {
                if (this.lastTranscriptSnapshot.get(id) !== signature) {
                    transcriptChanged = true;
                    break;
                }
            }
        }
        if (!transcriptChanged) {
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
                this.updateTranscriptItem(existing, item);
                this.transcriptEl.appendChild(existing);
                continue;
            }
            const node = this.renderTranscriptItem(item);
            this.transcriptRowMap.set(item.id, node);
            this.transcriptEl.appendChild(node);
        }
        this.lastTranscriptSnapshot = visibleSnapshot;
        if (wasNearBottom) {
            this.transcriptEl.scrollTop = this.transcriptEl.scrollHeight;
        }
    }
    resetTranscript() {
        this.transcriptEl?.empty();
        this.transcriptRowMap.clear();
        this.lastTranscriptSnapshot.clear();
    }
    isNearBottom(element) {
        return element.scrollHeight - element.scrollTop - element.clientHeight < 24;
    }
    itemSignature(item) {
        switch (item.kind) {
            case "tool_call":
                return `${item.kind}:${item.text}:${item.tone}`;
            case "streaming":
                return `${item.kind}:${item.text}:${item.queueID}`;
            default:
                return `${item.kind}:${item.role}:${item.text}`;
        }
    }
    updateTranscriptItem(element, item) {
        if (element.dataset.signature === this.itemSignature(item)) {
            return;
        }
        element.className = "";
        element.textContent = "";
        const replacement = this.renderTranscriptItem(item);
        element.className = replacement.className;
        element.textContent = replacement.textContent;
        element.dataset.signature = replacement.dataset.signature;
    }
    renderTranscriptItem(item) {
        const element = document.createElement("div");
        element.dataset.signature = this.itemSignature(item);
        if (item.kind === "tool_call") {
            element.addClass("sheaf-chat-bubble", "sheaf-chat-tool");
            if (item.tone === "error") {
                element.addClass("sheaf-chat-tool-error");
            }
            element.setText(item.text);
            return element;
        }
        element.addClass("sheaf-chat-bubble");
        if (item.role === "user") {
            element.addClass("sheaf-chat-bubble-user");
        }
        else if (item.role === "assistant") {
            element.addClass("sheaf-chat-bubble-assistant");
        }
        else {
            element.addClass("sheaf-chat-bubble-system");
        }
        if (item.kind === "streaming") {
            element.addClass("sheaf-chat-streaming");
        }
        element.setText(item.text);
        return element;
    }
}
