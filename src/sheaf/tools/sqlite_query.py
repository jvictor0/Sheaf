"""Tool for executing SQL against a Sheaf-managed SQLite database."""

from __future__ import annotations

import json
import re
import sqlite3
from pathlib import Path

from sheaf.config.settings import DATA_DIR, USER_DBS_DIR
from sheaf.tools.simple_tool import tool


_VALID_DB_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")


def _legacy_database_path() -> Path:
    return DATA_DIR / "sheaf.sqlite3"


def _database_root() -> Path:
    USER_DBS_DIR.mkdir(parents=True, exist_ok=True)
    return USER_DBS_DIR


def _remove_legacy_database_if_present() -> None:
    legacy = _legacy_database_path()
    if legacy.exists() and legacy.is_file():
        legacy.unlink()


def _normalize_database_name(database_name: str) -> str:
    name = database_name.strip()
    if not name:
        raise ValueError("SQL error: database_name must not be empty")
    if not _VALID_DB_NAME.match(name):
        raise ValueError(
            "SQL error: invalid database_name. Use letters, numbers, dot, underscore, or hyphen."
        )
    return name


def _database_path(database_name: str) -> Path:
    safe_name = _normalize_database_name(database_name)
    suffix = ".sqlite3" if "." not in safe_name else ""
    return _database_root() / f"{safe_name}{suffix}"


def _is_multi_statement_error(exc: Exception) -> bool:
    return "one statement at a time" in str(exc).lower()


@tool("list_sqlite_databases")
def list_sqlite_databases_tool() -> str:
    """List available SQLite databases under the configured data/user_dbs directory."""

    _remove_legacy_database_if_present()
    root = _database_root()
    entries = sorted(path for path in root.iterdir() if path.is_file() and path.suffix == ".sqlite3")
    if not entries:
        return "\n".join(["mode=list_databases", f"directory={root}", "count=0", "databases=[]"])

    names = [path.stem for path in entries]
    files = [path.name for path in entries]
    return "\n".join(
        [
            "mode=list_databases",
            f"directory={root}",
            f"count={len(entries)}",
            f"database_names={json.dumps(names, ensure_ascii=True)}",
            f"database_files={json.dumps(files, ensure_ascii=True)}",
        ]
    )


@tool("create_sqlite_database")
def create_sqlite_database_tool(database_name: str) -> str:
    """Create a named SQLite database file under the configured data/user_dbs directory."""

    _remove_legacy_database_if_present()
    db_path = _database_path(database_name)
    existed = db_path.exists()
    conn = sqlite3.connect(db_path)
    try:
        # Touches file and validates it can be opened as SQLite.
        conn.execute("PRAGMA user_version")
        conn.commit()
    except sqlite3.Error as exc:
        conn.rollback()
        raise ValueError(f"SQL error: {exc}") from exc
    finally:
        conn.close()

    return "\n".join(
        [
            "mode=create_database",
            f"database_name={db_path.stem}",
            f"database={db_path}",
            f"created={str(not existed).lower()}",
        ]
    )


@tool("run_sql")
def run_sql_tool(database_name: str, sql: str) -> str:
    """Execute SQL against a named Sheaf SQLite database."""

    _remove_legacy_database_if_present()
    sql_text = sql.strip()
    if not sql_text:
        raise ValueError("SQL error: sql must not be empty")

    db_path = _database_path(database_name)
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row

    try:
        cursor = conn.cursor()
        try:
            cursor.execute(sql_text)
        except (sqlite3.Error, sqlite3.Warning) as exc:
            if _is_multi_statement_error(exc):
                conn.executescript(sql_text)
                conn.commit()
                return "\n".join(
                    [
                        "mode=script",
                        "script_executed=true",
                        f"database={db_path}",
                    ]
                )
            conn.rollback()
            raise ValueError(f"SQL error: {exc}") from exc

        if cursor.description:
            columns = [str(item[0]) for item in cursor.description]
            rows = [dict(row) for row in cursor.fetchall()]
            return "\n".join(
                [
                    "mode=query",
                    f"database={db_path}",
                    f"columns={json.dumps(columns, ensure_ascii=True)}",
                    f"row_count={len(rows)}",
                    f"rows={json.dumps(rows, ensure_ascii=True)}",
                ]
            )

        conn.commit()
        return "\n".join(
            [
                "mode=statement",
                f"database={db_path}",
                f"rows_affected={cursor.rowcount}",
                f"last_row_id={cursor.lastrowid}",
            ]
        )
    except sqlite3.Error as exc:
        conn.rollback()
        raise ValueError(f"SQL error: {exc}") from exc
    finally:
        conn.close()
