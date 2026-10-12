"""Tests for mapping quest runner JSONL logs into AGUI events."""

from __future__ import annotations

import json
import unittest
from collections import Counter
from pathlib import Path
from typing import Any

from quest_runner_service.agui_mapper import (  # type: ignore[import-not-found]
    QuestLogToAguiMapper,
    iter_jsonl_events,
)


class SchemaValidationError(AssertionError):
    pass


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _load_agui_schema() -> dict[str, Any]:
    path = _repo_root() / "structure" / "schemas" / "ag_ui_events.schema.json"
    return json.loads(path.read_text(encoding="utf-8"))


def _resolve_schema(schema: dict[str, Any], root: dict[str, Any]) -> dict[str, Any]:
    ref = schema.get("$ref")
    if not isinstance(ref, str):
        return schema
    prefix = "#/$defs/"
    if not ref.startswith(prefix):
        raise SchemaValidationError(f"unsupported ref {ref}")
    return root["$defs"][ref[len(prefix) :]]


def _type_matches(expected: str | list[str], value: Any) -> bool:
    expected_types = expected if isinstance(expected, list) else [expected]
    for expected_type in expected_types:
        if expected_type == "object" and isinstance(value, dict):
            return True
        if expected_type == "array" and isinstance(value, list):
            return True
        if expected_type == "string" and isinstance(value, str):
            return True
        if expected_type == "number" and isinstance(value, int | float) and not isinstance(value, bool):
            return True
        if expected_type == "integer" and isinstance(value, int) and not isinstance(value, bool):
            return True
        if expected_type == "boolean" and isinstance(value, bool):
            return True
        if expected_type == "null" and value is None:
            return True
    return False


def _validate_against_schema(value: Any, schema: dict[str, Any], root: dict[str, Any]) -> None:
    schema = _resolve_schema(schema, root)
    if not schema:
        return

    if "const" in schema and value != schema["const"]:
        raise SchemaValidationError(f"expected const {schema['const']!r}, got {value!r}")
    if "enum" in schema and value not in schema["enum"]:
        raise SchemaValidationError(f"expected one of {schema['enum']!r}, got {value!r}")
    if "type" in schema and not _type_matches(schema["type"], value):
        raise SchemaValidationError(f"expected type {schema['type']!r}, got {type(value).__name__}")

    if "anyOf" in schema:
        errors: list[Exception] = []
        for option in schema["anyOf"]:
            try:
                _validate_against_schema(value, option, root)
                return
            except SchemaValidationError as exc:
                errors.append(exc)
        raise SchemaValidationError(f"value did not match anyOf: {errors}")

    if "oneOf" in schema:
        matches = 0
        last_error: Exception | None = None
        for option in schema["oneOf"]:
            try:
                _validate_against_schema(value, option, root)
                matches += 1
            except SchemaValidationError as exc:
                last_error = exc
        if matches != 1:
            raise SchemaValidationError(f"value matched {matches} oneOf branches; last error: {last_error}")

    if isinstance(value, dict):
        for required_key in schema.get("required", []):
            if required_key not in value:
                raise SchemaValidationError(f"missing required key {required_key!r}")
        properties = schema.get("properties", {})
        for key, property_schema in properties.items():
            if key in value:
                _validate_against_schema(value[key], property_schema, root)

    if isinstance(value, list) and "items" in schema:
        for item in value:
            _validate_against_schema(item, schema["items"], root)


def _branch_by_type(schema: dict[str, Any]) -> dict[str, dict[str, Any]]:
    branches: dict[str, dict[str, Any]] = {}
    for branch_ref in schema["oneOf"]:
        branch = _resolve_schema(branch_ref, schema)
        event_type = branch["properties"]["type"]["const"]
        branches[event_type] = branch
    return branches


class AguiStreamVerifier:
    def __init__(self) -> None:
        self.open_text: set[str] = set()
        self.open_reasoning_messages: set[str] = set()
        self.open_reasoning_phases: set[str] = set()
        self.open_tools: set[str] = set()
        self.seen_tools: set[str] = set()

    def consume(self, event: dict[str, Any]) -> None:
        event_type = event["type"]
        if event_type == "TEXT_MESSAGE_START":
            message_id = event["messageId"]
            if message_id in self.open_text:
                raise AssertionError(f"text message already open: {message_id}")
            self.open_text.add(message_id)
        elif event_type == "TEXT_MESSAGE_CONTENT":
            if event["messageId"] not in self.open_text:
                raise AssertionError(f"text content without start: {event['messageId']}")
        elif event_type == "TEXT_MESSAGE_END":
            message_id = event["messageId"]
            if message_id not in self.open_text:
                raise AssertionError(f"text end without start: {message_id}")
            self.open_text.remove(message_id)
        elif event_type == "REASONING_START":
            self.open_reasoning_phases.add(event["messageId"])
        elif event_type == "REASONING_MESSAGE_START":
            message_id = event["messageId"]
            if message_id in self.open_reasoning_messages:
                raise AssertionError(f"reasoning message already open: {message_id}")
            self.open_reasoning_messages.add(message_id)
        elif event_type == "REASONING_MESSAGE_CONTENT":
            if event["messageId"] not in self.open_reasoning_messages:
                raise AssertionError(f"reasoning content without start: {event['messageId']}")
        elif event_type == "REASONING_MESSAGE_END":
            message_id = event["messageId"]
            if message_id not in self.open_reasoning_messages:
                raise AssertionError(f"reasoning end without start: {message_id}")
            self.open_reasoning_messages.remove(message_id)
        elif event_type == "REASONING_END":
            message_id = event["messageId"]
            if message_id not in self.open_reasoning_phases:
                raise AssertionError(f"reasoning phase end without start: {message_id}")
            self.open_reasoning_phases.remove(message_id)
        elif event_type == "TOOL_CALL_START":
            tool_call_id = event["toolCallId"]
            if tool_call_id in self.open_tools:
                raise AssertionError(f"tool call already open: {tool_call_id}")
            self.open_tools.add(tool_call_id)
            self.seen_tools.add(tool_call_id)
        elif event_type == "TOOL_CALL_ARGS":
            if event["toolCallId"] not in self.open_tools:
                raise AssertionError(f"tool args without start: {event['toolCallId']}")
        elif event_type == "TOOL_CALL_RESULT":
            if event["toolCallId"] not in self.seen_tools:
                raise AssertionError(f"tool result without known tool: {event['toolCallId']}")
        elif event_type == "TOOL_CALL_END":
            tool_call_id = event["toolCallId"]
            if tool_call_id not in self.open_tools:
                raise AssertionError(f"tool end without start: {tool_call_id}")
            self.open_tools.remove(tool_call_id)

    def assert_balanced(self) -> None:
        if self.open_text:
            raise AssertionError(f"open text messages remain: {sorted(self.open_text)[:5]}")
        if self.open_reasoning_messages:
            raise AssertionError(f"open reasoning messages remain: {sorted(self.open_reasoning_messages)[:5]}")
        if self.open_reasoning_phases:
            raise AssertionError(f"open reasoning phases remain: {sorted(self.open_reasoning_phases)[:5]}")
        if self.open_tools:
            raise AssertionError(f"open tool calls remain: {sorted(self.open_tools)[:5]}")


class QuestLogToAguiMapperTests(unittest.TestCase):
    def test_maps_pi_stream_lifecycle_to_stable_agui_events(self) -> None:
        mapper = QuestLogToAguiMapper()

        def pi_event(sequence: int, payload: dict[str, Any]) -> dict[str, Any]:
            return {
                "event_kind": "provider.json",
                "harness": "pi",
                "payload": payload,
                "provider_thread_id": "pi-provider-thread",
                "role": "implementer",
                "schema_version": 1,
                "sequence": sequence,
                "step": 7,
                "thread": "pi-thread",
                "timestamp": "2026-06-08T00:00:00Z",
            }

        message = {
            "role": "assistant",
            "content": [],
            "api": "responses",
            "provider": "openai",
            "model": "gpt-test",
            "usage": {
                "input": 1,
                "output": 1,
                "cacheRead": 0,
                "cacheWrite": 0,
                "totalTokens": 2,
                "cost": {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0, "total": 0},
            },
            "stopReason": "toolUse",
            "timestamp": 123456,
        }
        tool_message = {
            **message,
            "content": [
                {
                    "type": "toolCall",
                    "id": "pi-tool-1",
                    "name": "search",
                    "arguments": {"query": "hello"},
                }
            ],
        }

        events: list[dict[str, Any]] = []
        for source in [
            pi_event(1, {"type": "message_start", "message": message}),
            pi_event(2, {"type": "message_update", "message": message, "assistantMessageEvent": {"type": "text_delta", "delta": "Hello"}}),
            pi_event(3, {"type": "message_update", "message": message, "assistantMessageEvent": {"type": "thinking_delta", "delta": "Plan"}}),
            pi_event(4, {"type": "message_update", "message": tool_message, "assistantMessageEvent": {"type": "tool_call_delta", "name": "search", "delta": '{"query":'}}),
            pi_event(5, {"type": "tool_execution_start", "toolCallId": "pi-tool-1", "toolName": "search", "args": {"query": "hello"}}),
            pi_event(6, {"type": "tool_execution_update", "toolCallId": "pi-tool-1", "toolName": "search", "args": {"query": "hello"}, "partialResult": "partial"}),
            pi_event(7, {"type": "tool_execution_end", "toolCallId": "pi-tool-1", "toolName": "search", "result": "done", "isError": False}),
            pi_event(8, {"type": "message_end", "message": tool_message}),
        ]:
            events.extend(mapper.consume(source))

        message_id = "pi-thread:step:7:pi-message:assistant:123456"
        self.assertEqual(
            [event["type"] for event in events],
            [
                "TEXT_MESSAGE_START",
                "TEXT_MESSAGE_CONTENT",
                "REASONING_START",
                "REASONING_MESSAGE_START",
                "REASONING_MESSAGE_CONTENT",
                "TOOL_CALL_START",
                "TOOL_CALL_ARGS",
                "TOOL_CALL_RESULT",
                "TOOL_CALL_RESULT",
                "TOOL_CALL_END",
                "REASONING_MESSAGE_END",
                "REASONING_END",
                "TEXT_MESSAGE_END",
            ],
        )
        self.assertEqual(events[0]["messageId"], message_id)
        self.assertEqual(events[1]["messageId"], message_id)
        self.assertEqual(events[3]["messageId"], f"{message_id}:thinking")
        self.assertEqual(events[5]["toolCallId"], "pi-tool-1")
        self.assertEqual(events[5]["parentMessageId"], message_id)
        self.assertEqual(events[6]["delta"], '{"query":')
        self.assertEqual(events[7]["content"], "partial")
        self.assertEqual(json.loads(events[8]["content"]), {"result": "done", "isError": False})
        self.assertEqual(mapper.errors(), [])

    def test_replays_fixture_logs_as_valid_agui_events(self) -> None:
        fixture_dir = Path(__file__).parent / "fixtures" / "quest_logs"
        log_paths = sorted(fixture_dir.glob("*.jsonl"))
        self.assertEqual(
            {path.stem for path in log_paths}, {"claude_code", "codex", "cursor", "pi"}
        )

        schema = _load_agui_schema()
        branches = _branch_by_type(schema)
        source_count = 0
        agui_count = 0
        fallback_events: list[dict[str, Any]] = []

        for path in log_paths:
            event_types: set[str] = set()
            mapper = QuestLogToAguiMapper()
            verifier = AguiStreamVerifier()
            for source_event in iter_jsonl_events(path):
                source_count += 1
                for agui_event in mapper.consume(source_event):
                    event_types.add(agui_event["type"])
                    branch = branches.get(agui_event.get("type"))
                    self.assertIsNotNone(branch, agui_event)
                    _validate_against_schema(agui_event, branch or {}, schema)
                    verifier.consume(agui_event)
                    agui_count += 1

            for agui_event in mapper.flush():
                branch = branches.get(agui_event.get("type"))
                self.assertIsNotNone(branch, agui_event)
                _validate_against_schema(agui_event, branch or {}, schema)
                verifier.consume(agui_event)
                agui_count += 1

            verifier.assert_balanced()
            self.assertTrue(
                {"RUN_STARTED", "RUN_FINISHED", "TEXT_MESSAGE_CONTENT", "TOOL_CALL_START", "TOOL_CALL_END"}
                <= event_types,
                (path.name, event_types),
            )
            fallback_events.extend(mapper.errors())

        self.assertGreater(source_count, 0)
        self.assertGreater(agui_count, source_count)
        fallback_counts = Counter(
            (
                event.get("harness"),
                event.get("event_kind"),
                event.get("payload", {}).get("type") if isinstance(event.get("payload"), dict) else None,
                event.get("payload", {}).get("subtype") if isinstance(event.get("payload"), dict) else None,
            )
            for event in fallback_events
        )
        self.assertEqual(
            fallback_events,
            [],
            f"Unhandled quest log event variants reached AGUI RAW fallback: {fallback_counts.most_common(10)}",
        )


if __name__ == "__main__":
    unittest.main()
