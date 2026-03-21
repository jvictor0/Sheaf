"""Shared filesystem write logging and repair helpers."""

from __future__ import annotations

import difflib
import os
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from sheaf.config.settings import VAULT_QUARANTINE_DIR
from sheaf.tools.patching import apply_unified_diff
from sheaf.tools.visibility import ensure_writable
from sheaf.vaults.checksums import sha256_text
from sheaf.vaults.paths import VaultRecord, relative_name, require_vault_for_path
from sheaf.vaults.runtime import db as vault_db
from sheaf.vaults.runtime import utc_now


@dataclass(frozen=True)
class WriteOperation:
    kind: str
    path: Path
    new_path: Path | None = None
    content: str | None = None
    patch: str | None = None
    overwrite: bool = False


@dataclass(frozen=True)
class WriteResult:
    message: str
    lsn: int | None = None


def _display_path(path: Path) -> str:
    resolved = path.resolve()
    home = Path.home().resolve()
    try:
        rel = resolved.relative_to(home)
    except ValueError:
        return str(resolved)
    return f"~/{rel.as_posix()}"


def _read_text_file(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError(f"Binary files are not supported: {path}") from exc


def _insert_log_row(
    conn: sqlite3.Connection,
    *,
    vault: VaultRecord,
    name: str,
    target_kind: str,
    action: str,
    data: str | None,
    new_name: str | None,
    recorded_at: str,
) -> int:
    cursor = conn.execute(
        """
        INSERT INTO log_records(vault_id, name, target_kind, action, data, new_name, recorded_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        """,
        (vault.id, name, target_kind, action, data, new_name, recorded_at),
    )
    return int(cursor.lastrowid)


def _upsert_file_state(
    conn: sqlite3.Connection,
    *,
    vault_id: int,
    name: str,
    lsn: int,
    checksum: str,
    recorded_at: str,
) -> None:
    row = conn.execute(
        "SELECT created_lsn, created_at FROM files WHERE vault_id = ? AND name = ?",
        (vault_id, name),
    ).fetchone()
    if row is None:
        conn.execute(
            """
            INSERT INTO files(vault_id, name, created_lsn, last_lsn, checksum, created_at, updated_at)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (vault_id, name, lsn, lsn, checksum, recorded_at, recorded_at),
        )
        return
    conn.execute(
        """
        UPDATE files
        SET last_lsn = ?, checksum = ?, updated_at = ?
        WHERE vault_id = ? AND name = ?
        """,
        (lsn, checksum, recorded_at, vault_id, name),
    )


def _record_create_file(conn: sqlite3.Connection, vault: VaultRecord, path: Path, content: str) -> int:
    recorded_at = utc_now()
    name = relative_name(vault.root_path, path)
    lsn = _insert_log_row(
        conn,
        vault=vault,
        name=name,
        target_kind="file",
        action="create",
        data=content,
        new_name=None,
        recorded_at=recorded_at,
    )
    _upsert_file_state(
        conn,
        vault_id=vault.id,
        name=name,
        lsn=lsn,
        checksum=sha256_text(content),
        recorded_at=recorded_at,
    )
    return lsn


def _record_delete_file(conn: sqlite3.Connection, vault: VaultRecord, path: Path, final_content: str) -> int:
    recorded_at = utc_now()
    name = relative_name(vault.root_path, path)
    lsn = _insert_log_row(
        conn,
        vault=vault,
        name=name,
        target_kind="file",
        action="delete",
        data=final_content,
        new_name=None,
        recorded_at=recorded_at,
    )
    conn.execute("DELETE FROM files WHERE vault_id = ? AND name = ?", (vault.id, name))
    return lsn


def _record_patch_file(conn: sqlite3.Connection, vault: VaultRecord, path: Path, patch: str, content: str) -> int:
    recorded_at = utc_now()
    name = relative_name(vault.root_path, path)
    lsn = _insert_log_row(
        conn,
        vault=vault,
        name=name,
        target_kind="file",
        action="patch",
        data=patch,
        new_name=None,
        recorded_at=recorded_at,
    )
    _upsert_file_state(
        conn,
        vault_id=vault.id,
        name=name,
        lsn=lsn,
        checksum=sha256_text(content),
        recorded_at=recorded_at,
    )
    return lsn


def _record_create_directory(conn: sqlite3.Connection, vault: VaultRecord, path: Path) -> int:
    return _insert_log_row(
        conn,
        vault=vault,
        name=relative_name(vault.root_path, path),
        target_kind="directory",
        action="create",
        data=None,
        new_name=None,
        recorded_at=utc_now(),
    )


def _record_delete_directory(conn: sqlite3.Connection, vault: VaultRecord, path: Path) -> int:
    return _insert_log_row(
        conn,
        vault=vault,
        name=relative_name(vault.root_path, path),
        target_kind="directory",
        action="delete",
        data=None,
        new_name=None,
        recorded_at=utc_now(),
    )


def _record_directory_move_same_vault(
    conn: sqlite3.Connection,
    *,
    vault: VaultRecord,
    source: Path,
    destination: Path,
) -> int:
    recorded_at = utc_now()
    old_name = relative_name(vault.root_path, source)
    new_name = relative_name(vault.root_path, destination)
    lsn = _insert_log_row(
        conn,
        vault=vault,
        name=old_name,
        target_kind="directory",
        action="rename",
        data=None,
        new_name=new_name,
        recorded_at=recorded_at,
    )
    rows = conn.execute(
        "SELECT name, created_lsn, checksum, created_at FROM files WHERE vault_id = ? AND name LIKE ?",
        (vault.id, f"{old_name}/%"),
    ).fetchall()
    for row in rows:
        old_child_name = str(row["name"])
        suffix = old_child_name[len(old_name) :]
        child_name = f"{new_name}{suffix}"
        conn.execute("DELETE FROM files WHERE vault_id = ? AND name = ?", (vault.id, old_child_name))
        conn.execute(
            """
            INSERT INTO files(vault_id, name, created_lsn, last_lsn, checksum, created_at, updated_at)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (
                vault.id,
                child_name,
                int(row["created_lsn"]),
                lsn,
                str(row["checksum"]),
                str(row["created_at"]),
                recorded_at,
            ),
        )
    return lsn


def record_filesystem_write(operation: WriteOperation) -> WriteResult:
    path = operation.path.resolve()
    ensure_writable(path)
    if operation.new_path is not None:
        ensure_writable(operation.new_path.resolve())

    if operation.kind == "create_file":
        if path.exists() and path.is_dir():
            raise ValueError("Target path points to a directory, expected a file")
        if path.exists() and not operation.overwrite:
            raise ValueError("Target file already exists and overwrite is false")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(operation.content or "", encoding="utf-8")
        with vault_db() as conn:
            vault = require_vault_for_path(conn, path)
            lsn = _record_create_file(conn, vault, path, operation.content or "")
            conn.commit()
        return WriteResult(message=f"Wrote {_display_path(path)}", lsn=lsn)

    if operation.kind == "create_directory":
        path.mkdir(parents=True, exist_ok=True)
        with vault_db() as conn:
            vault = require_vault_for_path(conn, path)
            lsn = _record_create_directory(conn, vault, path)
            conn.commit()
        return WriteResult(message=f"Created directory {_display_path(path)}", lsn=lsn)

    if operation.kind == "patch_file":
        if not path.exists() or not path.is_file():
            raise ValueError(f"File does not exist: {path}")
        original = _read_text_file(path)
        updated = apply_unified_diff(original, operation.patch or "")
        path.write_text(updated, encoding="utf-8")
        with vault_db() as conn:
            vault = require_vault_for_path(conn, path)
            lsn = _record_patch_file(conn, vault, path, operation.patch or "", updated)
            conn.commit()
        return WriteResult(message=f"Patched {_display_path(path)}", lsn=lsn)

    if operation.kind == "delete_path":
        if not path.exists():
            raise ValueError(f"Path does not exist: {path}")
        with vault_db() as conn:
            vault = require_vault_for_path(conn, path)
            if path.is_dir():
                if any(path.iterdir()):
                    raise ValueError("Directory is not empty")
                path.rmdir()
                lsn = _record_delete_directory(conn, vault, path)
                conn.commit()
                return WriteResult(message=f"Deleted directory {_display_path(path)}", lsn=lsn)
            final_content = _read_text_file(path)
            path.unlink()
            lsn = _record_delete_file(conn, vault, path, final_content)
            conn.commit()
        return WriteResult(message=f"Deleted {_display_path(path)}", lsn=lsn)

    if operation.kind in {"move_file", "move_directory"}:
        destination = operation.new_path
        if destination is None:
            raise ValueError("Destination path is required")
        if not path.exists():
            raise ValueError(f"Path does not exist: {path}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        source_is_dir = path.is_dir()
        if source_is_dir != (operation.kind == "move_directory"):
            raise ValueError("Move operation kind does not match source path type")
        source_content = None
        if not source_is_dir:
            source_content = _read_text_file(path)
        os.replace(path, destination)
        with vault_db() as conn:
            source_vault = require_vault_for_path(conn, path)
            destination_vault = require_vault_for_path(conn, destination)
            if source_is_dir:
                if source_vault.id == destination_vault.id:
                    lsn = _record_directory_move_same_vault(
                        conn,
                        vault=source_vault,
                        source=path,
                        destination=destination,
                    )
                else:
                    lsn = _insert_log_row(
                        conn,
                        vault=source_vault,
                        name=relative_name(source_vault.root_path, path),
                        target_kind="directory",
                        action="delete",
                        data=None,
                        new_name=None,
                        recorded_at=utc_now(),
                    )
                    _insert_log_row(
                        conn,
                        vault=destination_vault,
                        name=relative_name(destination_vault.root_path, destination),
                        target_kind="directory",
                        action="create",
                        data=None,
                        new_name=None,
                        recorded_at=utc_now(),
                    )
            else:
                create_lsn = _record_create_file(conn, destination_vault, destination, source_content or "")
                delete_lsn = _record_delete_file(conn, source_vault, path, source_content or "")
                lsn = delete_lsn or create_lsn
            conn.commit()
        return WriteResult(message=f"Moved {_display_path(path)} to {_display_path(destination)}", lsn=lsn)

    raise ValueError(f"Unsupported write operation kind: {operation.kind}")


def rebuild_files_table(conn: sqlite3.Connection, *, vault_id: int) -> None:
    conn.execute("DELETE FROM files WHERE vault_id = ?", (vault_id,))
    rows = conn.execute(
        "SELECT * FROM log_records WHERE vault_id = ? ORDER BY lsn ASC",
        (vault_id,),
    ).fetchall()
    for row in rows:
        action = str(row["action"])
        target_kind = str(row["target_kind"])
        name = str(row["name"])
        recorded_at = str(row["recorded_at"])
        lsn = int(row["lsn"])
        if target_kind == "directory":
            if action == "rename":
                old_prefix = name
                new_prefix = str(row["new_name"])
                descendants = conn.execute(
                    "SELECT name, created_lsn, checksum, created_at FROM files WHERE vault_id = ? AND name LIKE ?",
                    (vault_id, f"{old_prefix}/%"),
                ).fetchall()
                for item in descendants:
                    old_name = str(item["name"])
                    new_name = f"{new_prefix}{old_name[len(old_prefix):]}"
                    conn.execute("DELETE FROM files WHERE vault_id = ? AND name = ?", (vault_id, old_name))
                    conn.execute(
                        """
                        INSERT INTO files(vault_id, name, created_lsn, last_lsn, checksum, created_at, updated_at)
                        VALUES (?, ?, ?, ?, ?, ?, ?)
                        """,
                        (
                            vault_id,
                            new_name,
                            int(item["created_lsn"]),
                            lsn,
                            str(item["checksum"]),
                            str(item["created_at"]),
                            recorded_at,
                        ),
                    )
            continue
        if action == "create":
            _upsert_file_state(
                conn,
                vault_id=vault_id,
                name=name,
                lsn=lsn,
                checksum=sha256_text(str(row["data"] or "")),
                recorded_at=recorded_at,
            )
            continue
        if action == "patch":
            content = reconstruct_file_content(conn, vault_id=vault_id, name=name, up_to_lsn=lsn)
            _upsert_file_state(
                conn,
                vault_id=vault_id,
                name=name,
                lsn=lsn,
                checksum=sha256_text(content),
                recorded_at=recorded_at,
            )
            continue
        if action == "delete":
            conn.execute("DELETE FROM files WHERE vault_id = ? AND name = ?", (vault_id, name))
            continue
        if action == "rename":
            row_state = conn.execute(
                "SELECT created_lsn, checksum, created_at FROM files WHERE vault_id = ? AND name = ?",
                (vault_id, name),
            ).fetchone()
            if row_state is None:
                continue
            conn.execute("DELETE FROM files WHERE vault_id = ? AND name = ?", (vault_id, name))
            conn.execute(
                """
                INSERT INTO files(vault_id, name, created_lsn, last_lsn, checksum, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    vault_id,
                    str(row["new_name"]),
                    int(row_state["created_lsn"]),
                    lsn,
                    str(row_state["checksum"]),
                    str(row_state["created_at"]),
                    recorded_at,
                ),
            )


def reconstruct_file_content(
    conn: sqlite3.Connection,
    *,
    vault_id: int,
    name: str,
    up_to_lsn: int | None = None,
) -> str:
    params: list[object] = [vault_id]
    sql = "SELECT * FROM log_records WHERE vault_id = ? ORDER BY lsn ASC"
    if up_to_lsn is not None:
        sql = "SELECT * FROM log_records WHERE vault_id = ? AND lsn <= ? ORDER BY lsn ASC"
        params.append(up_to_lsn)
    content: Optional[str] = None
    current_name = name
    rows = conn.execute(sql, tuple(params)).fetchall()
    for row in rows:
        row_name = str(row["name"])
        if str(row["target_kind"]) != "file":
            continue
        if str(row["action"]) == "rename" and str(row["new_name"]) == current_name:
            current_name = row_name
            continue
        if row_name != current_name:
            continue
        action = str(row["action"])
        if action == "create":
            content = str(row["data"] or "")
        elif action == "patch":
            if content is None:
                raise ValueError(f"Cannot reconstruct {name}: missing create before patch")
            content = apply_unified_diff(content, str(row["data"] or ""))
        elif action == "delete":
            content = str(row["data"] or "")
        elif action == "rename":
            current_name = str(row["new_name"])
    if content is None:
        raise ValueError(f"No content history found for {name}")
    return content


def _is_binary_file(path: Path) -> bool:
    try:
        path.read_text(encoding="utf-8")
        return False
    except UnicodeDecodeError:
        return True


def repair_vault_state(*, root_path: str | None = None, vault_id: int | None = None) -> str:
    if root_path is None and vault_id is None:
        raise ValueError("repair_vault requires root_path or vault_id")
    with vault_db() as conn:
        if vault_id is not None:
            vault_row = conn.execute("SELECT id, root_path FROM vaults WHERE id = ?", (vault_id,)).fetchone()
        else:
            vault_row = conn.execute(
                "SELECT id, root_path FROM vaults WHERE root_path = ?",
                (str(Path(root_path or "").expanduser().resolve()),),
            ).fetchone()
        if vault_row is None:
            raise ValueError("Vault not found")
        vault = VaultRecord(id=int(vault_row["id"]), root_path=Path(str(vault_row["root_path"])).resolve())
        disk_files: dict[str, str] = {}
        skipped: list[str] = []
        for candidate in sorted(vault.root_path.rglob("*")):
            if not candidate.is_file():
                continue
            if _is_binary_file(candidate):
                skipped.append(relative_name(vault.root_path, candidate))
                continue
            disk_files[relative_name(vault.root_path, candidate)] = _read_text_file(candidate)
        file_rows = conn.execute("SELECT name, checksum FROM files WHERE vault_id = ?", (vault.id,)).fetchall()
        metadata = {str(row["name"]): str(row["checksum"]) for row in file_rows}
        created: list[str] = []
        patched: list[str] = []
        deleted: list[str] = []
        quarantined: list[str] = []
        for name, content in disk_files.items():
            checksum = sha256_text(content)
            if name not in metadata:
                lsn = _insert_log_row(
                    conn,
                    vault=vault,
                    name=name,
                    target_kind="file",
                    action="create",
                    data=content,
                    new_name=None,
                    recorded_at=utc_now(),
                )
                _upsert_file_state(
                    conn,
                    vault_id=vault.id,
                    name=name,
                    lsn=lsn,
                    checksum=checksum,
                    recorded_at=utc_now(),
                )
                created.append(name)
                continue
            if metadata[name] != checksum:
                previous = reconstruct_file_content(conn, vault_id=vault.id, name=name)
                patch = "".join(
                    difflib.unified_diff(
                        previous.splitlines(keepends=True),
                        content.splitlines(keepends=True),
                        fromfile=name,
                        tofile=name,
                    )
                )
                lsn = _insert_log_row(
                    conn,
                    vault=vault,
                    name=name,
                    target_kind="file",
                    action="patch",
                    data=patch,
                    new_name=None,
                    recorded_at=utc_now(),
                )
                _upsert_file_state(
                    conn,
                    vault_id=vault.id,
                    name=name,
                    lsn=lsn,
                    checksum=checksum,
                    recorded_at=utc_now(),
                )
                patched.append(name)
        timestamp = utc_now().replace(":", "-")
        for name in sorted(set(metadata) - set(disk_files)):
            content = reconstruct_file_content(conn, vault_id=vault.id, name=name)
            quarantine_path = VAULT_QUARANTINE_DIR / str(vault.id) / timestamp / name
            quarantine_path.parent.mkdir(parents=True, exist_ok=True)
            quarantine_path.write_text(content, encoding="utf-8")
            quarantined.append(name)
            _insert_log_row(
                conn,
                vault=vault,
                name=name,
                target_kind="file",
                action="delete",
                data=content,
                new_name=None,
                recorded_at=utc_now(),
            )
            conn.execute("DELETE FROM files WHERE vault_id = ? AND name = ?", (vault.id, name))
            deleted.append(name)
        conn.commit()
    parts = [
        "mode=repair_vault",
        f"vault_id={vault.id}",
        f"root_path={vault.root_path}",
        f"created={created}",
        f"patched={patched}",
        f"deleted={deleted}",
        f"quarantined={quarantined}",
        f"skipped_binary={skipped}",
    ]
    return "\n".join(parts)
