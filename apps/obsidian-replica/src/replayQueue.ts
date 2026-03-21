export class ReplayQueue {
  private tail: Promise<void> = Promise.resolve();

  enqueue(task: () => Promise<void>, onError: (error: unknown) => Promise<void> | void): void {
    this.tail = this.tail
      .catch(() => undefined)
      .then(task)
      .catch(async (error) => {
        await onError(error);
      });
  }
}
