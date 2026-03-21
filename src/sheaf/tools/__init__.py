"""Agent tool registry."""

from __future__ import annotations

from sheaf.tools.filesystem import (
    apply_patch_tool,
    create_directory_tool,
    create_file_tool,
    delete_path_tool,
    list_directory_tool,
    move_path_tool,
    read_file_tool,
    repair_vault_tool,
)
from sheaf.tools.simple_tool import SimpleTool
from sheaf.tools.sqlite_query import (
    create_sqlite_database_tool,
    list_sqlite_databases_tool,
    run_sql_tool,
)


def build_agent_tools() -> list[SimpleTool]:
    return [
        list_directory_tool,
        read_file_tool,
        create_file_tool,
        create_directory_tool,
        apply_patch_tool,
        move_path_tool,
        delete_path_tool,
        repair_vault_tool,
        list_sqlite_databases_tool,
        create_sqlite_database_tool,
        run_sql_tool,
    ]


__all__ = [
    "build_agent_tools",
    "list_directory_tool",
    "read_file_tool",
    "create_file_tool",
    "create_directory_tool",
    "apply_patch_tool",
    "move_path_tool",
    "delete_path_tool",
    "repair_vault_tool",
    "list_sqlite_databases_tool",
    "create_sqlite_database_tool",
    "run_sql_tool",
]
