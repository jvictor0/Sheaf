const FILE_TOOL_LABELS = {
    read_note: { verb: "Read", fallback: "Read file" },
    read_file: { verb: "Read", fallback: "Read file" },
    write_note: { verb: "Wrote", fallback: "Wrote file" },
    create_file: { verb: "Created", fallback: "Created file" },
    apply_patch: { verb: "Applied patch to", fallback: "Applied patch" },
    patch_note: { verb: "Patched", fallback: "Patched file" },
};
const PATH_KEYS = ["relative_path", "path", "file_path", "filepath", "target_path", "note_path"];
function asString(value) {
    return typeof value === "string" ? value : null;
}
function basename(path) {
    const trimmed = path.replace(/\\/g, "/").replace(/\/+$/, "");
    const parts = trimmed.split("/").filter(Boolean);
    return parts.length > 0 ? parts[parts.length - 1] : trimmed;
}
function pickPath(args) {
    for (const key of PATH_KEYS) {
        const candidate = asString(args[key]);
        if (candidate && candidate.trim()) {
            return candidate.trim();
        }
    }
    return null;
}
function formatPath(path) {
    const normalized = path.replace(/\\/g, "/");
    const vaultRelativeMatch = normalized.match(/(?:^|\/)data\/vaults\/[^/]+\/(.+)$/);
    if (vaultRelativeMatch?.[1]) {
        return vaultRelativeMatch[1];
    }
    if (!normalized.startsWith("/") && !/^[A-Za-z]:[\\/]/.test(normalized)) {
        return normalized;
    }
    return basename(normalized);
}
export function summarizeToolCall(call) {
    const label = FILE_TOOL_LABELS[call.name];
    const path = pickPath(call.args);
    if (label) {
        if (path) {
            return `${label.verb} ${formatPath(path)}`;
        }
        return label.fallback;
    }
    if (call.name === "list_notes") {
        const dir = asString(call.args.relative_dir) ?? asString(call.args.path);
        if (dir && dir.trim()) {
            return `Listed ${formatPath(dir.trim())}`;
        }
        return "Listed directory";
    }
    const fallbackName = call.name.replace(/_/g, " ").trim() || "tool";
    return `${call.isError ? "Tool failed" : "Used"} ${fallbackName}`;
}
