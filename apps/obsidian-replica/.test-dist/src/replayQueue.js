export class ReplayQueue {
    tail = Promise.resolve();
    enqueue(task, onError) {
        this.tail = this.tail
            .catch(() => undefined)
            .then(task)
            .catch(async (error) => {
            await onError(error);
        });
    }
}
