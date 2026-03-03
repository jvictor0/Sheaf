"""LangChain chat chain construction for sheaf."""

from __future__ import annotations

from typing import Iterable, Protocol

from langchain_core.messages import AIMessage, HumanMessage, SystemMessage, ToolMessage
from langchain_openai import ChatOpenAI

from sheaf.tools import build_agent_tools

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
        "When asked to persist notes or files, prefer using available tools."
    )


def invoke_chat_chain(
    *,
    api_key: str,
    model: str,
    messages: Iterable[HasRoleContent],
    enable_tools: bool = True,
) -> str:
    llm = ChatOpenAI(model=model, api_key=api_key)
    history = _to_langchain_messages(messages)
    system_message = SystemMessage(content=_base_system_prompt())

    if not enable_tools:
        result = llm.invoke([system_message, *history])
        text = result.content.strip() if isinstance(result.content, str) else str(result.content).strip()
        if not text:
            raise RuntimeError("LangChain returned an empty response")
        return text

    tools = build_agent_tools()
    llm_with_tools = llm.bind_tools(tools)
    tool_map = {tool.name: tool for tool in tools}

    max_rounds = 6
    for _ in range(max_rounds):
        ai = llm_with_tools.invoke([system_message, *history])
        history.append(ai)
        tool_calls = ai.tool_calls or []
        if not tool_calls:
            text = ai.content.strip() if isinstance(ai.content, str) else str(ai.content).strip()
            if not text:
                raise RuntimeError("LangChain returned an empty response")
            return text

        for call in tool_calls:
            tool_name = str(call.get("name", ""))
            tool_call_id = str(call.get("id", ""))
            args = call.get("args", {})
            tool = tool_map.get(tool_name)

            if tool is None:
                content = f"Tool error: unknown tool '{tool_name}'"
            else:
                try:
                    content = str(tool.invoke(args))
                except Exception as exc:  # noqa: BLE001
                    content = f"Tool error: {exc}"
            history.append(ToolMessage(content=content, tool_call_id=tool_call_id))

    # Fallback: if the model keeps requesting tools, force a final answer without tools.
    result = llm.invoke([system_message, *history])
    text = result.content.strip() if isinstance(result.content, str) else str(result.content).strip()
    if not text:
        raise RuntimeError("LangChain returned an empty response")
    return text
