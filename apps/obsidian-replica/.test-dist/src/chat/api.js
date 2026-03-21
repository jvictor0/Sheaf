import { CHAT_PROTOCOL_VERSION, decodeCreateThreadResponse, decodeEnterChatResponse, decodeModelListResponse, decodeThreadsResponse, } from "./protocol.js";
async function requestObsidianUrl(args) {
    const obsidian = await import("obsidian");
    return obsidian.requestUrl(args);
}
export class ChatApiClient {
    settings;
    constructor(settings) {
        this.settings = settings;
    }
    async listThreads() {
        const response = await requestObsidianUrl({
            url: `${this.settings().serverBaseUrl}/threads`,
            method: "GET",
        });
        return decodeThreadsResponse(response.json);
    }
    async createThread(name) {
        const response = await requestObsidianUrl({
            url: `${this.settings().serverBaseUrl}/threads`,
            method: "POST",
            contentType: "application/json",
            body: JSON.stringify({ name }),
        });
        return decodeCreateThreadResponse(response.json);
    }
    async listModels() {
        const response = await requestObsidianUrl({
            url: `${this.settings().serverBaseUrl}/models`,
            method: "GET",
        });
        return decodeModelListResponse(response.json);
    }
    async enterThread(threadID, knownTailTurnID) {
        const response = await requestObsidianUrl({
            url: `${this.settings().serverBaseUrl}/threads/${encodeURIComponent(threadID)}/enter-chat`,
            method: "POST",
            contentType: "application/json",
            body: JSON.stringify({
                protocol_version: CHAT_PROTOCOL_VERSION,
                known_tail_turn_id: knownTailTurnID,
            }),
        });
        return decodeEnterChatResponse(response.json);
    }
}
