export const CHAT_PROTOCOL_VERSION = 1;
function isObject(value) {
    return typeof value === "object" && value !== null;
}
function asString(value) {
    return typeof value === "string" ? value : null;
}
function asBoolean(value) {
    return value === true;
}
export function normalizeJSONValue(value) {
    if (value === null ||
        typeof value === "string" ||
        typeof value === "number" ||
        typeof value === "boolean") {
        return value;
    }
    if (Array.isArray(value)) {
        return value.map((item) => normalizeJSONValue(item));
    }
    if (isObject(value)) {
        const output = {};
        for (const [key, item] of Object.entries(value)) {
            output[key] = normalizeJSONValue(item);
        }
        return output;
    }
    return String(value);
}
export function decodeThreadSummary(value) {
    if (!isObject(value) || !asString(value.thread_id)) {
        throw new Error("Invalid thread summary");
    }
    const threadID = asString(value.thread_id) ?? "";
    return {
        thread_id: threadID,
        name: asString(value.name) ?? threadID,
        prev_thread_id: asString(value.prev_thread_id),
        start_turn_id: asString(value.start_turn_id),
        is_archived: asBoolean(value.is_archived),
        tail_turn_id: asString(value.tail_turn_id),
        created_at: asString(value.created_at),
        updated_at: asString(value.updated_at),
    };
}
export function decodeThreadsResponse(value) {
    if (!isObject(value) || !Array.isArray(value.threads)) {
        throw new Error("Invalid thread list response");
    }
    return value.threads.map((thread) => decodeThreadSummary(thread));
}
export function decodeCreateThreadResponse(value) {
    if (!isObject(value) || !asString(value.thread_id)) {
        throw new Error("Invalid create thread response");
    }
    return { thread_id: asString(value.thread_id) ?? "" };
}
export function decodeEnterChatResponse(value) {
    if (!isObject(value) ||
        !asString(value.session_id) ||
        !asString(value.websocket_url) ||
        typeof value.accepted_protocol_version !== "number") {
        throw new Error("Invalid enter-chat response");
    }
    return {
        session_id: asString(value.session_id) ?? "",
        websocket_url: asString(value.websocket_url) ?? "",
        accepted_protocol_version: value.accepted_protocol_version,
    };
}
export function decodeCommittedTurn(value) {
    if (!isObject(value) || !asString(value.id) || !asString(value.thread_id) || !asString(value.speaker)) {
        throw new Error("Invalid committed turn");
    }
    const rawToolCalls = Array.isArray(value.tool_calls) ? value.tool_calls : [];
    const toolCalls = rawToolCalls
        .filter((toolCall) => isObject(toolCall))
        .map((toolCall) => ({
        id: asString(toolCall.id) ?? "",
        name: asString(toolCall.name) ?? "tool",
        args: isObject(toolCall.args)
            ? Object.fromEntries(Object.entries(toolCall.args).map(([key, item]) => [key, normalizeJSONValue(item)]))
            : {},
        result: asString(toolCall.result) ?? "",
        isError: asBoolean(toolCall.is_error),
    }));
    return {
        id: asString(value.id) ?? "",
        thread_id: asString(value.thread_id) ?? "",
        prev_turn_id: asString(value.prev_turn_id),
        speaker: asString(value.speaker) ?? "system",
        message_text: asString(value.message_text) ?? "",
        model_name: asString(value.model_name),
        created_at: asString(value.created_at),
        tool_calls: toolCalls,
    };
}
export function decodeModelListResponse(value) {
    if (!isObject(value) || !Array.isArray(value.models)) {
        throw new Error("Invalid models response");
    }
    return value.models
        .filter((item) => isObject(item) && typeof item.name === "string")
        .map((item) => ({
        name: asString(item.name) ?? "",
        provider: asString(item.provider) ?? "unknown",
        is_default: asBoolean(item.is_default),
    }));
}
