"""LangChain chat chain construction for sheaf."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Protocol

from langchain_core.messages import AIMessage, HumanMessage, SystemMessage, ToolMessage
from langchain_openai import ChatOpenAI

from sheaf.config.settings import TOME_DIR
from sheaf.tools import build_agent_tools


@dataclass
class ToolCallRecord:
    id: str
    name: str
    args: dict[str, Any]
    result: str
    is_error: bool


@dataclass
class ChatChainResult:
    response: str
    tool_calls: list[ToolCallRecord]


class HasRoleContent(Protocol):
    role: str
    content: str


def _to_langchain_messages(messages: Iterable[HasRoleContent]) -> list[object]:
    out: list[object] = []
    for msg in messages:
        role = msg.role.strip().lower()
        if role == "user":
            out.append(HumanMessage(content=msg.content))
        elif role == "assistant":
            out.append(AIMessage(content=msg.content))
        else:
            out.append(SystemMessage(content=msg.content))
    return out


def _base_system_prompt() -> str:
    return (
        "You are sheaf, a pragmatic assistant. Use they/them self-reference when relevant. "
        "When asked to persist notes or files, prefer using available tools. "
        "For persistent structured data, use list_sqlite_databases to discover DBs, "
        "create_sqlite_database to create named DBs, and run_sql(database_name, sql) to query them "
        "under the configured data/sqlite directory."
    )


def _system_prompt() -> str:
    path = Path(TOME_DIR) / "system_prompt.md"
    try:
        if path.exists():
            content = path.read_text(encoding="utf-8").strip()
            if content:
                return content
    except OSError:
        # Fall back to the default prompt if the file cannot be read.
        pass
    return _base_system_prompt()


def invoke_chat_chain(
    *,
    api_key: str,
    model: str,
    messages: Iterable[HasRoleContent],
    enable_tools: bool = True,
) -> ChatChainResult:
    llm = ChatOpenAI(model=model, api_key=api_key)
    history = _to_langchain_messages(messages)
    system_message = SystemMessage(content=_system_prompt())

    if not enable_tools:
        result = llm.invoke([system_message, *history])
        text = result.content.strip() if isinstance(result.content, str) else str(result.content).strip()
        if not text:
            raise RuntimeError("LangChain returned an empty response")
        return ChatChainResult(response=text, tool_calls=[])

    tools = build_agent_tools()
    llm_with_tools = llm.bind_tools(tools)
    tool_map = {tool.name: tool for tool in tools}

    max_rounds = 6
    collected_calls: list[ToolCallRecord] = []
    for _ in range(max_rounds):
        ai = llm_with_tools.invoke([system_message, *history])
        history.append(ai)
        tool_calls = ai.tool_calls or []
        if not tool_calls:
            text = ai.content.strip() if isinstance(ai.content, str) else str(ai.content).strip()
            if not text:
                raise RuntimeError("LangChain returned an empty response")
            return ChatChainResult(response=text, tool_calls=collected_calls)

        for call in tool_calls:
            tool_name = str(call.get("name", ""))
            tool_call_id = str(call.get("id", ""))
            args = call.get("args", {})
            tool = tool_map.get(tool_name)
            is_error = False

            if tool is None:
                content = f"Tool error: unknown tool '{tool_name}'"
                is_error = True
            else:
                try:
                    content = str(tool.invoke(args))
                except Exception as exc:  # noqa: BLE001
                    content = f"Tool error: {exc}"
                    is_error = True

            record_args = args if isinstance(args, dict) else {"value": args}
            collected_calls.append(
                ToolCallRecord(
                    id=tool_call_id,
                    name=tool_name,
                    args=record_args,
                    result=content,
                    is_error=is_error,
                )
            )
            history.append(ToolMessage(content=content, tool_call_id=tool_call_id))

    # Fallback: if the model keeps requesting tools, force a final answer without tools.
    result = llm.invoke([system_message, *history])
    text = result.content.strip() if isinstance(result.content, str) else str(result.content).strip()
    if not text:
        raise RuntimeError("LangChain returned an empty response")
    return ChatChainResult(response=text, tool_calls=collected_calls)
