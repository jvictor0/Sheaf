"""LangGraph chat runtime with per-chat SQLite checkpoints."""

from __future__ import annotations

from typing import Annotated, TypedDict

from langchain_core.messages import AIMessage, BaseMessage, RemoveMessage
from langgraph.checkpoint.sqlite import SqliteSaver
from langgraph.graph import END, START, StateGraph
from langgraph.graph.message import add_messages

from sheaf.llm.dispatcher import Message, build_dispatcher
from sheaf.llm.model_properties import ModelLimits


class ChatState(TypedDict):
    messages: Annotated[list[BaseMessage], add_messages]
    rolling_summary: str
    compaction_count: int


def _message_content_to_text(content: object) -> str:
    if isinstance(content, str):
        return content
    return str(content)


def _to_dispatcher_messages(messages: list[BaseMessage]) -> list[Message]:
    out: list[Message] = []
    for msg in messages:
        role = getattr(msg, "type", "system")
        if role == "human":
            mapped = "user"
        elif role == "ai":
            mapped = "assistant"
        else:
            mapped = "system"
        out.append(Message(role=mapped, content=_message_content_to_text(getattr(msg, "content", ""))))
    return out


def _estimate_token_count(*, limits: ModelLimits, messages: list[BaseMessage], summary: str) -> int:
    # Lightweight approximation to avoid extra tokenizer dependencies.
    total = 0
    for msg in messages:
        content = _message_content_to_text(getattr(msg, "content", ""))
        total += 4 + max(1, len(content) // 4)
    total += max(1, len(summary) // 4)
    total += limits.reserved_output_tokens + limits.safety_margin_tokens
    return total


def compile_chat_graph(*, saver: SqliteSaver):
    dispatcher = build_dispatcher()
    limits = dispatcher.model_properties.limits

    def maybe_compact(state: ChatState) -> dict[str, object]:
        messages = state.get("messages", [])
        if not isinstance(messages, list) or len(messages) < (limits.recent_messages_to_keep + 2):
            return {}

        existing_summary = str(state.get("rolling_summary", "")).strip()
        estimated = _estimate_token_count(limits=limits, messages=messages, summary=existing_summary)
        trigger_budget = int(limits.context_window_tokens * limits.compaction_trigger_ratio)
        if estimated < trigger_budget:
            return {}

        # Keep recent turns under the target budget and summarize older turns.
        history_budget = int(limits.context_window_tokens * limits.compaction_target_ratio)
        history_budget -= limits.reserved_output_tokens + limits.safety_margin_tokens
        history_budget = max(1, history_budget)

        keep = 0
        running = 0
        min_keep = max(2, limits.recent_messages_to_keep)
        for msg in reversed(messages):
            msg_tokens = 4 + max(1, len(_message_content_to_text(getattr(msg, "content", ""))) // 4)
            if keep < min_keep or (running + msg_tokens) <= history_budget:
                running += msg_tokens
                keep += 1
                continue
            break

        keep = min(len(messages), max(2, keep))
        old_messages = messages[:-keep]
        if not old_messages:
            return {}

        remove_ops: list[RemoveMessage] = []
        for msg in old_messages:
            msg_id = getattr(msg, "id", None)
            if not isinstance(msg_id, str) or not msg_id:
                return {}
            remove_ops.append(RemoveMessage(id=msg_id, content=""))

        transcript_lines: list[str] = []
        for item in _to_dispatcher_messages(old_messages):
            transcript_lines.append(f"{item.role}: {item.content}")
        transcript = "\n".join(transcript_lines)

        summary_prompt: list[Message] = [
            Message(
                role="system",
                content=(
                    "Update the rolling conversation summary. Keep key facts, decisions, tasks, "
                    "constraints, and open questions. Keep it concise and factual."
                ),
            )
        ]
        if existing_summary:
            summary_prompt.append(
                Message(role="system", content=f"Existing rolling summary:\n{existing_summary}")
            )
        summary_prompt.append(Message(role="user", content=f"Older transcript to compress:\n{transcript}"))

        new_summary = dispatcher.generate(summary_prompt, enable_tools=False).strip()
        if not new_summary:
            return {}

        return {
            "messages": remove_ops,
            "rolling_summary": new_summary,
            "compaction_count": int(state.get("compaction_count", 0)) + 1,
        }

    def assistant_turn(state: ChatState) -> dict[str, list[BaseMessage]]:
        history = _to_dispatcher_messages(state.get("messages", []))
        rolling_summary = str(state.get("rolling_summary", "")).strip()
        if rolling_summary:
            history = [
                Message(role="system", content=f"Rolling conversation summary:\n{rolling_summary}"),
                *history,
            ]
        generation = dispatcher.generate_with_details(history)
        tool_calls = [
            {
                "id": call.id,
                "name": call.name,
                "args": call.args,
                "result": call.result,
                "is_error": call.is_error,
            }
            for call in generation.tool_calls
        ]
        return {
            "messages": [
                AIMessage(
                    content=generation.response,
                    additional_kwargs={"tool_calls_made": tool_calls},
                )
            ]
        }

    builder = StateGraph(ChatState)
    builder.add_node("maybe_compact", maybe_compact)
    builder.add_node("assistant_turn", assistant_turn)
    builder.add_edge(START, "maybe_compact")
    builder.add_edge("maybe_compact", "assistant_turn")
    builder.add_edge("assistant_turn", END)
    return builder.compile(checkpointer=saver)
