from __future__ import annotations

import asyncio
import sqlite3
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace

import sheaf.server.runtime as rr
from sheaf.llm.dispatcher import GenerationResult
from sheaf.llm.model_properties import ModelLimits, ModelProperties


class _FlakyDispatcher:
    def __init__(self) -> None:
        self.calls = 0

    def stream_generate_with_details(self, messages, *, on_token, on_thinking=None, enable_tools=True):
        self.calls += 1
        if self.calls == 1:
            raise RuntimeError("temporary network issue")
        if on_thinking is not None:
            on_thinking("flaky_recovered")
        on_token("hello")
        return GenerationResult(response="hello", tool_calls=[])


class _FatalDispatcher:
    def stream_generate_with_details(self, messages, *, on_token, on_thinking=None, enable_tools=True):
        raise rr.FatalExecutionError("Unsupported model 'bad-model'")


class _ThinkingDispatcher:
    def stream_generate_with_details(self, messages, *, on_token, on_thinking=None, enable_tools=True):
        if on_thinking is not None:
            on_thinking("model_request_started")
            on_thinking("planning_next_step")
        on_token("hi")
        on_token(" there")
        return GenerationResult(response="hi there", tool_calls=[])


class _CompactionDispatcher:
    def stream_generate_with_details(self, messages, *, on_token, on_thinking=None, enable_tools=True):
        on_token("ok")
        return GenerationResult(response="ok", tool_calls=[])


class _ConflictDispatcher:
    def stream_generate_with_details(self, messages, *, on_token, on_thinking=None, enable_tools=True):
        with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
            conn.execute("UPDATE threads SET tail_turn_id = 'external-tail-update'")
            conn.commit()
        on_token("ok")
        return GenerationResult(response="ok", tool_calls=[])


class _LLMSummaryDispatcher:
    def __init__(self) -> None:
        self.summary_calls = 0

    def generate(self, messages, *, enable_tools=True):
        self.summary_calls += 1
        return "short summary"

    def stream_generate_with_details(self, messages, *, on_token, on_thinking=None, enable_tools=True):
        on_token("ok")
        return GenerationResult(response="ok", tool_calls=[])


class _CaptureMessagesDispatcher:
    def __init__(self) -> None:
        self.last_messages = None

    def stream_generate_with_details(self, messages, *, on_token, on_thinking=None, enable_tools=True):
        self.last_messages = list(messages)
        on_token("ok")
        return GenerationResult(response="ok", tool_calls=[])


class _MultiCaptureDispatcher:
    def __init__(self) -> None:
        self.calls: list[list[rr.Message]] = []

    def stream_generate_with_details(self, messages, *, on_token, on_thinking=None, enable_tools=True):
        self.calls.append(list(messages))
        on_token("ok")
        return GenerationResult(response="ok", tool_calls=[])


class _CollectingWebSocket:
    def __init__(self) -> None:
        self.frames: list[dict[str, object]] = []

    async def send_json(self, payload: dict[str, object]) -> None:
        self.frames.append(payload)


def _configure_paths(tmp_path: Path) -> None:
    rr.DATA_DIR = tmp_path / "data"
    rr.DATA_ARCHIVE_DIR = tmp_path / "data_archive"
    rr.SERVER_DB_PATH = rr.DATA_DIR / "server.sqlite3"
    rr.USER_DBS_DIR = rr.DATA_DIR / "user_dbs"
    rr.SYSTEM_PROMPTS_DIR = rr.DATA_DIR / "system_prompts"


def _new_runtime(tmp_path: Path) -> rr.RewriteRuntime:
    _configure_paths(tmp_path)
    runtime = rr.RewriteRuntime()
    runtime.initialize()
    return runtime


def test_nonfatal_retry_and_backoff(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    flaky = _FlakyDispatcher()
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: flaky)

    thread_id = runtime.create_thread()
    queue_id = runtime.enqueue_message(
        thread_id=thread_id,
        text="hello",
        model_name="gpt-5-mini",
        in_response_to_turn_id=None,
        client_message_id="m1",
        session_id=None,
    )

    asyncio.run(runtime.process_next_runnable())

    conn = sqlite3.connect(rr.SERVER_DB_PATH)
    conn.row_factory = sqlite3.Row
    row = conn.execute("SELECT * FROM message_queue WHERE id = ?", (queue_id,)).fetchone()
    assert row is not None
    assert int(row["attempts"]) == 1
    assert row["locked_by"] is None

    conn.execute("UPDATE message_queue SET available_at = ? WHERE id = ?", (rr.utc_now(), queue_id))
    conn.commit()
    conn.close()

    asyncio.run(runtime.process_next_runnable())

    conn = sqlite3.connect(rr.SERVER_DB_PATH)
    queue_row = conn.execute("SELECT * FROM message_queue WHERE id = ?", (queue_id,)).fetchone()
    assert queue_row is None
    turn_count = conn.execute("SELECT COUNT(*) FROM turns WHERE thread_id = ?", (thread_id,)).fetchone()[0]
    assert turn_count == 2
    conn.close()


def test_fatal_errors_move_to_error_table(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: _FatalDispatcher())

    thread_id = runtime.create_thread()
    queue_id = runtime.enqueue_message(
        thread_id=thread_id,
        text="hello",
        model_name="bad-model",
        in_response_to_turn_id=None,
        client_message_id="m2",
        session_id=None,
    )

    asyncio.run(runtime.process_next_runnable())

    conn = sqlite3.connect(rr.SERVER_DB_PATH)
    conn.row_factory = sqlite3.Row
    queue_row = conn.execute("SELECT * FROM message_queue WHERE id = ?", (queue_id,)).fetchone()
    assert queue_row is None
    error_row = conn.execute("SELECT * FROM queue_errors WHERE queue_id = ?", (queue_id,)).fetchone()
    assert error_row is not None
    assert "Unsupported model" in str(error_row["error_text"])
    request_row = conn.execute("SELECT * FROM requests ORDER BY created_at DESC LIMIT 1").fetchone()
    assert request_row is not None
    assert request_row["turn_id"] is None
    assert "fatal_error[" in str(request_row["error_text"])
    assert request_row["completed_at"] is not None
    conn.close()


def test_streaming_persists_thinking_traces(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: _ThinkingDispatcher())

    thread_id = runtime.create_thread()
    runtime.enqueue_message(
        thread_id=thread_id,
        text="hello",
        model_name="gpt-5-mini",
        in_response_to_turn_id=None,
        client_message_id="m3",
        session_id=None,
    )

    asyncio.run(runtime.process_next_runnable())

    conn = sqlite3.connect(rr.SERVER_DB_PATH)
    conn.row_factory = sqlite3.Row
    assistant = conn.execute(
        "SELECT id FROM turns WHERE thread_id = ? AND speaker = 'assistant' ORDER BY created_at DESC LIMIT 1",
        (thread_id,),
    ).fetchone()
    assert assistant is not None

    traces = conn.execute(
        "SELECT trace_text FROM thinking_traces WHERE turn_id = ? ORDER BY sequence_no",
        (assistant["id"],),
    ).fetchall()
    assert len(traces) >= 2

    stream_event = conn.execute(
        "SELECT payload_json FROM turn_events WHERE turn_id = ? AND event_type = 'assistant_stream_complete'",
        (assistant["id"],),
    ).fetchone()
    assert stream_event is not None
    conn.close()


def test_context_compaction_event_persisted(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: _CompactionDispatcher())

    def _tiny_limits(*, provider: str, model: str):
        return ModelProperties(
            provider=provider,
            model=model,
            limits=ModelLimits(
                context_window_tokens=100,
                max_output_tokens=32,
                compaction_trigger_ratio=0.5,
                compaction_target_ratio=0.35,
                recent_messages_to_keep=2,
            ),
        )

    monkeypatch.setattr(rr, "resolve_model_properties", _tiny_limits)

    thread_id = runtime.create_thread()
    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        now = rr.utc_now()
        prev = None
        for i in range(6):
            turn_id = f"u-{i}"
            conn.execute(
                """
                INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
                VALUES (?, ?, ?, 'user', ?, NULL, NULL, NULL, ?)
                """,
                (turn_id, thread_id, prev, "x" * 120, now),
            )
            prev = turn_id
        conn.execute("UPDATE threads SET tail_turn_id = ?, updated_at = ? WHERE id = ?", (prev, now, thread_id))
        conn.commit()

    runtime.enqueue_message(
        thread_id=thread_id,
        text="new message to trigger compaction",
        model_name="gpt-5-mini",
        in_response_to_turn_id=prev,
        client_message_id="m4",
        session_id=None,
    )

    asyncio.run(runtime.process_next_runnable())

    conn = sqlite3.connect(rr.SERVER_DB_PATH)
    conn.row_factory = sqlite3.Row
    assistant = conn.execute(
        "SELECT id FROM turns WHERE thread_id = ? AND speaker = 'assistant' ORDER BY created_at DESC LIMIT 1",
        (thread_id,),
    ).fetchone()
    assert assistant is not None
    compaction = conn.execute(
        "SELECT payload_json FROM turn_events WHERE turn_id = ? AND event_type = 'context_compaction'",
        (assistant["id"],),
    ).fetchone()
    assert compaction is not None
    conn.close()


def test_system_prompt_injected_into_prompt_messages(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    dispatcher = _CaptureMessagesDispatcher()
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: dispatcher)

    prompt_path = rr.SYSTEM_PROMPTS_DIR / "sheaf_default.md"
    prompt_path.write_text("You are sheaf test prompt.", encoding="utf-8")

    thread_id = runtime.create_thread()
    runtime.enqueue_message(
        thread_id=thread_id,
        text="hello",
        model_name="gpt-5-mini",
        in_response_to_turn_id=None,
        client_message_id="m5",
        session_id=None,
    )

    asyncio.run(runtime.process_next_runnable())

    assert dispatcher.last_messages is not None
    assert dispatcher.last_messages[0].role == "system"
    assert "You are sheaf test prompt." in dispatcher.last_messages[0].content


def test_detach_websocket_removes_session_and_queue_mapping(tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    thread_id = runtime.create_thread()
    session = runtime.create_session(thread_id, None)
    queue_id = runtime.enqueue_message(
        thread_id=thread_id,
        text="hello",
        model_name="gpt-5-mini",
        in_response_to_turn_id=None,
        client_message_id="m6",
        session_id=session.session_id,
    )

    runtime.detach_websocket(session.session_id)

    assert session.session_id not in runtime._sessions
    assert queue_id not in runtime._queue_delivery_map


def test_handshake_context_budget_uses_token_estimate(tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    thread_id = runtime.create_thread()

    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        now = rr.utc_now()
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('u1', ?, NULL, 'user', ?, NULL, NULL, NULL, ?)
            """,
            (thread_id, "hello world", now),
        )
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('a1', ?, 'u1', 'assistant', ?, NULL, NULL, 'gpt-5-mini', ?)
            """,
            (thread_id, "response text", now),
        )
        conn.execute("UPDATE threads SET tail_turn_id = 'a1', updated_at = ? WHERE id = ?", (now, thread_id))
        conn.commit()

    session = runtime.create_session(thread_id, None)
    ws = _CollectingWebSocket()
    asyncio.run(runtime.stream_handshake(session, ws))

    context_budget = next(frame for frame in ws.frames if frame["type"] == "context_budget")
    expected = runtime._estimate_message_tokens(
        [
            rr.Message(role="user", content="hello world"),
            rr.Message(role="assistant", content="response text"),
        ]
    )
    assert context_budget["context_size"] == expected
    assert int(context_budget["max_context_size"]) > 0


def test_fetch_handshake_turns_uses_prev_turn_chain_not_created_at(tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    thread_id = runtime.create_thread()
    same_time = rr.utc_now()

    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('t1', ?, NULL, 'user', 'u1', NULL, NULL, NULL, ?)
            """,
            (thread_id, same_time),
        )
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('t2', ?, 't1', 'assistant', 'a1', NULL, NULL, 'gpt-5-mini', ?)
            """,
            (thread_id, same_time),
        )
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('t3', ?, 't2', 'user', 'u2', NULL, NULL, NULL, ?)
            """,
            (thread_id, same_time),
        )
        conn.execute("UPDATE threads SET tail_turn_id = 't3', updated_at = ? WHERE id = ?", (same_time, thread_id))
        conn.commit()

    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        turns = runtime._fetch_handshake_turns(conn, thread_id, "t1")

    assert [turn["id"] for turn in turns] == ["t2", "t3"]


def test_connect_applies_per_connection_pragmas(tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)

    conn = runtime._connect()
    try:
        sync = conn.execute("PRAGMA synchronous").fetchone()[0]
        busy = conn.execute("PRAGMA busy_timeout").fetchone()[0]
    finally:
        conn.close()

    assert int(sync) == 1  # NORMAL
    assert int(busy) == 5000


def test_enqueue_message_wakes_worker_event(tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    worker_loop = asyncio.get_event_loop_policy().new_event_loop()
    asyncio.get_event_loop_policy().set_event_loop(worker_loop)
    runtime._worker_loop_event_loop = worker_loop
    runtime._worker_wake = asyncio.Event()
    thread_id = runtime.create_thread()
    try:
        assert runtime._worker_wake.is_set() is False
        runtime.enqueue_message(
            thread_id=thread_id,
            text="wake me",
            model_name="gpt-5-mini",
            in_response_to_turn_id=None,
            client_message_id="wake-1",
            session_id=None,
        )
        # call_soon_threadsafe schedules onto the worker loop; process pending callbacks.
        runtime._worker_loop_event_loop.call_soon(runtime._worker_loop_event_loop.stop)
        runtime._worker_loop_event_loop.run_forever()
        assert runtime._worker_wake.is_set() is True
    finally:
        runtime._worker_loop_event_loop.close()
        asyncio.get_event_loop_policy().set_event_loop(None)
        runtime._worker_loop_event_loop = None
        runtime._worker_wake = None


def test_load_thread_messages_uses_chain_order(tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    thread_id = runtime.create_thread()
    same_time = rr.utc_now()

    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('c1', ?, NULL, 'user', 'first', NULL, NULL, NULL, ?)
            """,
            (thread_id, same_time),
        )
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('c2', ?, 'c1', 'assistant', 'second', NULL, NULL, 'gpt-5-mini', ?)
            """,
            (thread_id, same_time),
        )
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('c3', ?, 'c2', 'user', 'third', NULL, NULL, NULL, ?)
            """,
            (thread_id, same_time),
        )
        conn.execute("UPDATE threads SET tail_turn_id = 'c3', updated_at = ? WHERE id = ?", (same_time, thread_id))
        conn.commit()

    with runtime._db() as conn:
        messages = runtime._load_thread_messages(conn, thread_id)

    non_system = [m.content for m in messages if m.role != "system"]
    assert non_system == ["first", "second", "third"]


def test_load_thread_messages_anchors_on_nearest_turn_context(tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    thread_id = runtime.create_thread()
    now = rr.utc_now()

    anchor_context = rr.json.dumps(
        {
            "messages": [
                {"role": "system", "content": "compacted summary"},
                {"role": "user", "content": "u1"},
            ]
        }
    )

    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('u0', ?, NULL, 'user', 'u0', NULL, NULL, NULL, ?)
            """,
            (thread_id, now),
        )
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('a0', ?, 'u0', 'assistant', 'a0', NULL, NULL, 'gpt-5-mini', ?)
            """,
            (thread_id, now),
        )
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('u1', ?, 'a0', 'user', 'u1', NULL, NULL, NULL, ?)
            """,
            (thread_id, now),
        )
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('a1', ?, 'u1', 'assistant', 'a1', ?, NULL, 'gpt-5-mini', ?)
            """,
            (thread_id, anchor_context, now),
        )
        conn.execute(
            """
            INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
            VALUES ('u2', ?, 'a1', 'user', 'u2', NULL, NULL, NULL, ?)
            """,
            (thread_id, now),
        )
        conn.execute("UPDATE threads SET tail_turn_id = 'u2', updated_at = ? WHERE id = ?", (now, thread_id))
        conn.commit()

    with runtime._db() as conn:
        messages = runtime._load_thread_messages(conn, thread_id)

    roles_and_contents = [(m.role, m.content) for m in messages]
    assert roles_and_contents == [
        ("system", "compacted summary"),
        ("user", "u1"),
        ("assistant", "a1"),
        ("user", "u2"),
    ]


def test_request_token_fields_use_estimator(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: _CompactionDispatcher())

    thread_id = runtime.create_thread()
    runtime.enqueue_message(
        thread_id=thread_id,
        text="hello world",
        model_name="gpt-5-mini",
        in_response_to_turn_id=None,
        client_message_id="m7",
        session_id=None,
    )

    asyncio.run(runtime.process_next_runnable())

    conn = sqlite3.connect(rr.SERVER_DB_PATH)
    conn.row_factory = sqlite3.Row
    request_row = conn.execute("SELECT input_tokens, output_tokens, request_json FROM requests ORDER BY created_at DESC LIMIT 1").fetchone()
    conn.close()
    assert request_row is not None

    payload = rr.json.loads(str(request_row["request_json"]))
    prompt_messages = [rr.Message(role=item["role"], content=item["content"]) for item in payload["messages"]]
    assert int(request_row["input_tokens"]) == runtime._estimate_message_tokens(prompt_messages)
    assert int(request_row["output_tokens"]) == runtime._estimate_tokens("ok")


def test_request_marked_failed_on_commit_conflict(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: _ConflictDispatcher())

    thread_id = runtime.create_thread()
    runtime.enqueue_message(
        thread_id=thread_id,
        text="trigger commit conflict",
        model_name="gpt-5-mini",
        in_response_to_turn_id=None,
        client_message_id="m8",
        session_id=None,
    )
    asyncio.run(runtime.process_next_runnable())

    conn = sqlite3.connect(rr.SERVER_DB_PATH)
    conn.row_factory = sqlite3.Row
    request_row = conn.execute("SELECT * FROM requests ORDER BY created_at DESC LIMIT 1").fetchone()
    queue_rows = conn.execute("SELECT COUNT(*) AS c FROM message_queue").fetchone()
    conn.close()
    assert request_row is not None
    assert request_row["turn_id"] is None
    assert str(request_row["error_text"]).startswith("execution_conflict:")
    assert request_row["completed_at"] is not None
    assert int(queue_rows["c"]) == 0


def test_visible_directories_defaults_read_only(tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute("SELECT path, access_mode FROM visible_directories").fetchall()
    by_path = {str(row["path"]): str(row["access_mode"]) for row in rows}
    assert str(rr.REPO_ROOT.resolve()) in by_path
    assert by_path[str(rr.REPO_ROOT.resolve())] == "read_only"
    assert str((rr.DATA_DIR / "user_dbs").resolve()) not in by_path


def test_turn_context_persists_model_prompt_for_assistant(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: _CompactionDispatcher())
    thread_id = runtime.create_thread()
    runtime.enqueue_message(
        thread_id=thread_id,
        text="context test",
        model_name="gpt-5-mini",
        in_response_to_turn_id=None,
        client_message_id="m9",
        session_id=None,
    )
    asyncio.run(runtime.process_next_runnable())

    conn = sqlite3.connect(rr.SERVER_DB_PATH)
    conn.row_factory = sqlite3.Row
    assistant = conn.execute(
        "SELECT turn_context, message_text FROM turns WHERE thread_id = ? AND speaker = 'assistant' ORDER BY created_at DESC LIMIT 1",
        (thread_id,),
    ).fetchone()
    user = conn.execute(
        "SELECT turn_context FROM turns WHERE thread_id = ? AND speaker = 'user' ORDER BY created_at DESC LIMIT 1",
        (thread_id,),
    ).fetchone()
    conn.close()

    assert assistant is not None
    assert assistant["turn_context"] is not None
    payload = rr.json.loads(str(assistant["turn_context"]))
    assert isinstance(payload, dict)
    messages = payload.get("messages")
    assert isinstance(messages, list)
    assert messages[-1]["role"] == "user"
    assert messages[-1]["content"] == "context test"
    assert user is not None
    assert user["turn_context"] is None


def test_context_compaction_uses_llm_summary_when_available(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    dispatcher = _LLMSummaryDispatcher()
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: dispatcher)

    def _tiny_limits(*, provider: str, model: str):
        return ModelProperties(
            provider=provider,
            model=model,
            limits=ModelLimits(
                context_window_tokens=100,
                max_output_tokens=32,
                compaction_trigger_ratio=0.5,
                compaction_target_ratio=0.35,
                recent_messages_to_keep=2,
            ),
        )

    monkeypatch.setattr(rr, "resolve_model_properties", _tiny_limits)

    thread_id = runtime.create_thread()
    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        now = rr.utc_now()
        prev = None
        for i in range(6):
            turn_id = f"s-{i}"
            conn.execute(
                """
                INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
                VALUES (?, ?, ?, 'user', ?, NULL, NULL, NULL, ?)
                """,
                (turn_id, thread_id, prev, "x" * 120, now),
            )
            prev = turn_id
        conn.execute("UPDATE threads SET tail_turn_id = ?, updated_at = ? WHERE id = ?", (prev, now, thread_id))
        conn.commit()

    runtime.enqueue_message(
        thread_id=thread_id,
        text="trigger",
        model_name="gpt-5-mini",
        in_response_to_turn_id=prev,
        client_message_id="m10",
        session_id=None,
    )
    asyncio.run(runtime.process_next_runnable())
    assert dispatcher.summary_calls >= 1


def test_incremental_prompt_uses_compacted_ancestor_context(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    dispatcher = _MultiCaptureDispatcher()
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: dispatcher)

    def _tiny_limits(*, provider: str, model: str):
        return ModelProperties(
            provider=provider,
            model=model,
            limits=ModelLimits(
                context_window_tokens=100,
                max_output_tokens=32,
                compaction_trigger_ratio=0.5,
                compaction_target_ratio=0.35,
                recent_messages_to_keep=2,
            ),
        )

    monkeypatch.setattr(rr, "resolve_model_properties", _tiny_limits)

    thread_id = runtime.create_thread()
    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        now = rr.utc_now()
        prev = None
        for i in range(6):
            turn_id = f"z-{i}"
            conn.execute(
                """
                INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
                VALUES (?, ?, ?, 'user', ?, NULL, NULL, NULL, ?)
                """,
                (turn_id, thread_id, prev, "x" * 120, now),
            )
            prev = turn_id
        conn.execute("UPDATE threads SET tail_turn_id = ?, updated_at = ? WHERE id = ?", (prev, now, thread_id))
        conn.commit()

    runtime.enqueue_message(
        thread_id=thread_id,
        text="first new",
        model_name="gpt-5-mini",
        in_response_to_turn_id=prev,
        client_message_id="m12",
        session_id=None,
    )
    asyncio.run(runtime.process_next_runnable())
    assert len(dispatcher.calls) == 1

    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        tail = conn.execute("SELECT tail_turn_id FROM threads WHERE id = ?", (thread_id,)).fetchone()
        assert tail is not None
        new_tail = str(tail["tail_turn_id"])

    runtime.enqueue_message(
        thread_id=thread_id,
        text="second new",
        model_name="gpt-5-mini",
        in_response_to_turn_id=new_tail,
        client_message_id="m13",
        session_id=None,
    )
    asyncio.run(runtime.process_next_runnable())
    assert len(dispatcher.calls) == 2

    second_prompt = dispatcher.calls[1]
    second_contents = [m.content for m in second_prompt]
    assert any(content.startswith("Context compaction summary of earlier turns:") for content in second_contents)
    assert "ok" in second_contents  # previous assistant turn appended incrementally
    assert second_contents[-1] == "second new"


def test_fatal_classification_uses_exception_types_not_message_text(tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    assert runtime._is_fatal_error(RuntimeError("authentication failed")) is False
    assert runtime._is_fatal_error(rr.FatalExecutionError("fatal")) is True
    assert runtime._is_fatal_error(rr.UnsupportedModelError("unsupported")) is True


def test_model_provider_uses_registry_without_db_roundtrip(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)

    @contextmanager
    def _db_should_not_be_called():
        raise AssertionError("_db() should not be called by _model_provider")
        yield

    monkeypatch.setattr(runtime, "_db", _db_should_not_be_called)
    monkeypatch.setattr(
        rr,
        "get_model_registry",
        lambda: SimpleNamespace(
            resolve_model=lambda _name, allow_refresh=False: SimpleNamespace(provider="ollama")
        ),
    )
    assert runtime._model_provider("llama3.2:latest") == "ollama"


def test_commit_stage_fatal_error_moves_row_to_queue_errors(monkeypatch, tmp_path: Path) -> None:
    runtime = _new_runtime(tmp_path)
    monkeypatch.setattr(rr, "build_dispatcher", lambda model_override=None: _CompactionDispatcher())
    monkeypatch.setattr(runtime, "_model_provider", lambda _model_name: "openai")

    thread_id = runtime.create_thread()
    queue_id = runtime.enqueue_message(
        thread_id=thread_id,
        text="trigger commit fatal",
        model_name="gpt-5-mini",
        in_response_to_turn_id=None,
        client_message_id="m11",
        session_id=None,
    )

    original_db = runtime._db
    calls = {"n": 0}

    @contextmanager
    def _db_with_commit_failure():
        calls["n"] += 1
        if calls["n"] == 5:
            raise rr.FatalExecutionError("forced commit failure")
        with original_db() as conn:
            yield conn

    monkeypatch.setattr(runtime, "_db", _db_with_commit_failure)
    asyncio.run(runtime.process_next_runnable())

    conn = sqlite3.connect(rr.SERVER_DB_PATH)
    conn.row_factory = sqlite3.Row
    queue_row = conn.execute("SELECT * FROM message_queue WHERE id = ?", (queue_id,)).fetchone()
    error_row = conn.execute("SELECT * FROM queue_errors WHERE queue_id = ?", (queue_id,)).fetchone()
    request_row = conn.execute("SELECT * FROM requests ORDER BY created_at DESC LIMIT 1").fetchone()
    conn.close()

    assert queue_row is None
    assert error_row is not None
    assert error_row["failure_stage"] == "commit"
    assert request_row is not None
    assert request_row["turn_id"] is None
    assert "fatal_error[commit]" in str(request_row["error_text"])
