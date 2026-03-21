"""Minimal local tool wrapper used by Sheaf without LangChain dependencies."""

from __future__ import annotations

import inspect
from dataclasses import dataclass
from typing import Any, Callable, get_args, get_origin, get_type_hints


@dataclass(frozen=True)
class SimpleTool:
    name: str
    description: str
    parameters_schema: dict[str, Any]
    _func: Callable[..., str]

    def invoke(self, payload: dict[str, Any] | None = None) -> str:
        args = payload or {}
        return self._func(**args)


def _json_schema_type(annotation: Any) -> str:
    origin = get_origin(annotation)
    if origin is None:
        if annotation is bool:
            return "boolean"
        if annotation is int:
            return "integer"
        if annotation is float:
            return "number"
        return "string"
    if origin is list:
        return "array"
    if origin is dict:
        return "object"
    if origin is tuple:
        return "array"
    if origin is set:
        return "array"
    if origin is type(None):
        return "null"
    args = [arg for arg in get_args(annotation) if arg is not type(None)]
    if args:
        return _json_schema_type(args[0])
    return "string"


def _derive_parameters_schema(func: Callable[..., str]) -> dict[str, Any]:
    sig = inspect.signature(func)
    hints = get_type_hints(func)
    properties: dict[str, Any] = {}
    required: list[str] = []

    for name, param in sig.parameters.items():
        if param.kind in {inspect.Parameter.VAR_POSITIONAL, inspect.Parameter.VAR_KEYWORD}:
            continue
        annotation = hints.get(name, str)
        properties[name] = {"type": _json_schema_type(annotation)}
        if param.default is inspect.Parameter.empty:
            required.append(name)

    schema: dict[str, Any] = {
        "type": "object",
        "properties": properties,
    }
    if required:
        schema["required"] = required
    return schema


def tool(
    name: str,
    *,
    description: str | None = None,
    parameters_schema: dict[str, Any] | None = None,
) -> Callable[[Callable[..., str]], SimpleTool]:
    def _decorate(func: Callable[..., str]) -> SimpleTool:
        resolved_description = (description or (func.__doc__ or "")).strip() or f"Invoke tool '{name}'."
        resolved_schema = parameters_schema or _derive_parameters_schema(func)
        return SimpleTool(
            name=name,
            description=resolved_description,
            parameters_schema=resolved_schema,
            _func=func,
        )

    return _decorate
