export function normalizeReplicaPath(path) {
    return path.replace(/\\/g, "/").replace(/^\/+/, "").replace(/\/+/g, "/");
}
