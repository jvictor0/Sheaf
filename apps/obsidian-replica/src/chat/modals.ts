import { App, Modal, Setting } from "obsidian";

const DEFAULT_NEW_THREAD_NAME = "New thread";

export class NewThreadModal extends Modal {
  private name = DEFAULT_NEW_THREAD_NAME;

  constructor(
    app: App,
    private readonly onSubmit: (name: string) => void,
  ) {
    super(app);
  }

  onOpen(): void {
    const { contentEl } = this;
    contentEl.empty();
    this.titleEl.setText("New thread");

    let inputEl: HTMLInputElement | null = null;
    new Setting(contentEl)
      .setName("Thread name")
      .addText((text) => {
        inputEl = text.inputEl;
        text.setValue(this.name).onChange((value) => {
          this.name = value;
        });
        text.inputEl.addEventListener("keydown", (event) => {
          if (event.key === "Enter") {
            event.preventDefault();
            this.onSubmit(this.name);
            this.close();
          }
        });
      });

    const actions = contentEl.createDiv({ cls: "sheaf-modal-actions" });
    actions
      .createEl("button", { text: "Cancel" })
      .addEventListener("click", () => this.close());
    actions
      .createEl("button", { text: "Create", cls: "mod-cta" })
      .addEventListener("click", () => {
        this.onSubmit(this.name);
        this.close();
      });

    window.setTimeout(() => inputEl?.focus(), 0);
  }
}
