from __future__ import annotations

import json
from pathlib import Path

import pytest

from sheaf.llm.dispatcher import OpenAIDispatcher
from sheaf.tools import build_agent_tools
from sheaf.tools.sqlite_query import (
    create_sqlite_database_tool,
    list_sqlite_databases_tool,
    run_sql_tool,
)
import sheaf.tools.sqlite_query as sqlite_query


def _parse_result(text: str) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        parsed[key] = value
    return parsed


@pytest.fixture
def isolated_data_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    data_dir = tmp_path / "data"
    monkeypatch.setattr(sqlite_query, "DATA_DIR", data_dir)
    monkeypatch.setattr(sqlite_query, "USER_DBS_DIR", data_dir / "user_dbs")
    return data_dir


def test_run_sql_creates_database_file(isolated_data_dir: Path) -> None:
    result = run_sql_tool.invoke(
        {"database_name": "things_db", "sql": "CREATE TABLE things (id INTEGER PRIMARY KEY, name TEXT)"}
    )
    parsed = _parse_result(result)
    assert parsed["mode"] == "statement"
    assert (isolated_data_dir / "user_dbs" / "things_db.sqlite3").exists()


def test_run_sql_select_returns_columns_and_rows(isolated_data_dir: Path) -> None:
    run_sql_tool.invoke(
        {"database_name": "users", "sql": "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"}
    )
    run_sql_tool.invoke({"database_name": "users", "sql": "INSERT INTO users (name) VALUES ('Ada')"})
    run_sql_tool.invoke({"database_name": "users", "sql": "INSERT INTO users (name) VALUES ('Lin')"})

    result = run_sql_tool.invoke({"database_name": "users", "sql": "SELECT id, name FROM users ORDER BY id"})
    parsed = _parse_result(result)

    assert parsed["mode"] == "query"
    assert json.loads(parsed["columns"]) == ["id", "name"]
    assert parsed["row_count"] == "2"
    assert json.loads(parsed["rows"]) == [{"id": 1, "name": "Ada"}, {"id": 2, "name": "Lin"}]


def test_run_sql_write_returns_affected_rows_metadata(isolated_data_dir: Path) -> None:
    run_sql_tool.invoke(
        {"database_name": "tasks", "sql": "CREATE TABLE tasks (id INTEGER PRIMARY KEY, title TEXT)"}
    )
    insert_result = _parse_result(
        run_sql_tool.invoke({"database_name": "tasks", "sql": "INSERT INTO tasks (title) VALUES ('Ship feature')"})
    )
    assert insert_result["mode"] == "statement"
    assert insert_result["rows_affected"] == "1"
    assert insert_result["last_row_id"] == "1"

    update_result = _parse_result(
        run_sql_tool.invoke({"database_name": "tasks", "sql": "UPDATE tasks SET title='Done' WHERE id=1"})
    )
    assert update_result["rows_affected"] == "1"

    delete_result = _parse_result(
        run_sql_tool.invoke({"database_name": "tasks", "sql": "DELETE FROM tasks WHERE id=1"})
    )
    assert delete_result["rows_affected"] == "1"


def test_run_sql_executes_multi_statement_script(isolated_data_dir: Path) -> None:
    result = run_sql_tool.invoke(
        {
            "database_name": "projects",
            "sql": (
                "CREATE TABLE projects (id INTEGER PRIMARY KEY, name TEXT);"
                "INSERT INTO projects (name) VALUES ('Sheaf');"
                "INSERT INTO projects (name) VALUES ('Atlas');"
            )
        }
    )
    parsed = _parse_result(result)
    assert parsed["mode"] == "script"
    assert parsed["script_executed"] == "true"

    query_result = _parse_result(
        run_sql_tool.invoke({"database_name": "projects", "sql": "SELECT name FROM projects ORDER BY id"})
    )
    assert json.loads(query_result["rows"]) == [{"name": "Sheaf"}, {"name": "Atlas"}]


def test_run_sql_reports_error_and_rolls_back_when_script_fails(isolated_data_dir: Path) -> None:
    run_sql_tool.invoke(
        {"database_name": "items", "sql": "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"}
    )

    with pytest.raises(ValueError, match="SQL error:"):
        run_sql_tool.invoke(
            {
                "database_name": "items",
                "sql": (
                    "BEGIN;"
                    "INSERT INTO items (name) VALUES ('ok');"
                    "INSERT INTO does_not_exist (name) VALUES ('bad');"
                    "COMMIT;"
                )
            }
        )

    result = _parse_result(
        run_sql_tool.invoke({"database_name": "items", "sql": "SELECT COUNT(*) AS c FROM items"})
    )
    rows = json.loads(result["rows"])
    assert rows == [{"c": 0}]


def test_create_sqlite_database_tool_creates_named_db_under_sqlite_dir(isolated_data_dir: Path) -> None:
    result = _parse_result(create_sqlite_database_tool.invoke({"database_name": "named_db"}))
    assert result["mode"] == "create_database"
    assert result["database_name"] == "named_db"
    assert result["created"] == "true"
    assert (isolated_data_dir / "user_dbs" / "named_db.sqlite3").exists()


def test_create_sqlite_database_removes_legacy_generic_db(isolated_data_dir: Path) -> None:
    legacy_db = isolated_data_dir / "sheaf.sqlite3"
    isolated_data_dir.mkdir(parents=True, exist_ok=True)
    legacy_db.write_bytes(b"legacy")
    assert legacy_db.exists()

    create_sqlite_database_tool.invoke({"database_name": "fresh"})
    assert not legacy_db.exists()


def test_list_sqlite_databases_returns_known_names(isolated_data_dir: Path) -> None:
    create_sqlite_database_tool.invoke({"database_name": "alpha"})
    create_sqlite_database_tool.invoke({"database_name": "beta"})

    result = _parse_result(list_sqlite_databases_tool.invoke({}))
    assert result["mode"] == "list_databases"
    assert result["count"] == "2"
    assert json.loads(result["database_names"]) == ["alpha", "beta"]
    assert json.loads(result["database_files"]) == ["alpha.sqlite3", "beta.sqlite3"]


def test_run_sql_is_registered_in_tool_registry() -> None:
    names = {tool.name for tool in build_agent_tools()}
    assert "run_sql" in names
    assert "create_sqlite_database" in names
    assert "list_sqlite_databases" in names


def test_registered_tools_expose_description_and_parameters_schema() -> None:
    tools = build_agent_tools()
    assert tools
    for item in tools:
        assert isinstance(item.description, str)
        assert item.description.strip()
        assert isinstance(item.parameters_schema, dict)
        assert item.parameters_schema.get("type") == "object"
        assert isinstance(item.parameters_schema.get("properties"), dict)


def test_openai_tool_definitions_are_derived_from_tool_metadata() -> None:
    dispatcher = OpenAIDispatcher(api_key="test-key", model="gpt-5-mini")
    definitions = dispatcher._openai_tool_definitions()
    by_name = {item["function"]["name"]: item for item in definitions}

    assert "write_note" in by_name
    write_tool = by_name["write_note"]["function"]
    assert "Write UTF-8 text to a path allowed by visible_directories policy." in write_tool["description"]
    assert write_tool["parameters"]["properties"]["relative_path"]["type"] == "string"
    assert write_tool["parameters"]["properties"]["content"]["type"] == "string"
    assert write_tool["parameters"]["properties"]["overwrite"]["type"] == "boolean"
    assert write_tool["parameters"]["required"] == ["relative_path", "content"]
