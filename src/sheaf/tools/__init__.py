"""Agent tool registry."""

from __future__ import annotations

from sheaf.tools.file_read_list import list_notes_tool, read_note_tool
from sheaf.tools.file_write import write_note_tool
from sheaf.tools.simple_tool import SimpleTool
from sheaf.tools.sqlite_query import (
    create_sqlite_database_tool,
    list_sqlite_databases_tool,
    run_sql_tool,
)


def build_agent_tools() -> list[SimpleTool]:
    return [
        write_note_tool,
        list_notes_tool,
        read_note_tool,
        list_sqlite_databases_tool,
        create_sqlite_database_tool,
        run_sql_tool,
    ]


__all__ = [
    "build_agent_tools",
    "write_note_tool",
    "list_notes_tool",
    "read_note_tool",
    "list_sqlite_databases_tool",
    "create_sqlite_database_tool",
    "run_sql_tool",
]
