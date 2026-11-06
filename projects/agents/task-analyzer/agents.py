"""Claude CLI dispatch for ingest.py's agent_runner contract.

Batch up to 12 items in isolated scratch directories, write the full prompt
and input files there, and launch ``claude -p`` with a pointer to that prompt.
Capture stdout/stderr in claude.log. Validate the produced JSON, retry invalid
items once, and atomically stage valid results. Invalid model output remains
an open gap; a failed retry process with missing results raises with its log.

Models default to each prompt's model_hint, overridable by ``model``. Resolve
the executable from ``claude_bin``, then ``TASK_ANALYZER_CLAUDE_BIN``, then
``claude`` on PATH. A missing executable fails before dispatch.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import assets

_ROOT = Path(__file__).resolve().parent
_CLAUDE_BIN = "claude"
_CLAUDE_BIN_ENV_VAR = "TASK_ANALYZER_CLAUDE_BIN"

_MAX_BATCH_SIZE = 12
_LOG_TAIL_CHARS = 4000

# kind -> prompt asset basename (hyphen for phase-labeling, matching
# prompts/phase-labeling.md -- ingest.py's KIND_PROMPT_NAME uses the same
# mapping; shared with ingest.py).
_PROMPT_NAME = {"complexity": "complexity", "grading": "grading", "phase_labeling": "phase-labeling"}

# kind -> rubric/taxonomy asset path, substituted into {{RUBRIC_PATH}}.
_RUBRIC_PATH = {
    "complexity": _ROOT / "rubrics" / "complexity.md",
    "grading": _ROOT / "rubrics" / "grading.md",
    "phase_labeling": _ROOT / "rubrics" / "phase-taxonomy.md",
}

# kind -> (entity key field, input-blob field, manifest blob-reference field).
# The manifest field names match how each prompt's own text refers to them
# (complexity: "brief_file"; grading: "review-text file references" -- a
# list, since a task can have multiple review rounds joined into one text;
# phase_labeling: "a reference to that session's turn-by-turn timeline").
_KIND_FIELDS = {
    "complexity": ("task_key", "brief_text", "brief_file"),
    "grading": ("task_key", "review_text", "review_files"),
    "phase_labeling": ("session_key", "timeline", "timeline_file"),
}

# kind -> required top-level keys of a valid produced JSON file for that
# kind. Matches ingest.py's KIND_REQUIRED_KEYS exactly (duplicated
# because both modules implement the staging contract).
#
# followup-7, defect 2: "grading" items have a second valid shape --
# prompts/grading.md's "Ungradeable" contract -- for when every review
# joined to a task turns out to be mis-joined (or grading is otherwise
# impossible) and the agent correctly declines to grade rather than
# fabricating a verdict. That shape carries the SAME required keys (with
# G1-G5/verdict_sequence/rounds_to_accept/final_grade/evidence set to
# `null` rather than real values) plus one additional key, `reason` (a
# non-empty string).
_REQUIRED_KEYS = {
    "complexity": {"task_key", "C1", "C2", "C3", "C4", "C5", "C6", "C7", "composite", "rationale"},
    "grading": {
        "task_key", "G1", "G2", "G3", "G4", "G5", "n_critical", "n_important", "n_minor",
        "verdict_sequence", "rounds_to_accept", "final_grade", "evidence",
        "reviewer_models", "excluded_reviews",
    },
    "phase_labeling": {"session_key", "labels"},
}

# followup-7, fix-round-1 (finding 2): the two valid grading shapes are
# discriminated on SUBSTANCE (whether the G fields are null), never on the
# mere presence of a `reason` key -- an earlier version of this fix used
# `reason`'s presence alone, which would have silently suppressed a real,
# populated grade that happened to also carry a harmless explanatory
# `reason`. Matches ingest.py's identical, deliberately-duplicated copy.
_GRADING_G_FIELDS = ("G1", "G2", "G3", "G4", "G5")


def _grading_shape(data: dict) -> Optional[str]:
    """``"graded"`` when every G field is present and non-null (`reason`,
    if present, is ignored for this classification); ``"ungradeable"``
    when every G field is null AND `reason` is a non-empty string;
    ``None`` (invalid) for anything else -- in particular a half-null
    hybrid (some G fields null, some not)."""
    g_values = [data.get(f) for f in _GRADING_G_FIELDS]
    n_null = sum(1 for v in g_values if v is None)
    if n_null == 0:
        return "graded"
    if n_null == len(g_values):
        reason = data.get("reason")
        if isinstance(reason, str) and reason.strip():
            return "ungradeable"
        return None
    return None  # half-null hybrid -- reject


def _chunks(items: List[dict], size: int) -> List[List[dict]]:
    return [items[i : i + size] for i in range(0, len(items), size)]


def _validate_output(kind: str, data: Any) -> bool:
    if not isinstance(data, dict):
        return False
    if not _REQUIRED_KEYS[kind].issubset(data.keys()):
        return False
    if kind == "phase_labeling":
        labels = data.get("labels")
        if not isinstance(labels, dict) or not labels:
            return False
    if kind == "grading":
        if _grading_shape(data) is None:
            return False
    return True


def _staging_filename(entity_key: str, version: str, input_sha256: str) -> str:
    return f"{entity_key}__v{version}__{input_sha256[:12]}.json"


def _write_staged(staging_dir: Path, kind: str, entity_key: str, version: str, input_sha256: str, data: dict) -> Path:
    out_dir = Path(staging_dir) / kind
    out_dir.mkdir(parents=True, exist_ok=True)
    fname = _staging_filename(entity_key, version, input_sha256)
    final = out_dir / fname
    tmp = out_dir / (fname + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    os.replace(tmp, final)
    return final


def _resolve_claude_bin(claude_bin: Optional[str]) -> Path:
    executable = claude_bin if claude_bin is not None else os.environ.get(_CLAUDE_BIN_ENV_VAR) or _CLAUDE_BIN
    resolved = shutil.which(str(executable))
    if resolved is None:
        raise RuntimeError(
            f"Claude executable not found: {executable}. Resolution order: "
            f"claude_bin param -> {_CLAUDE_BIN_ENV_VAR} env var -> {_CLAUDE_BIN} on PATH."
        )
    return Path(resolved).resolve()


def _render_prompt(prompt_text: str, rubric_path: Path, items_json_path: Path, output_dir: Path) -> str:
    return (
        prompt_text.replace("{{RUBRIC_PATH}}", str(rubric_path))
        .replace("{{ITEMS_JSON_PATH}}", str(items_json_path))
        .replace("{{OUTPUT_DIR}}", str(output_dir))
    )


def _invoke_claude(work_dir: Path, model: str, prompt_path: Path, claude_bin: Path) -> Tuple[int, str]:
    """Run a headless scorer; return its exit code and captured log tail."""
    pointer = f"Read your full task prompt at {prompt_path} and follow it exactly."
    cmd = [
        str(claude_bin), "-p", pointer,
        "--model", model,
        # Scorers write staged output inside their scratch directory.
        "--permission-mode", "acceptEdits",
    ]
    log_path = work_dir / "claude.log"
    with open(os.devnull, "rb") as devnull, open(log_path, "wb") as log_f:
        result = subprocess.run(cmd, stdin=devnull, stdout=log_f, stderr=subprocess.STDOUT, cwd=str(work_dir), check=False)
    try:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        log_text = ""
    return result.returncode, log_text[-_LOG_TAIL_CHARS:]


def _dispatch_batch(
    kind: str, items: List[dict], staging_dir: Path, model: str, prompt_text: str, claude_bin: Path,
) -> Tuple[Dict[str, dict], int, str]:
    """One Claude invocation for at most ``_MAX_BATCH_SIZE`` items. Returns
    ``(results, returncode, log_tail)``: ``results`` maps entity_key ->
    validated output dict for whichever items produced a valid output file
    (items that failed are simply absent); ``returncode``/``log_tail`` are
    the subprocess's exit code and captured log tail, for the caller to
    decide whether a nonzero exit needs to be surfaced."""
    key_field, blob_field, manifest_field = _KIND_FIELDS[kind]

    work_dir = (Path(staging_dir) / kind / "_work" / uuid.uuid4().hex).resolve()
    output_dir = work_dir / "output"
    output_dir.mkdir(parents=True, exist_ok=True)

    manifest = []
    for i, item in enumerate(items):
        blob_path = work_dir / f"item_{i}.txt"
        blob_path.write_text(item[blob_field], encoding="utf-8")
        entry = {key_field: item[key_field]}
        if manifest_field == "review_files":
            entry[manifest_field] = [str(blob_path)]
        else:
            entry[manifest_field] = str(blob_path)
        manifest.append(entry)

    items_json_path = work_dir / "items.json"
    items_json_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    # Copy the rubric into the work dir so every path the scorer touches is
    # under its own cwd -- headless sessions cannot answer the approval
    # prompt that reads outside the sandbox root can trigger (mode/model
    # dependent; observed blocking haiku and sonnet scorers alike).
    local_rubric_path = work_dir / "rubric.md"
    local_rubric_path.write_text(Path(_RUBRIC_PATH[kind]).read_text(encoding="utf-8"), encoding="utf-8")

    rendered = _render_prompt(prompt_text, local_rubric_path, items_json_path, output_dir)
    prompt_path = work_dir / "prompt.md"
    prompt_path.write_text(rendered, encoding="utf-8")

    returncode, log_tail = _invoke_claude(work_dir, model, prompt_path, claude_bin)

    results: Dict[str, dict] = {}
    for item in items:
        entity_key = item[key_field]
        out_path = output_dir / f"{entity_key}.json"
        if not out_path.exists():
            continue
        try:
            data = json.loads(out_path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            continue
        if _validate_output(kind, data):
            results[entity_key] = data
    return results, returncode, log_tail


def claude_runner(
    kind: str,
    items: List[dict],
    staging_dir: Path,
    *,
    model: Optional[str] = None,
    claude_bin: Optional[str] = None,
) -> List[Path]:
    """Real ``agent_runner`` for ``ingest.py`` (see ``ingest.py``'s module
    docstring for the exact contract this must satisfy). Batches ``items``
    into groups of at most ``_MAX_BATCH_SIZE``, dispatches each batch to
    Claude, validates/retries-once, and returns the final staged paths
    actually written.

    ``claude_bin`` overrides the Claude executable; see the module docstring
    for resolution order and retry behavior."""
    if kind not in _KIND_FIELDS:
        raise ValueError(f"unknown kind: {kind!r}")
    if not items:
        return []

    claude_bin = _resolve_claude_bin(claude_bin)

    prompt_text, _prompt_version, model_hint = assets.load_prompt(_PROMPT_NAME[kind])
    resolved_model = model if model is not None else model_hint

    key_field, _blob_field, _manifest_field = _KIND_FIELDS[kind]
    staging_dir = Path(staging_dir)

    written: List[Path] = []
    for batch in _chunks(items, _MAX_BATCH_SIZE):
        results, _rc, _log_tail = _dispatch_batch(kind, batch, staging_dir, resolved_model, prompt_text, claude_bin)

        invalid = [item for item in batch if item[key_field] not in results]
        if invalid:
            retry_results, retry_rc, retry_log_tail = _dispatch_batch(
                kind, invalid, staging_dir, resolved_model, prompt_text, claude_bin
            )
            results.update(retry_results)

            still_invalid = [item for item in invalid if item[key_field] not in results]
            if still_invalid and retry_rc != 0:
                keys = ", ".join(item[key_field] for item in still_invalid)
                raise RuntimeError(
                    f"Claude exited {retry_rc} dispatching {kind} batch for [{keys}] "
                    f"(after 1 retry) -- refusing to silently drop these items. "
                    f"Log tail:\n{retry_log_tail}"
                )

        for item in batch:
            entity_key = item[key_field]
            data = results.get(entity_key)
            if data is None:
                continue  # still invalid after retry (non-crash validation failure) -- left as an open gap
            path = _write_staged(staging_dir, kind, entity_key, item["version"], item["input_sha256"], data)
            written.append(path)

    return written
