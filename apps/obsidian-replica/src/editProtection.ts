import { Annotation, EditorState, TransactionSpec } from "@codemirror/state";

const REPLICA_SYSTEM_WRITE = Annotation.define<boolean>();

export function allowReplicaWrite(spec: TransactionSpec): TransactionSpec {
  const existing = spec.annotations
    ? Array.isArray(spec.annotations)
      ? spec.annotations
      : [spec.annotations]
    : [];
  return {
    ...spec,
    annotations: [...existing, REPLICA_SYSTEM_WRITE.of(true)],
  };
}

export function createReplicaEditBlocker(isReplicaProtectedFile: () => boolean) {
  return EditorState.transactionFilter.of((transaction) => {
    if (!transaction.docChanged) {
      return transaction;
    }
    if (transaction.annotation(REPLICA_SYSTEM_WRITE) === true) {
      return transaction;
    }
    if (!isReplicaProtectedFile()) {
      return transaction;
    }
    return [];
  });
}
