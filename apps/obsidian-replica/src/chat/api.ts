import {
  CHAT_PROTOCOL_VERSION,
  decodeCreateThreadResponse,
  decodeEnterChatResponse,
  decodeModelListResponse,
  decodeThreadsResponse,
} from "./protocol.js";

import type { ChatEnterResponse, ChatModelOption, ChatThreadSummary, ReplicaPluginSettings } from "../types.js";

type RequestUrlArgs = {
  url: string;
  method: string;
  contentType?: string;
  body?: string;
};

async function requestObsidianUrl(args: RequestUrlArgs): Promise<{ json: unknown }> {
  const obsidian = await import("obsidian");
  return obsidian.requestUrl(args);
}

export class ChatApiClient {
  constructor(private readonly settings: () => ReplicaPluginSettings) {}

  async listThreads(): Promise<ChatThreadSummary[]> {
    const response = await requestObsidianUrl({
      url: `${this.settings().serverBaseUrl}/threads`,
      method: "GET",
    });
    return decodeThreadsResponse(response.json);
  }

  async createThread(name: string): Promise<{ thread_id: string }> {
    const response = await requestObsidianUrl({
      url: `${this.settings().serverBaseUrl}/threads`,
      method: "POST",
      contentType: "application/json",
      body: JSON.stringify({ name }),
    });
    return decodeCreateThreadResponse(response.json);
  }

  async listModels(): Promise<ChatModelOption[]> {
    const response = await requestObsidianUrl({
      url: `${this.settings().serverBaseUrl}/models`,
      method: "GET",
    });
    return decodeModelListResponse(response.json);
  }

  async enterThread(threadID: string, knownTailTurnID: string | null): Promise<ChatEnterResponse> {
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
