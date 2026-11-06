"""Idempotent ingest driver for the SDD task-analyzer pipeline (design.md D3).

Owns the join algorithm discovery.py deliberately left out: given a set of
landed changes (discovery.landed_changes), find their SDD task briefs and
match session transcripts to tasks, quarantining ambiguous joins rather than
guessing (discovery.Quarantine). Two public entry points:

- ``plan_work(conn, repo_root) -> WorkPlan``: a pure, read-only survey of
  what a real run would do -- new changes/tasks/sessions to ingest, and
  agentic gaps (missing complexity/grades/phase-label rows), each carrying
  the cache-key reason it's a gap and whether a staged file already
  satisfies it.
- ``run(conn, repo_root, *, dry_run, no_agents, rescore, agent_runner) ->
  RunReport``: executes the plan. Each task's mechanical rows (task row +
  its sessions) and agentic rows are written in one SQLite transaction --
  a crash rolls the whole task back, and the next run resumes from
  whatever was already staged to disk (D3's crash-resume contract).

Directory conventions owned by this module (not fixed elsewhere):

- Task briefs for a landed change ``<name>`` live at
  ``.superpowers/sdd/<name>/<task_key>-brief.md``, read via
  ``git ls-tree -r``/``git show`` at the *resolved commit SHA* of the
  archive ref (never the filesystem/working tree -- D3 requires discovery
  to be a pure diff against a landed git ref, and that applies to brief
  content too, not just which changes/tasks exist: a brief edited or added
  only in the working tree must not affect ingestion). ``<task_key>`` is
  whatever precedes ``-brief.md`` and looks like a task key (``task-N``,
  ``pM-task-N``, ...); files like ``task-N-review-brief.md`` are not task
  briefs and are skipped. This assumes the SDD directory name equals the
  openspec change name -- a simplifying assumption for new changes going
  forward; the historical corpus (differently-named SDD dirs, e.g. this
  very change's own ``.superpowers/sdd/task-analyzer/``) is Task 8's
  migration script's problem, not this one's.
- **Plan-derived briefs, the fallback (followup-6).** ``.superpowers/`` is
  the Superpowers workflow's own uncommitted scratch space -- the SDD
  workflow never commits it, so a landed change's task briefs are, in the
  steady-state common case, simply absent at the archive ref (only the
  2026-07-19 migration's historical corpus happened to have them, because
  that extraction read live worktree scratch, not a committed ref). When
  ``_iter_briefs`` finds ZERO committed briefs for a landed change, tasks
  are instead derived from that change's own committed Superpowers PLAN
  file (``discovery.landed_changes``'s already-resolved ``plan_path``,
  read via ``git show``, never the working tree): every ``Task N:``
  heading (any markdown heading level, colon required -- see
  ``_PLAN_TASK_HEADING_RE``'s comment for why: every real task heading on
  main has one, and it's what excludes a plan's own "Task N Review"
  sub-headings from being mistaken for a task boundary, skipping headings
  inside fenced code blocks) opens a task section running through the
  line before the next heading at the SAME level (whether or not that
  heading is itself a task heading). ``task_key = "task-N"``;
  ``brief_path`` is the synthetic reference
  ``"<plan_path>#task-N"`` (``_PLAN_DERIVED_PATH_RE``), distinguishing a
  plan-derived brief from a real committed brief file's path everywhere
  ``disc.briefs`` values are used (``_brief_text_for`` dispatches on this
  marker). This is a per-CHANGE, all-or-nothing fallback -- real briefs,
  when ANY exist for a change, always win outright (plan derivation is
  never mixed in alongside them for that same change); a change with
  neither committed briefs nor a resolvable plan file ingests as a change
  row with zero tasks, exactly as before this fallback existed.
  ``brief_sha256``/``brief_bytes`` and every downstream agentic cache key
  (complexity/grading input hashes) derive from the resulting
  ``brief_text`` exactly as for a real brief -- no special-casing.
- Session transcripts are discovered from two corpora, independent of
  ``repo_root`` (a change's sessions accumulate across every worktree
  checkout, never just whichever one happens to be on disk right now):
  codex rollouts under ``codex_sessions_root`` (default
  ``~/.codex/sessions``, glob ``**/rollout-*.jsonl``) and claude project
  transcripts under ``claude_projects_root`` (default
  ``~/.claude/projects``, glob ``**/*.jsonl``). Both roots are overridable
  keyword arguments (tests point them at fixture directories).
- A session joins a task when ``discovery.task_keys(session.prompt)``
  yields a ``task`` key matching exactly one brief's task_key among the
  briefs whose ``change_dir`` (or ``openspec_change``) equals a change
  being processed this run; sessions whose task key matches more than one
  candidate are quarantined (``task_id`` left NULL, never guessed);
  sessions with no task key at all are not part of any task and are
  skipped (not stored) -- they're irrelevant chatter, not ambiguous joins.

Agentic dispatch contract (for Task 7's real ``agent_runner``): ``kind`` is
one of ``complexity`` / ``grading`` / ``phase_labeling`` (matching the
prompt asset basenames, hyphen for phase-labeling: ``prompts/complexity.md``,
``prompts/grading.md``, ``prompts/phase-labeling.md``). ``agent_runner`` is
called as ``agent_runner(kind, items, staging_dir)`` where ``items`` is a
list of plain dicts, one per gap needing dispatch (already filtered to
exclude gaps already satisfied by a valid staged file). ``task_key``/
``session_key`` below is the *qualified* cache-key entity (``"<change>--
<task_key>"`` for complexity/grading, the full ``provider:native-id`` for
phase_labeling) -- i.e. exactly ``GapItem.entity_key`` -- not the bare task
key, since staging filenames must be unique across changes that reuse the
same task numbering:

- complexity: ``{"task_key", "version", "input_sha256", "brief_text"}``
- grading:    ``{"task_key", "version", "input_sha256", "review_text"}``
- phase_labeling: ``{"session_key", "version", "input_sha256", "timeline"}``

The callable is expected to write valid staged JSON files (design.md D3's
``staging/<kind>/<entity_key>__v<version>__<sha256[:12]>.json``, atomic
tmp+rename) for whichever items it manages to score, and return the list of
paths it wrote; ``ingest.py`` re-reads staging afterwards rather than
trusting the return value's completeness, so partial batches (a crash
partway through) still get picked up correctly.

Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

import assets
import costs
import db
import discovery
import extractors

_ROOT = Path(__file__).resolve().parent
DEFAULT_STAGING_DIR = _ROOT / "staging"
DEFAULT_CODEX_SESSIONS_ROOT = Path("~/.codex/sessions").expanduser()
DEFAULT_CLAUDE_PROJECTS_ROOT = Path("~/.claude/projects").expanduser()
# The one-shot 2026-07-19 migration's source tree (migrate_v0.py) -- also
# `backfill-turns`'s fallback source of per-turn phase labels for migrated_v0
# sessions, none of which have a staging/phase_labeling/ file of their own
# (they never went through this module's live agentic-dispatch path).
DEFAULT_ANALYSIS_DIR = _ROOT.parents[2] / "analysis" / "sdd-model-analysis" / "data"

# kind (agent-dispatch vocabulary, Task 7) -> table name
KIND_TABLE = {"complexity": "complexity", "grading": "grades", "phase_labeling": "phase_tokens"}
# kind -> rubric/taxonomy asset path (current-version source of truth)
KIND_RUBRIC_PATH = {
    "complexity": _ROOT / "rubrics" / "complexity.md",
    "grading": _ROOT / "rubrics" / "grading.md",
    "phase_labeling": _ROOT / "rubrics" / "phase-taxonomy.md",
}
# kind -> the version column name in that kind's table
KIND_VERSION_COLUMN = {
    "complexity": "rubric_version",
    "grading": "rubric_version",
    "phase_labeling": "taxonomy_version",
}
# kind -> prompt asset basename (note the hyphen for phase-labeling, unlike
# the underscore "kind" vocabulary itself).
KIND_PROMPT_NAME = {"complexity": "complexity", "grading": "grading", "phase_labeling": "phase-labeling"}
# kind -> required top-level keys of a valid staged JSON file for that kind.
KIND_REQUIRED_KEYS = {
    "complexity": {"task_key", "C1", "C2", "C3", "C4", "C5", "C6", "C7", "composite", "rationale"},
    "grading": {
        "task_key", "G1", "G2", "G3", "G4", "G5", "n_critical", "n_important", "n_minor",
        "verdict_sequence", "rounds_to_accept", "final_grade", "evidence",
        "reviewer_models", "excluded_reviews",
    },
    "phase_labeling": {"session_key", "labels"},
}

# followup-7, fix-round-1 (finding 2): a grading result's shape is
# discriminated on its SUBSTANCE (whether the G fields are null), never on
# the mere presence of a `reason` key -- a populated grade may legitimately
# carry a harmless explanatory `reason` too, and must still ingest as a
# grade. Matches agents.py's identical, deliberately-duplicated copy (see
# that module's docstring for why the duplication).
_GRADING_G_FIELDS = ("G1", "G2", "G3", "G4", "G5")


def _grading_shape(data: dict) -> Optional[str]:
    """``"graded"`` when every G field is present and non-null (a `reason`
    key, if present, is ignored for this classification); ``"ungradeable"``
    when every G field is null AND `reason` is a non-empty string
    (prompts/grading.md's Ungradeable shape); ``None`` (invalid) for
    anything else -- in particular a half-null hybrid (some G fields null,
    some not) or an all-null shape with no real `reason`, both rejected
    outright rather than guessed at."""
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


REASON_MISSING = "missing"
REASON_INPUT_CHANGED = "input_changed"
REASON_STALE_VERSION = "stale_version"


# --------------------------------------------------------------------------
# Public data shapes
# --------------------------------------------------------------------------


@dataclass
class GapItem:
    """One agentic cache-miss: entity ``entity_key`` (a task_key-qualified
    string for complexity/grading, or a full ``provider:native-id`` session
    id for phase_labeling) needs (re-)scoring at ``version`` because
    ``reason`` (one of the ``REASON_*`` constants). ``staged_path`` is set
    when a valid staged file already satisfies this exact cache key --
    ``run`` will use it instead of dispatching."""

    entity_key: str
    kind: str
    version: str
    input_sha256: str
    reason: str
    stale_versions: List[str] = field(default_factory=list)
    staged_path: Optional[str] = None

    @property
    def staging_satisfied(self) -> bool:
        return self.staged_path is not None


@dataclass
class WorkPlan:
    """Pure survey of what a real run would do. See module docstring."""

    ref_sha: str
    new_changes: List[str] = field(default_factory=list)
    new_tasks: List[str] = field(default_factory=list)  # "<change>/<task_key>"
    new_sessions: List[str] = field(default_factory=list)  # "provider:native-id"
    missing_complexity: List[GapItem] = field(default_factory=list)
    missing_grades: List[GapItem] = field(default_factory=list)
    missing_phase_labels: List[GapItem] = field(default_factory=list)
    quarantined: List[Any] = field(default_factory=list)  # discovery.Quarantine
    unlanded: List[str] = field(default_factory=list)

    def is_empty(self) -> bool:
        return not (
            self.new_changes
            or self.new_tasks
            or self.new_sessions
            or self.missing_complexity
            or self.missing_grades
            or self.missing_phase_labels
            or self.quarantined
        )


@dataclass
class RunReport:
    ref_sha: str
    dry_run: bool
    changes_written: int = 0
    tasks_written: int = 0
    sessions_written: int = 0
    rows_written: Dict[str, int] = field(default_factory=dict)  # kind -> upserted count
    dispatched: Dict[str, int] = field(default_factory=dict)  # kind -> agent_runner calls
    staged_used: Dict[str, int] = field(default_factory=dict)  # kind -> satisfied from staging
    stale: Dict[str, int] = field(default_factory=dict)  # kind -> stale, not rescored this run
    quarantined: List[Any] = field(default_factory=list)
    unlanded: List[str] = field(default_factory=list)
    plan: Optional[WorkPlan] = None
    # followup-7, defect 2: distinguish "the agent correctly declined --
    # nothing gradeable" from "infra genuinely failed to produce output",
    # so a run report can show scored/ungradeable/failed counts separately
    # instead of only a single crash-or-succeed outcome. `ungradeable`/
    # `failed` are kind -> count (mirroring rows_written's shape);
    # `ungradeable_items`/`failed_items` carry the per-item detail (entity
    # key + reason/error) for the report and ingest_log.
    ungradeable: Dict[str, int] = field(default_factory=dict)
    failed: Dict[str, int] = field(default_factory=dict)
    ungradeable_items: List[Dict[str, str]] = field(default_factory=list)
    failed_items: List[Dict[str, str]] = field(default_factory=list)
    # followup-7, defect 1 amendment: sessions whose sessions.task_id was
    # reconciled this run because it disagreed with today's evidence-based
    # join/quarantine outcome (e.g. residue from a run predating the
    # evidence-gated join fix). See run()'s healing pass.
    healed_sessions: int = 0
    healed_session_ids: List[str] = field(default_factory=list)
    # fix-round-1, finding 1: a stale `grades` row (current rubric_version,
    # OLD input_sha256 -- REASON_INPUT_CHANGED) deleted because this task's
    # fresh result turned out to be ungradeable; see the grading branch of
    # _resolve_gap. Also surfaced per-item via
    # ungradeable_items[i]["superseded_grades_row"].
    superseded_grades: int = 0

    @property
    def total_writes(self) -> int:
        return (
            self.changes_written
            + self.tasks_written
            + self.sessions_written
            + sum(self.rows_written.values())
        )


# --------------------------------------------------------------------------
# Internal discovery/join (shared by plan_work and run)
# --------------------------------------------------------------------------


@dataclass
class _JoinedSession:
    rec: Any  # extractors.SessionRecord
    provider: str
    role: str
    change_name: str
    task_key: str
    review_round: int = 0

    @property
    def session_id(self) -> str:
        return f"{self.provider}:{self.rec.session_id}"


@dataclass
class _Discovery:
    ref_sha: str
    repo_root: Any
    changes: List[Any]  # discovery.LandedChange, in scope for this run
    unlanded: List[str]
    briefs: Dict[Tuple[str, str], str]  # (change_name, task_key) -> git-relative brief path
    joined: Dict[Tuple[str, str], List[_JoinedSession]]  # (change_name, task_key) -> sessions
    quarantined: List[Any]  # discovery.Quarantine, same order as quarantined_sessions
    quarantined_sessions: List[_JoinedSession]  # task_key/change_name are "" (unjoined)


_TASK_KEY_RE = re.compile(r"(?:[a-z0-9]+-)?task-\d+$")


def _brief_task_key(stem: str) -> Optional[str]:
    """``stem`` is a brief filename with ``-brief.md``/``-implementer-prompt.md``
    already stripped. Returns the task key if ``stem`` looks like one
    (``task-N`` or ``<prefix>-task-N``), else None (filters out siblings
    like ``task-1-review-brief.md`` whose stem is ``task-1-review``)."""
    if _TASK_KEY_RE.fullmatch(stem):
        return stem
    return None


def _git(repo_root, args: List[str]) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo_root)] + args, capture_output=True, text=True, check=True,
    )
    return result.stdout


def _git_ls_tree_r(repo_root, sha: str, path: str) -> List[str]:
    """Recursive `git ls-tree -r <sha> -- <path>`, blob paths only (full
    repo-relative paths, since -r already expands directories). Empty list
    if the path doesn't exist at that ref (git exits 0 with empty output,
    not an error) -- same convention as discovery._ls_tree."""
    out = _git(repo_root, ["ls-tree", "-r", sha, "--", path])
    paths = []
    for line in out.splitlines():
        if not line.strip():
            continue
        meta, _, name = line.partition("\t")
        parts = meta.split()
        if len(parts) != 3:
            continue
        _mode, typ, _blob_sha = parts
        if typ == "blob":
            paths.append(name)
    return paths


def _git_show(repo_root, sha: str, path: str) -> str:
    """Read a file's content at a specific commit -- never the working
    tree (D3: discovery, including brief content, is a pure diff against a
    landed git ref)."""
    return _git(repo_root, ["show", f"{sha}:{path}"])


def _iter_briefs(repo_root, ref_sha: str, change_name: str) -> List[Tuple[str, str]]:
    """Task briefs for `change_name`, read via git ls-tree at `ref_sha` --
    never the filesystem, so a brief added or edited only in the working
    tree (not committed/landed) is invisible here."""
    prefix = f".superpowers/sdd/{change_name}/"
    out = []
    for path in sorted(_git_ls_tree_r(repo_root, ref_sha, prefix)):
        name = Path(path).name
        for suffix in ("-brief.md", "-implementer-prompt.md"):
            if name.endswith(suffix):
                stem = name[: -len(suffix)]
                task_key = _brief_task_key(stem)
                if task_key:
                    out.append((task_key, path))
                break
    return out


# Plan-derived briefs (followup-6, design.md D3 amendment): a landed
# change with zero committed briefs falls back to its Superpowers plan
# file's own "Task N:" sections.
#
# Accepted task-heading form, from direct inspection of every plan
# committed on main (``docs/superpowers/plans/*.md``, 362 real task
# headings surveyed): ANY markdown heading level (real plans use "## Task
# N" as often as "### Task N") followed by "Task", whitespace, digits, and
# a COLON -- every single real task heading in the corpus has the colon
# immediately after the number, with no exception. The colon is required
# specifically to exclude a plan's own "### Task N Review" sub-headings
# (a real, common shape -- e.g. docs/superpowers/plans/2026-06-26-add-
# synth-midi-controller-io.md has "### Task 1 Review" before "### Task
# 2"): without it, followup-6's original fix-round-1 review found this
# folded an entire review section's instructions/results into the
# preceding task's brief_text, polluting brief_sha256 and every downstream
# agentic cache key. A section runs from its own heading line through the
# line before the NEXT heading at the SAME level (whether or not that
# heading is itself a task heading) -- not "the next Task heading of any
# number" (the `subagent-driven-development` skill's own `task-brief`
# script uses that looser rule, which is exactly what let "Task 1 Review"
# leak into Task 1's section in the first place; same-level closing fixes
# it while still leaving a task's own deeper subsections, e.g. "####
# Verification", correctly inside its section).
_PLAN_TASK_HEADING_RE = re.compile(r"^(#+)[ \t]+Task[ \t]+(\d+):")
_HEADING_RE = re.compile(r"^(#+)[ \t]")
_FENCE_LINE_RE = re.compile(r"^```")
# `disc.briefs` values are normally a real committed brief's git-relative
# path; a plan-derived brief instead uses this synthetic marker so
# `_brief_text_for` knows to re-extract from the plan rather than `git
# show` the "path" directly (which isn't a real blob).
_PLAN_DERIVED_PATH_RE = re.compile(r"^(?P<plan_path>.+\.md)#task-(?P<task_num>\d+)$")


def _task_sections_from_plan(plan_text: str) -> Dict[int, str]:
    """``{task_number: full_section_text}`` for every ``Task N:`` heading
    in ``plan_text`` (any heading level, skipping headings inside fenced
    code blocks) -- a section runs from its own heading line through the
    line before the next heading at the SAME level, per this module's
    task-heading comment above. Returns ``{}`` for a plan with no task
    headings at all (not an error -- see ``_iter_plan_derived_briefs``)."""
    sections: Dict[int, List[str]] = {}
    current: Optional[int] = None
    current_level: Optional[int] = None
    infence = False
    for line in plan_text.splitlines():
        if _FENCE_LINE_RE.match(line):
            infence = not infence
        if not infence:
            heading_m = _HEADING_RE.match(line)
            if heading_m and current is not None and len(heading_m.group(1)) == current_level:
                # A heading at the SAME level as the currently open task
                # closes it, whether or not this heading is itself a new
                # task heading (e.g. "Task N Review" at the same level).
                current = None
                current_level = None
            task_m = _PLAN_TASK_HEADING_RE.match(line)
            if task_m:
                current = int(task_m.group(2))
                current_level = len(task_m.group(1))
                sections.setdefault(current, [])
        if current is not None:
            sections[current].append(line)
    return {n: "\n".join(lines) for n, lines in sections.items()}


def _iter_plan_derived_briefs(repo_root, ref_sha: str, plan_path: str) -> List[Tuple[str, str]]:
    """Fallback for a landed change with zero committed briefs
    (followup-6): ``[(task_key, synthetic_brief_path), ...]`` derived from
    ``plan_path``'s own committed ``Task N:`` sections at ``ref_sha``.
    Empty list (no error) if the plan file can't be read at that ref, or
    has no task headings at all -- a change in either state simply
    ingests with zero tasks, same as a change with no briefs and no plan
    path at all."""
    try:
        plan_text = _git_show(repo_root, ref_sha, plan_path)
    except subprocess.CalledProcessError:
        return []
    sections = _task_sections_from_plan(plan_text)
    return [(f"task-{n}", f"{plan_path}#task-{n}") for n in sorted(sections)]


def _brief_text_for(repo_root, ref_sha: str, brief_path: str) -> str:
    """A task's brief text given its ``disc.briefs`` value -- either a real
    committed brief file (``git show`` verbatim), or a synthetic
    plan-derived reference ``"<plan_path>#task-N"`` (followup-6): re-reads
    the plan file at ``ref_sha`` and re-extracts that task's section.
    Re-deriving on every call (rather than caching at discovery time)
    matches this module's existing pattern for real briefs, which are
    likewise re-fetched via ``git show`` at each of the two call sites
    that need brief text, not cached once and threaded through."""
    m = _PLAN_DERIVED_PATH_RE.match(brief_path)
    if not m:
        return _git_show(repo_root, ref_sha, brief_path)
    plan_text = _git_show(repo_root, ref_sha, m.group("plan_path"))
    sections = _task_sections_from_plan(plan_text)
    return sections[int(m.group("task_num"))]


def _iter_session_files(codex_root, claude_root) -> List[Tuple[str, Path]]:
    files: List[Tuple[str, Path]] = []
    if codex_root and Path(codex_root).is_dir():
        for p in sorted(Path(codex_root).glob("**/rollout-*.jsonl")):
            files.append(("codex", p))
    if claude_root and Path(claude_root).is_dir():
        for p in sorted(Path(claude_root).glob("**/*.jsonl")):
            files.append(("claude", p))
    return files


def _extract(provider: str, path: Path):
    if provider == "codex":
        return extractors.extract_codex(path)
    return extractors.extract_claude(path)


def _has_change_evidence(rec, landed_change: "discovery.LandedChange", keys: Dict[str, str]) -> bool:
    """True iff ``rec``'s own prompt affirmatively identifies
    ``landed_change`` SPECIFICALLY -- followup-7, defect 1: a session may
    only join a (change, task_key) pair with real evidence tying it to
    that exact change, never merely because narrowing to ``--change``
    happened to leave it as the sole remaining candidate (that made
    ``ingest.py --change add-sdd-task-analyzer`` join ~75 unrelated
    sessions to ``add-sdd-task-analyzer--task-1`` in the first real canary
    run, sessions that correctly quarantine as ambiguous in a full,
    unscoped run against all ~70 landed changes). Three evidence sources,
    reusing ``discovery.task_keys``'s own extraction rather than
    reimplementing it:

    - ``keys["change_dir"]``/``keys["openspec_change"]`` name the change
      explicitly (a ``.superpowers/sdd/<name>/task-N-brief.md`` path, or an
      ``openspec/changes/<name>`` path / "OpenSpec change `<name>`"
      mention).
    - ``keys["plan"]`` (a ``plans/<file>.md`` path mention) matches this
      change's own resolved ``plan_path`` basename.
    - the change's own SDD brief directory, ``.superpowers/sdd/<name>/``,
      appears literally in the prompt -- broader than the two
      ``task_keys``-derived checks above (which require an exact
      ``-brief.md``/``-implementer-prompt.md`` filename suffix): this also
      covers a reviewer/auditor session whose prompt names a sibling file
      under the same directory (a ``-report.md``/``-review-package.md``/
      etc., none of which ``task_keys`` extracts a change_dir from today).

    Deliberately does NOT try to resolve the historical mismatch between
    an SDD directory name and its openspec change name (e.g. this very
    project's own ``.superpowers/sdd/task-analyzer/`` for the
    ``add-sdd-task-analyzer`` change) -- ingest.py's module docstring
    already documents that as the migration script's problem, not the
    live join path's; a session referencing only the SDD-dir alias
    correctly quarantines here (see followup-7-report.md for how many of
    the real corpus's quarantines are exactly this case)."""
    name = landed_change.name
    if keys.get("change_dir") == name or keys.get("openspec_change") == name:
        return True
    plan_key = keys.get("plan")
    if plan_key and landed_change.plan_path and Path(landed_change.plan_path).name == plan_key:
        return True
    prompt = rec.prompt or ""
    if f".superpowers/sdd/{name}/" in prompt:
        return True
    return False


def _discover(
    conn,
    repo_root,
    *,
    archive_ref: str = "refs/heads/main",
    change: Optional[str] = None,
    codex_sessions_root=None,
    claude_projects_root=None,
) -> _Discovery:
    landed, ref_sha = discovery.landed_changes(repo_root, archive_ref=archive_ref)
    landed_by_name = {c.name: c for c in landed}

    unlanded: List[str] = []
    if change is not None:
        if change not in landed_by_name:
            unlanded.append(change)
            changes_in_scope = []
        else:
            changes_in_scope = [landed_by_name[change]]
    else:
        changes_in_scope = list(landed)

    # Join EVIDENCE is computed over the FULL landed set, always --
    # regardless of `--change` scoping (followup-7, defect 1). `--change`
    # narrows `changes_in_scope`, which in turn narrows `briefs` below
    # (and therefore which tasks/sessions actually get INGESTED this run
    # -- see run()/`_plan_from_disc`, both keyed off `disc.briefs`), but
    # it must never narrow which candidates a session's task_key is
    # matched against: doing so is exactly what made a bare "task-1"
    # mention join whatever the sole in-scope change happened to be.
    all_briefs: Dict[Tuple[str, str], str] = {}
    all_brief_index: Dict[str, List[str]] = {}  # task_key -> [change_name, ...]
    for c in landed:
        change_briefs = _iter_briefs(repo_root, ref_sha, c.name)
        if not change_briefs and c.plan_path:
            # Plan-derived fallback (followup-6): only when this change has
            # NO committed briefs at all -- real briefs always take
            # precedence outright, per-change, never mixed with plan
            # derivation for the same change.
            change_briefs = _iter_plan_derived_briefs(repo_root, ref_sha, c.plan_path)
        for task_key, path in change_briefs:
            all_briefs[(c.name, task_key)] = path
            all_brief_index.setdefault(task_key, []).append(c.name)

    in_scope_names = {c.name for c in changes_in_scope}
    briefs = {k: v for k, v in all_briefs.items() if k[0] in in_scope_names}

    codex_root = codex_sessions_root if codex_sessions_root is not None else DEFAULT_CODEX_SESSIONS_ROOT
    claude_root = claude_projects_root if claude_projects_root is not None else DEFAULT_CLAUDE_PROJECTS_ROOT

    joined: Dict[Tuple[str, str], List[_JoinedSession]] = {}
    quarantined: List[Any] = []
    quarantined_sessions: List[_JoinedSession] = []

    for provider, path in _iter_session_files(codex_root, claude_root):
        try:
            rec = _extract(provider, path)
        except Exception:
            continue

        # Mechanical, role-independent fact about the transcript (followup-4,
        # design.md D5 amendment): every verdict turn's own ended_at is a
        # review boundary once this session is classified as a reviewer
        # (discovery.review_boundaries only reads this for role=="reviewer";
        # computed unconditionally here since role isn't known yet).
        rec.verdict_boundaries = [
            t.ended_at for t in rec.turns if getattr(t, "is_verdict", False) and t.ended_at
        ]

        keys = discovery.task_keys(rec.prompt)
        task_key = keys.get("task")
        if not task_key:
            continue  # no task reference at all -- not part of this pipeline

        task_candidates = all_brief_index.get(task_key, [])
        role = discovery.classify_role(rec.prompt, rec.spawn_path)

        if len(task_candidates) == 0:
            continue  # no landed change has a brief for this key at all -- irrelevant, not ambiguous

        evidenced = [c for c in task_candidates if _has_change_evidence(rec, landed_by_name[c], keys)]

        if len(evidenced) == 0:
            # A task_key that some landed change(s) recognize, but this
            # session gives no affirmative evidence for any specific one
            # of them (followup-7, defect 1) -- e.g. a bare "implement
            # task 1" mention, or a reference to an SDD directory whose
            # name doesn't match its change's own openspec name. Quarantine,
            # never guess via "only one was in scope."
            quarantined.append(
                discovery.Quarantine(
                    session_id=f"{provider}:{rec.session_id}",
                    reason="no change evidence for task key",
                    candidates=sorted(f"{c}/{task_key}" for c in task_candidates),
                )
            )
            quarantined_sessions.append(
                _JoinedSession(rec=rec, provider=provider, role=role, change_name="", task_key="")
            )
            continue
        if len(evidenced) > 1:
            quarantined.append(
                discovery.Quarantine(
                    session_id=f"{provider}:{rec.session_id}",
                    reason="task key matches >1 task",
                    candidates=sorted(f"{c}/{task_key}" for c in evidenced),
                )
            )
            # Still recorded mechanically -- task_id NULL, never a guessed
            # join (design.md's Quarantine contract).
            quarantined_sessions.append(
                _JoinedSession(rec=rec, provider=provider, role=role, change_name="", task_key="")
            )
            continue

        change_name = evidenced[0]
        joined.setdefault((change_name, task_key), []).append(
            _JoinedSession(rec=rec, provider=provider, role=role, change_name=change_name, task_key=task_key)
        )

    # Assign review_round per task group (design D5), mechanical.
    for key, sessions in joined.items():
        round_map = discovery.assign_review_rounds(_with_role(sessions))
        for js in sessions:
            js.review_round = round_map.get(js.rec.session_id, 0)

    return _Discovery(
        ref_sha=ref_sha,
        repo_root=repo_root,
        changes=changes_in_scope,
        unlanded=unlanded,
        briefs=briefs,
        joined=joined,
        quarantined=quarantined,
        quarantined_sessions=quarantined_sessions,
    )


def _with_role(sessions: List[_JoinedSession]):
    """assign_review_rounds reads `.role`/`.started_at`/etc via getattr; bolt
    `role` onto each SessionRecord so it can be passed directly."""
    out = []
    for js in sessions:
        js.rec.role = js.role
        out.append(js)
    return [js.rec for js in out]


def _review_text(rec) -> str:
    if rec.last_message:
        return rec.last_message
    lines = []
    for t in rec.turns:
        for item in t.items:
            if item.startswith("SAY:"):
                lines.append(item[len("SAY:"):].strip())
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Cache-key gap computation (read-only; shared by plan_work and run)
# --------------------------------------------------------------------------


def _existing_task_id(conn, change_name: str, task_key: str) -> Optional[int]:
    row = conn.execute(
        "SELECT t.task_id FROM tasks t JOIN changes c ON c.change_id = t.change_id "
        "WHERE c.name = ? AND t.task_key = ?",
        (change_name, task_key),
    ).fetchone()
    return row[0] if row else None


def _version_int(v: str) -> int:
    """Versions are integer strings, compared numerically (design.md: "the
    row with the numerically greatest version" is current) -- never
    lexicographically (``"10" < "2"`` as strings, but 10 > 2 as versions).
    Non-numeric input sorts as always-oldest rather than raising, since this
    is used for sorting/reporting, not as a hard schema guarantee."""
    try:
        return int(v)
    except (TypeError, ValueError):
        return -1


def _current_and_stale(conn, table: str, version_col: str, id_col: str, id_val, current_version: str):
    rows = conn.execute(
        f'SELECT "{version_col}", input_sha256 FROM "{table}" WHERE "{id_col}" = ?',
        (id_val,),
    ).fetchall()
    current_target = _version_int(current_version)
    current_hash = None
    stale: List[str] = []
    for row in rows:
        v, h = row[0], row[1]
        if _version_int(v) == current_target:
            current_hash = h
        elif v not in stale:
            stale.append(v)
    stale.sort(key=_version_int)
    return current_hash, stale


def _staging_filename(entity_key: str, version: str, input_sha256: str) -> str:
    return f"{entity_key}__v{version}__{input_sha256[:12]}.json"


def _validate_staged(kind: str, data: Any, n_turns: Optional[int] = None) -> bool:
    if not isinstance(data, dict):
        return False
    if not KIND_REQUIRED_KEYS[kind].issubset(data.keys()):
        return False
    if kind == "phase_labeling":
        labels = data.get("labels")
        if not isinstance(labels, dict) or not labels:
            return False
        if n_turns is not None:
            for i in range(1, n_turns + 1):
                if str(i) not in labels:
                    return False
    if kind == "grading":
        # followup-7, fix-round-1 (finding 2): enforce the two valid
        # grading shapes exclusively -- reject a half-null hybrid outright
        # rather than let it slip through as either shape.
        if _grading_shape(data) is None:
            return False
    return True


def _check_staged(staging_dir, kind: str, entity_key: str, version: str, input_sha256: str, n_turns=None):
    """Returns (valid_path_or_None, data_or_None, exists: bool)."""
    fname = _staging_filename(entity_key, version, input_sha256)
    path = Path(staging_dir) / kind / fname
    if not path.exists():
        return None, None, False
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None, None, True
    if not _validate_staged(kind, data, n_turns=n_turns):
        return None, None, True
    return path, data, True


def _entity_key(change_name: str, task_key: str) -> str:
    return f"{change_name}--{task_key}"


def _gap_for(conn, kind: str, entity_key: str, id_col: str, id_val, input_text: str) -> Optional[GapItem]:
    version = assets.read_asset_version(KIND_RUBRIC_PATH[kind])
    input_sha256 = db.sha256_text(input_text)
    if id_val is None:
        return GapItem(entity_key=entity_key, kind=kind, version=version, input_sha256=input_sha256, reason=REASON_MISSING)

    current_hash, stale = _current_and_stale(conn, KIND_TABLE[kind], KIND_VERSION_COLUMN[kind], id_col, id_val, version)
    if current_hash == input_sha256:
        return None
    if current_hash is not None:
        return GapItem(entity_key=entity_key, kind=kind, version=version, input_sha256=input_sha256, reason=REASON_INPUT_CHANGED)
    if stale:
        return GapItem(
            entity_key=entity_key, kind=kind, version=version, input_sha256=input_sha256,
            reason=REASON_STALE_VERSION, stale_versions=stale,
        )
    return GapItem(entity_key=entity_key, kind=kind, version=version, input_sha256=input_sha256, reason=REASON_MISSING)


def _compute_gaps(conn, disc: _Discovery, staging_dir):
    """Returns (missing_complexity, missing_grades, missing_phase_labels), each
    a list of GapItem, `staged_path` filled in where a valid staged file
    already satisfies the exact cache key."""
    complexity_gaps: List[GapItem] = []
    grading_gaps: List[GapItem] = []
    phase_gaps: List[GapItem] = []

    for (change_name, task_key), brief_path in disc.briefs.items():
        task_id = _existing_task_id(conn, change_name, task_key)
        brief_text = _brief_text_for(disc.repo_root, disc.ref_sha, brief_path)
        ekey = _entity_key(change_name, task_key)

        gap = _gap_for(conn, "complexity", ekey, "task_id", task_id, brief_text)
        if gap:
            complexity_gaps.append(gap)

        sessions = disc.joined.get((change_name, task_key), [])
        reviewer_sessions = sorted(
            (js for js in sessions if js.role == "reviewer"), key=lambda j: j.rec.started_at or ""
        )
        if reviewer_sessions:
            review_text = "\n\n---\n\n".join(_review_text(js.rec) for js in reviewer_sessions)
            gap = _gap_for(conn, "grading", ekey, "task_id", task_id, review_text)
            if gap:
                grading_gaps.append(gap)

        for js in sessions:
            if js.role != "implementer" or js.review_round != 0:
                continue
            timeline = extractors.render_timeline(js.rec)
            gap = _gap_for(conn, "phase_labeling", js.session_id, "session_id", js.session_id if _session_exists(conn, js.session_id) else None, timeline)
            if gap:
                gap._n_turns = js.rec.n_turns
                phase_gaps.append(gap)

    for gaps, kind in ((complexity_gaps, "complexity"), (grading_gaps, "grading"), (phase_gaps, "phase_labeling")):
        for g in gaps:
            n_turns = getattr(g, "_n_turns", None)
            path, _data, _exists = _check_staged(staging_dir, kind, g.entity_key, g.version, g.input_sha256, n_turns=n_turns)
            g.staged_path = str(path) if path else None

    return complexity_gaps, grading_gaps, phase_gaps


def _session_exists(conn, session_id: str) -> bool:
    row = conn.execute("SELECT 1 FROM sessions WHERE session_id = ?", (session_id,)).fetchone()
    return row is not None


# --------------------------------------------------------------------------
# plan_work
# --------------------------------------------------------------------------


def plan_work(
    conn,
    repo_root,
    *,
    archive_ref: str = "refs/heads/main",
    change: Optional[str] = None,
    staging_dir=None,
    codex_sessions_root=None,
    claude_projects_root=None,
) -> WorkPlan:
    """Pure, read-only survey of what a real run would do. Never writes to
    ``conn`` or the filesystem (a valid-but-invalid staged file is reported
    as a to-dispatch gap, not renamed -- that happens in ``run``)."""
    staging_dir = staging_dir if staging_dir is not None else DEFAULT_STAGING_DIR
    disc = _discover(
        conn, repo_root, archive_ref=archive_ref, change=change,
        codex_sessions_root=codex_sessions_root, claude_projects_root=claude_projects_root,
    )
    return _plan_from_disc(conn, disc, staging_dir)


def _plan_from_disc(conn, disc: _Discovery, staging_dir) -> WorkPlan:
    existing_change_names = {
        r[0] for r in conn.execute("SELECT name FROM changes").fetchall()
    }
    new_changes = [c.name for c in disc.changes if c.name not in existing_change_names]

    new_tasks: List[str] = []
    for (change_name, task_key) in disc.briefs:
        if _existing_task_id(conn, change_name, task_key) is None:
            new_tasks.append(f"{change_name}/{task_key}")

    existing_session_ids = {
        r[0] for r in conn.execute("SELECT session_id FROM sessions").fetchall()
    }
    # followup-7, fix-round-1 (finding 3): joined sessions must be filtered
    # to `disc.briefs`'s keys -- the SCOPED (change, task_key) set -- not
    # every key `disc.joined` (which spans the FULL landed universe,
    # always, for evidence purposes; see _discover) happens to have. run()
    # itself only ever writes sessions for keys in disc.briefs (its
    # per-task loop iterates sorted(disc.briefs.items())), so a scoped
    # dry-run's new_sessions must match that, not overstate it with joins
    # to out-of-scope changes that this run would never actually write.
    # Quarantined sessions are correctly UNSCOPED here -- run()'s
    # new_quarantined block writes every one of them regardless of
    # --change, since quarantine isn't a per-change concept.
    new_sessions = [
        js.session_id
        for key in disc.briefs
        for js in disc.joined.get(key, [])
        if js.session_id not in existing_session_ids
    ] + [
        js.session_id for js in disc.quarantined_sessions if js.session_id not in existing_session_ids
    ]

    complexity_gaps, grading_gaps, phase_gaps = _compute_gaps(conn, disc, staging_dir)

    return WorkPlan(
        ref_sha=disc.ref_sha,
        new_changes=sorted(new_changes),
        new_tasks=sorted(new_tasks),
        new_sessions=sorted(new_sessions),
        missing_complexity=complexity_gaps,
        missing_grades=grading_gaps,
        missing_phase_labels=phase_gaps,
        quarantined=disc.quarantined,
        unlanded=disc.unlanded,
    )


# --------------------------------------------------------------------------
# run
# --------------------------------------------------------------------------


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


_TABLE_TO_KIND = {table: kind for kind, table in KIND_TABLE.items()}


def _normalize_rescore(rescore) -> Set[str]:
    """``--rescore`` takes real table names (``complexity``/``grades``/
    ``phase_tokens``, per the CLI contract); translate to the internal
    ``kind`` vocabulary (``complexity``/``grading``/``phase_labeling``) that
    gap-processing compares against. Also accepts a bare ``kind`` string
    directly, so callers (and tests) can use either spelling."""
    if not rescore:
        return set()
    values = {rescore} if isinstance(rescore, str) else set(rescore)
    return {_TABLE_TO_KIND.get(v, v) for v in values}


def _upsert_change(conn, name: str):
    db.upsert(conn, "changes", {"name": name, "ingested_at": _now_iso()}, ["name"])


def _get_change_id(conn, name: str) -> int:
    return conn.execute("SELECT change_id FROM changes WHERE name = ?", (name,)).fetchone()[0]


def _upsert_task(conn, change_id: int, task_key: str, brief_path: str, brief_text: str) -> int:
    row = {
        "change_id": change_id,
        "task_key": task_key,
        "brief_path": str(brief_path),
        "brief_sha256": db.sha256_text(brief_text),
        "brief_bytes": len(brief_text.encode("utf-8")),
        "brief_text": brief_text,
    }
    db.upsert(conn, "tasks", row, ["change_id", "task_key"])
    return conn.execute(
        "SELECT task_id FROM tasks WHERE change_id = ? AND task_key = ?", (change_id, task_key)
    ).fetchone()[0]


def _upsert_session(conn, task_id: Optional[int], js: _JoinedSession):
    rec = js.rec
    row = {
        "session_id": js.session_id,
        "task_id": task_id,
        "provider": rec.provider,
        "harness_entry": rec.harness_entry,
        "role": js.role,
        "model": rec.model,
        "effort": rec.effort,
        "started_at": rec.started_at,
        "ended_at": rec.ended_at,
        "transcript_path": None,
        "input_tokens": rec.tokens.input_tokens,
        "cached_tokens": rec.tokens.cached_tokens,
        "output_tokens": rec.tokens.output_tokens,
        "reasoning_tokens": rec.tokens.reasoning_tokens,
        "peak_context": rec.peak_context,
        "n_compactions": rec.n_compactions,
        "n_turns": rec.n_turns,
        "n_tool_calls": rec.n_tool_calls,
        "review_round": js.review_round,
        "verdict_boundaries_json": json.dumps(getattr(rec, "verdict_boundaries", None) or []),
    }
    db.upsert(conn, "sessions", row, ["session_id"])


def _upsert_session_turns(conn, session_id: str, rec) -> int:
    """Populate ``session_turns`` for one session's already-extracted
    ``rec.turns`` (design.md D5 amendment, followup-4) -- mechanical,
    deterministic from the transcript, alongside its ``sessions`` row.
    Returns the number of turn rows upserted."""
    n = 0
    for turn in rec.turns:
        db.upsert(
            conn, "session_turns",
            {
                "session_id": session_id, "turn_idx": turn.index,
                "started_at": turn.started_at, "ended_at": turn.ended_at,
                "output_tokens": turn.output_tokens,
            },
            ["session_id", "turn_idx"],
        )
        n += 1
    return n


def _upsert_complexity(conn, task_id: int, gap: GapItem, data: dict, scored_by: str):
    row = {
        "task_id": task_id,
        "c1": data["C1"], "c2": data["C2"], "c3": data["C3"], "c4": data["C4"],
        "c5": data["C5"], "c6": data["C6"], "c7": data["C7"],
        "composite": data["composite"],
        "rationale_json": json.dumps(data.get("rationale", {}), sort_keys=True),
        "rubric_version": gap.version,
        "input_sha256": gap.input_sha256,
        "scored_by": scored_by,
    }
    db.upsert(conn, "complexity", row, ["task_id", "rubric_version"])


def _upsert_grade(conn, task_id: int, gap: GapItem, data: dict, review_text: str, scored_by: str):
    row = {
        "task_id": task_id,
        "g1": data["G1"], "g2": data["G2"], "g3": data["G3"], "g4": data["G4"], "g5": data["G5"],
        "n_critical": data["n_critical"], "n_important": data["n_important"], "n_minor": data["n_minor"],
        "rounds_to_accept": data["rounds_to_accept"],
        "verdict_sequence_json": json.dumps(data.get("verdict_sequence")),
        "final_grade": data["final_grade"],
        "evidence_json": json.dumps(data.get("evidence")),
        "excluded_reviews_json": json.dumps(data.get("excluded_reviews", [])),
        "review_text": review_text,
        "rubric_version": gap.version,
        "input_sha256": gap.input_sha256,
        "scored_by": scored_by,
    }
    db.upsert(conn, "grades", row, ["task_id", "rubric_version"])


def _upsert_phase_tokens(conn, session_id: str, rec, gap: GapItem, data: dict, scored_by: str):
    labels: Dict[str, str] = data["labels"]
    agg: Dict[str, Dict[str, int]] = {}
    for turn in rec.turns:
        phase = labels.get(str(turn.index), "other")
        bucket = agg.setdefault(phase, {"output_tokens": 0, "turns": 0})
        bucket["output_tokens"] += turn.output_tokens
        bucket["turns"] += 1
    for phase, totals in agg.items():
        row = {
            "session_id": session_id,
            "phase": phase,
            "output_tokens": totals["output_tokens"],
            "turns": totals["turns"],
            "taxonomy_version": gap.version,
            "input_sha256": gap.input_sha256,
            "scored_by": scored_by,
        }
        db.upsert(conn, "phase_tokens", row, ["session_id", "phase", "taxonomy_version"])


def _upsert_turn_phases(conn, session_id: str, rec, gap: GapItem, data: dict, scored_by: str):
    """Populate ``turn_phases`` from the same staged labeling JSON that just
    fed ``_upsert_phase_tokens`` -- design.md D5 amendment, followup-4.
    ``_validate_staged``'s completeness check (every turn 1..n_turns has a
    label key) guarantees one row per turn here for a freshly-dispatched or
    freshly-staged session, matching ``phase_tokens``'s aggregation exactly
    (unlike ``_upsert_phase_tokens`` above, which defaults an unlabeled turn
    to ``"other"`` rather than dropping it -- kept here too, for the same
    aggregation-equals-phase_tokens invariant to hold turn-for-turn)."""
    labels: Dict[str, str] = data["labels"]
    for turn in rec.turns:
        phase = labels.get(str(turn.index), "other")
        db.upsert(
            conn, "turn_phases",
            {
                "session_id": session_id, "turn_idx": turn.index, "phase": phase,
                "taxonomy_version": gap.version, "input_sha256": gap.input_sha256,
                "scored_by": scored_by,
            },
            ["session_id", "turn_idx", "taxonomy_version"],
        )


def _scored_by_for(kind: str) -> str:
    try:
        _text, _version, model_hint = assets.load_prompt(KIND_PROMPT_NAME[kind])
        return model_hint
    except (OSError, ValueError):
        return "unknown"


def _resolve_gap(conn, staging_dir, kind: str, gap: GapItem, agent_runner, no_agents: bool, rescore_set: Set[str], report: RunReport, item_builder, *, strict: bool = False) -> bool:
    """Ensure `gap` is satisfied (dispatching if needed), returning True if
    the gap no longer needs work this run (a row was upserted, OR -- grading
    only, followup-7 defect 2 -- the result was durably recorded as
    ungradeable). Mutates `report`'s dispatched/staged_used/stale/ungradeable/
    failed counters. Does not commit -- caller owns the transaction.

    ``strict`` controls what happens when ``agent_runner`` runs without
    raising but no valid staged file appears afterward (followup-7, defect
    2): by default this fails just THIS item (logged in
    ``report.failed``/``report.failed_items``, gap left open for a future
    run) and lets the caller's run continue; with ``strict=True`` it raises
    ``RuntimeError`` as before, aborting the run. A missing ``agent_runner``
    entirely (genuine misconfiguration, not a per-item outcome) always
    raises regardless of ``strict``."""
    if gap.reason == REASON_STALE_VERSION and kind not in rescore_set:
        report.stale[kind] = report.stale.get(kind, 0) + 1
        return False

    if no_agents:
        return False

    n_turns = getattr(gap, "_n_turns", None)
    path, data, exists = _check_staged(staging_dir, kind, gap.entity_key, gap.version, gap.input_sha256, n_turns=n_turns)

    if path is None and exists:
        # Invalid staged file for this exact cache key -- quarantine it and
        # fall through to dispatch.
        bad = Path(staging_dir) / kind / _staging_filename(gap.entity_key, gap.version, gap.input_sha256)
        bad.rename(bad.with_name(bad.name + ".err"))

    if path is None:
        if agent_runner is None:
            raise RuntimeError(f"agentic gap for {kind}/{gap.entity_key} but no agent_runner was provided")
        item = item_builder()
        agent_runner(kind, [item], Path(staging_dir))
        report.dispatched[kind] = report.dispatched.get(kind, 0) + 1
        path, data, exists = _check_staged(staging_dir, kind, gap.entity_key, gap.version, gap.input_sha256, n_turns=n_turns)
        if path is None:
            # followup-7, defect 2: agent_runner ran without raising, but no
            # valid staged file resulted. agents.py's own retry logic
            # already distinguishes "the model declined" (silently dropped,
            # exit 0) from "the subprocess itself crashed" (raises there);
            # by the time control reaches here, neither of those raised, so
            # this is either an agent that ignored its own output contract
            # (prompts/*.md now require a file for every item, always) or a
            # softer infra hiccup. Fail just this item, not the whole run,
            # unless the caller opted into --strict.
            msg = f"agent_runner did not produce a valid staged file for {kind}/{gap.entity_key}"
            report.failed[kind] = report.failed.get(kind, 0) + 1
            report.failed_items.append({"kind": kind, "entity_key": gap.entity_key, "error": msg})
            if strict:
                raise RuntimeError(msg)
            return False
    else:
        report.staged_used[kind] = report.staged_used.get(kind, 0) + 1

    if kind == "grading":
        # followup-7, defect 2 (and fix-round-1, finding 2): the shape --
        # not the mere presence of a "reason" key -- discriminates a
        # correctly-declined grading result (every G field null, see
        # prompts/grading.md's Ungradeable shape) from a real grade that
        # merely also carries a harmless explanatory `reason`.
        # _validate_staged already rejected anything else (a half-null
        # hybrid) before this data could ever reach here, so this
        # reclassification is just reading back that same decision.
        shape = _grading_shape(data) if isinstance(data, dict) else None
        if shape == "ungradeable":
            # Record this durably WITHOUT a grades row -- the staged JSON
            # file itself is the durable record, already written under the
            # deterministic staging path keyed by (entity_key, version,
            # input_sha256), so it's found again (and this same branch
            # re-taken, idempotently) on any future run over unchanged
            # input -- and let the run continue.
            #
            # fix-round-1, finding 1: an existing `grades` row for this
            # exact (task_id, rubric_version) may already exist and be
            # STALE -- e.g. the gap exists because REASON_INPUT_CHANGED
            # (the joined review text changed since that row was scored),
            # and the new input's own result is "nothing gradeable." That
            # old row's input_sha256 no longer matches any real input, so
            # leaving it in place would have downstream readers silently
            # keep consuming a grade for text that's no longer what's
            # actually joined to this task. Delete/supersede it -- deletion
            # or its cache key: rubric_version does not track
            # input_sha256, and the old row can never become "current"
            # again just from the ungradeable result. The DELETE is a
            # no-op (0 rows) in the far more common REASON_MISSING case,
            # where no prior row exists at all.
            change_name, task_key = gap.entity_key.split("--", 1)
            task_id = _existing_task_id(conn, change_name, task_key)
            superseded = 0
            if task_id is not None:
                cur = conn.execute(
                    "DELETE FROM grades WHERE task_id = ? AND rubric_version = ?",
                    (task_id, gap.version),
                )
                superseded = cur.rowcount if cur.rowcount and cur.rowcount > 0 else 0
            report.ungradeable[kind] = report.ungradeable.get(kind, 0) + 1
            entry = {"kind": kind, "entity_key": gap.entity_key, "reason": str(data.get("reason"))}
            if superseded:
                entry["superseded_grades_row"] = True
                report.superseded_grades += superseded
            report.ungradeable_items.append(entry)
            return True

    scored_by = _scored_by_for(kind)
    if kind == "complexity":
        # entity_key is "<change>--<task_key>"; resolve via change name join.
        change_name, task_key = gap.entity_key.split("--", 1)
        task_id = _existing_task_id(conn, change_name, task_key)
        _upsert_complexity(conn, task_id, gap, data, scored_by)
    elif kind == "grading":
        change_name, task_key = gap.entity_key.split("--", 1)
        task_id = _existing_task_id(conn, change_name, task_key)
        _upsert_grade(conn, task_id, gap, data, item_builder.review_text if hasattr(item_builder, "review_text") else "", scored_by)
    else:
        rec = item_builder.rec
        _upsert_phase_tokens(conn, gap.entity_key, rec, gap, data, scored_by)
        _upsert_turn_phases(conn, gap.entity_key, rec, gap, data, scored_by)

    report.rows_written[kind] = report.rows_written.get(kind, 0) + 1
    return True


def run(
    conn,
    repo_root,
    *,
    dry_run: bool = False,
    no_agents: bool = False,
    rescore=None,
    agent_runner=None,
    archive_ref: str = "refs/heads/main",
    change: Optional[str] = None,
    staging_dir=None,
    codex_sessions_root=None,
    claude_projects_root=None,
    strict: bool = False,
) -> RunReport:
    staging_dir = Path(staging_dir) if staging_dir is not None else DEFAULT_STAGING_DIR
    rescore_set = _normalize_rescore(rescore)

    disc = _discover(
        conn, repo_root, archive_ref=archive_ref, change=change,
        codex_sessions_root=codex_sessions_root, claude_projects_root=claude_projects_root,
    )
    plan = _plan_from_disc(conn, disc, staging_dir)

    report = RunReport(
        ref_sha=disc.ref_sha, dry_run=dry_run, plan=plan,
        quarantined=disc.quarantined, unlanded=disc.unlanded,
    )

    if dry_run:
        return report

    for staging_kind_dir in KIND_TABLE:
        (staging_dir / staging_kind_dir).mkdir(parents=True, exist_ok=True)

    for change_name in plan.new_changes:
        _upsert_change(conn, change_name)
        conn.commit()
        report.changes_written += 1

    gaps_by_task: Dict[Tuple[str, str], Dict[str, GapItem]] = {}
    for gap in plan.missing_complexity:
        gaps_by_task.setdefault(tuple(gap.entity_key.split("--", 1)), {})["complexity"] = gap
    for gap in plan.missing_grades:
        gaps_by_task.setdefault(tuple(gap.entity_key.split("--", 1)), {})["grading"] = gap

    # phase_labeling gaps are recomputed per-session below (they don't key
    # by task), rather than reused from `plan` -- reusing them would race
    # against sessions written earlier in this same loop.

    for (change_name, task_key), brief_path in sorted(disc.briefs.items()):
        try:
            change_id = _get_change_id(conn, change_name)
            existing_task_id = _existing_task_id(conn, change_name, task_key)
            # Brief content always comes from the landed ref (D3), never the
            # working tree -- needed both for the task row and, below, for
            # a complexity dispatch, so fetch it once per task per run.
            # (A real committed brief or a plan-derived section -- see
            # _brief_text_for/the module docstring's followup-6 note.)
            brief_text = _brief_text_for(disc.repo_root, disc.ref_sha, brief_path)
            if existing_task_id is None:
                task_id = _upsert_task(conn, change_id, task_key, brief_path, brief_text)
                report.tasks_written += 1
            else:
                task_id = existing_task_id

            sessions = disc.joined.get((change_name, task_key), [])
            for js in sessions:
                if not _session_exists(conn, js.session_id):
                    _upsert_session(conn, task_id, js)
                    _upsert_session_turns(conn, js.session_id, js.rec)
                    report.sessions_written += 1

            task_gaps = gaps_by_task.get((change_name, task_key), {})

            if "complexity" in task_gaps:
                gap = task_gaps["complexity"]

                class _Item:
                    def __call__(self):
                        return {
                            "task_key": gap.entity_key, "version": gap.version,
                            "input_sha256": gap.input_sha256, "brief_text": brief_text,
                        }

                _resolve_gap(conn, staging_dir, "complexity", gap, agent_runner, no_agents, rescore_set, report, _Item(), strict=strict)

            if "grading" in task_gaps:
                gap = task_gaps["grading"]
                reviewer_sessions = sorted(
                    (js for js in sessions if js.role == "reviewer"), key=lambda j: j.rec.started_at or ""
                )
                review_text = "\n\n---\n\n".join(_review_text(js.rec) for js in reviewer_sessions)

                class _GradeItem:
                    review_text = None

                    def __call__(self):
                        return {
                            "task_key": gap.entity_key, "version": gap.version,
                            "input_sha256": gap.input_sha256, "review_text": review_text,
                        }

                gi = _GradeItem()
                gi.review_text = review_text
                _resolve_gap(conn, staging_dir, "grading", gap, agent_runner, no_agents, rescore_set, report, gi, strict=strict)

            for js in sessions:
                if js.role != "implementer" or js.review_round != 0:
                    continue
                session_id = js.session_id
                timeline = extractors.render_timeline(js.rec)
                version = assets.read_asset_version(KIND_RUBRIC_PATH["phase_labeling"])
                input_sha256 = db.sha256_text(timeline)
                current_hash, stale = _current_and_stale(
                    conn, "phase_tokens", "taxonomy_version", "session_id", session_id, version
                )
                if current_hash == input_sha256:
                    continue
                if current_hash is not None:
                    reason = REASON_INPUT_CHANGED
                elif stale:
                    reason = REASON_STALE_VERSION
                else:
                    reason = REASON_MISSING
                gap = GapItem(
                    entity_key=session_id, kind="phase_labeling", version=version,
                    input_sha256=input_sha256, reason=reason, stale_versions=stale,
                )
                gap._n_turns = js.rec.n_turns

                class _PhaseItem:
                    rec = js.rec

                    def __call__(self):
                        return {
                            "session_key": session_id, "version": version,
                            "input_sha256": input_sha256, "timeline": timeline,
                        }

                pi = _PhaseItem()
                _resolve_gap(conn, staging_dir, "phase_labeling", gap, agent_runner, no_agents, rescore_set, report, pi, strict=strict)

            conn.commit()
        except Exception:
            conn.rollback()
            raise

    new_quarantined = [js for js in disc.quarantined_sessions if not _session_exists(conn, js.session_id)]
    if new_quarantined:
        try:
            for js in new_quarantined:
                _upsert_session(conn, None, js)
                _upsert_session_turns(conn, js.session_id, js.rec)
                report.sessions_written += 1
            conn.commit()
        except Exception:
            conn.rollback()
            raise

    # Healing (followup-7, defect 1 amendment): reconcile any ALREADY
    # committed session whose stored task_id disagrees with today's
    # evidence-based join/quarantine outcome -- e.g. residue from a run
    # that predates the evidence-gated join fix (a session the old
    # scope-narrowing bug joined to a task it had no real evidence for).
    # Computed over the full disc.joined/disc.quarantined_sessions universe
    # -- never narrowed by --change, same principle as the join logic
    # itself -- and run AFTER the per-task loop and new-quarantined writes
    # above so any task/session this run just created is already visible.
    # A session with no row yet is untouched here (the paths above handle
    # it); a session whose correct target task doesn't exist in the DB at
    # all (that change/task has never been ingested) heals to NULL
    # (quarantined) rather than being left pointing at a demonstrably wrong
    # task_id -- a later run that ingests that change/task heals it again,
    # this time to the real task_id. This is what makes a re-run after this
    # fix converge/idempotently heal any mis-joined session.task_id rows
    # left by the canary, rather than freezing them in place forever (the
    # insert-only guards elsewhere in run() only ever add brand-new
    # sessions; they never revisit ones already present).
    expected_target: Dict[str, Optional[Tuple[str, str]]] = {}
    for (jn_change, jn_task), sessions in disc.joined.items():
        for js in sessions:
            expected_target[js.session_id] = (jn_change, jn_task)
    for js in disc.quarantined_sessions:
        expected_target.setdefault(js.session_id, None)

    healed_ids: List[str] = []
    for session_id, target in expected_target.items():
        row = conn.execute("SELECT task_id FROM sessions WHERE session_id = ?", (session_id,)).fetchone()
        if row is None:
            continue  # not yet committed -- handled by the insert paths above
        current_task_id = row[0]
        correct_task_id = _existing_task_id(conn, target[0], target[1]) if target is not None else None
        if current_task_id != correct_task_id:
            conn.execute("UPDATE sessions SET task_id = ? WHERE session_id = ?", (correct_task_id, session_id))
            healed_ids.append(session_id)
    if healed_ids:
        conn.commit()
        report.healed_sessions = len(healed_ids)
        report.healed_session_ids = sorted(healed_ids)

    # Only append an ingest_log row when the run actually did something --
    # a true no-op re-run (zero writes, zero dispatches) is reported via the
    # returned RunReport/stdout only, not persisted to the audit trail, so
    # repeated no-op runs don't bloat ingest_log. (dump_jsonl also excludes
    # ingest_log entirely, belt-and-suspenders for dump byte-stability.)
    if report.total_writes > 0 or report.healed_sessions > 0 or report.ungradeable_items or report.failed_items:
        started_at = _now_iso()
        finished_at = started_at
        actions = {
            "archive_ref": archive_ref,
            "rows_written": report.rows_written,
            "dispatched": report.dispatched,
            "staged_used": report.staged_used,
            "stale": report.stale,
            "quarantined": [q.session_id for q in report.quarantined],
            "unlanded": report.unlanded,
            "new_changes": plan.new_changes,
            "new_tasks": plan.new_tasks,
            "ungradeable": report.ungradeable,
            "ungradeable_items": report.ungradeable_items,
            "failed": report.failed,
            "failed_items": report.failed_items,
            "healed_sessions": report.healed_sessions,
            "healed_session_ids": report.healed_session_ids,
            "superseded_grades": report.superseded_grades,
        }
        conn.execute(
            "INSERT INTO ingest_log(started_at, finished_at, git_sha, actions_json) VALUES (?, ?, ?, ?)",
            (started_at, finished_at, report.ref_sha, json.dumps(actions, sort_keys=True)),
        )
        conn.commit()

    dump_path = _dump_path_for(conn)
    if dump_path is not None:
        db.dump_jsonl(conn, dump_path)

    return report


def _dump_path_for(conn) -> Optional[Path]:
    for _seq, name, file in conn.execute("PRAGMA database_list").fetchall():
        if name == "main" and file:
            p = Path(file)
            return p.parent / (p.stem + ".dump.jsonl")
    return None


# --------------------------------------------------------------------------
# backfill-turns (design.md D5 amendment, followup-4)
# --------------------------------------------------------------------------


def _analysis_phase_labels_key(session_id: str) -> Optional[str]:
    """``"{provider}:{native_id}"`` -> the analysis dataset's
    ``phase_labels``/``timelines`` file stem, ``"{provider}-{native_id}"``
    (the inverse of ``migrate_v0.py``'s ``_session_key_to_sid``)."""
    provider, sep, native_id = session_id.partition(":")
    if not sep or not native_id:
        return None
    return f"{provider}-{native_id}"


def _find_staged_phase_labels(session_id: str, version: str, input_sha256: str, staging_dir=None) -> Optional[dict]:
    """A live-ingested session's own ``staging/phase_labeling/`` JSON, if it
    still exists at the exact cache key its persisted ``phase_tokens`` row
    was built from. (None of the currently-committed dataset's sessions
    have one -- they all predate this module's live staging path, see
    ``DEFAULT_ANALYSIS_DIR``'s docstring -- but a future backfill run
    against live-ingested sessions whose staged file survived should prefer
    this over the analysis-dataset fallback below.)"""
    staging_dir = Path(staging_dir) if staging_dir is not None else DEFAULT_STAGING_DIR
    path = staging_dir / "phase_labeling" / _staging_filename(session_id, version, input_sha256)
    if not path.exists():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None
    labels = data.get("labels")
    return labels if isinstance(labels, dict) and labels else None


def _find_turn_labels(conn, session_id: str, analysis_dir: Path):
    """``(labels, taxonomy_version, input_sha256, scored_by)`` for
    backfilling ``turn_phases``, reusing the cache-key metadata of this
    session's own (already-persisted) ``phase_tokens`` rows at their
    numerically-greatest ``taxonomy_version`` -- so a backfilled
    ``turn_phases`` row is directly comparable/joinable with the
    ``phase_tokens`` row it must aggregate up to. Returns
    ``(None, None, None, None)`` if this session has no ``phase_tokens`` at
    all (never phase-labeled -- nothing to backfill turn-level detail for)
    or no per-turn label source (staged JSON, else the analysis dataset's
    ``phase_labels/<key>.json``) survives for it."""
    existing = conn.execute(
        "SELECT DISTINCT taxonomy_version, input_sha256, scored_by FROM phase_tokens WHERE session_id = ?",
        (session_id,),
    ).fetchall()
    if not existing:
        return None, None, None, None
    row = max(existing, key=lambda r: _version_int(r["taxonomy_version"]))
    version, input_sha256, scored_by = row["taxonomy_version"], row["input_sha256"], row["scored_by"]

    staged = _find_staged_phase_labels(session_id, version, input_sha256)
    if staged is not None:
        return staged, version, input_sha256, scored_by

    key = _analysis_phase_labels_key(session_id)
    if key:
        path = Path(analysis_dir) / "phase_labels" / f"{key}.json"
        if path.exists():
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
                labels = data.get("labels")
                if isinstance(labels, dict) and labels:
                    return labels, version, input_sha256, scored_by
            except (json.JSONDecodeError, OSError):
                pass

    return None, None, None, None


def _recompute_phase_tokens_from_turn_phases(conn, session_id: str) -> int:
    """For a session with EXISTING ``turn_phases`` rows, recompute
    ``phase_tokens.output_tokens``/``.turns`` mechanically from those rows'
    (unchanged) phase LABELS joined against the session's CURRENT
    ``session_turns.output_tokens`` -- design.md D5/D2 amendment,
    followup-5. Phase labels are the agentic judgment (cache-keyed,
    untouched here); per-turn token weights are mechanical, so when
    ``session_turns`` is regenerated under a corrected extractor (its
    deltas change), the ``phase_tokens`` row those labels aggregate up to
    must be updated to match -- never re-derived from an external label
    source (that's ``_verify_and_backfill_turn_phases``'s job, for
    sessions that don't have ``turn_phases`` yet at all). Only
    ``output_tokens``/``turns`` are updated in place; ``input_sha256``/
    ``scored_by`` (the cache-key columns) are left untouched, since the
    underlying agentic judgment hasn't changed.

    MUST run before ``_verify_and_backfill_turn_phases`` for the same
    session in the same ``backfill_turns`` call: that function's
    all-or-nothing check compares a fresh candidate (labels x current
    session_turns) against whatever's currently stored in
    ``phase_tokens`` -- if this session's ``session_turns`` were just
    regenerated with different deltas, an un-recomputed (stale)
    ``phase_tokens`` row would spuriously fail that comparison and
    wrongly self-heal-delete a session's already-correct labels.

    Returns the number of (phase, taxonomy_version) rows updated."""
    versions = [
        r[0] for r in conn.execute(
            "SELECT DISTINCT taxonomy_version FROM turn_phases WHERE session_id = ?", (session_id,)
        ).fetchall()
    ]
    updated = 0
    for version in versions:
        rows = conn.execute(
            "SELECT tp.phase, st.output_tokens FROM turn_phases tp "
            "JOIN session_turns st ON st.session_id = tp.session_id AND st.turn_idx = tp.turn_idx "
            "WHERE tp.session_id = ? AND tp.taxonomy_version = ?",
            (session_id, version),
        ).fetchall()
        agg: Dict[str, Dict[str, int]] = {}
        for r in rows:
            bucket = agg.setdefault(r["phase"], {"output_tokens": 0, "turns": 0})
            bucket["output_tokens"] += r["output_tokens"] or 0
            bucket["turns"] += 1
        for phase, totals in agg.items():
            conn.execute(
                "UPDATE phase_tokens SET output_tokens = ?, turns = ? "
                "WHERE session_id = ? AND phase = ? AND taxonomy_version = ?",
                (totals["output_tokens"], totals["turns"], session_id, phase, version),
            )
            updated += 1
    return updated


def _verify_and_backfill_turn_phases(conn, session_id: str, analysis_dir: Path, counts: Dict[str, int]) -> None:
    """(Re-)compute ``turn_phases`` for one round-0 implementer session
    from its already-committed ``session_turns`` rows, verifying the
    aggregation-equals-``phase_tokens`` invariant BEFORE writing anything
    (fix-round-1 review, finding 1: the original backfill wrote whatever
    partial set of rows a turn-index-keyed label lookup happened to
    produce, with no check that it actually summed back to the stored
    ``phase_tokens`` -- on the real dataset this silently produced 9
    row-level mismatches across 3 sessions, plus 39 sessions counted as
    "backfilled" despite writing zero rows at all, because every label
    lookup missed).

    All-or-nothing per (session, taxonomy_version): the full candidate
    aggregation is computed and compared against ``phase_tokens`` first;
    only if it matches EXACTLY are any ``turn_phases`` rows written. On a
    mismatch, no rows are written and any PRE-EXISTING (possibly wrong,
    left by an earlier buggy backfill run) ``turn_phases`` rows for this
    session/version are deleted first -- self-healing, and idempotent
    (both first-run-clean and every re-run converge to the same state: a
    session whose per-turn labels can never realign with its re-extracted
    turns stays at zero ``turn_phases`` rows forever, correctly falling
    back to ``costs.py``'s session-level-scaled apportionment). Reads
    ``session_turns`` from the database rather than re-parsing the
    transcript again -- the caller has usually just written it in this
    same transaction, and it's the authoritative per-turn output-token
    source either way.

    Mutates ``counts``: ``turn_phases_backfilled`` (verified and written),
    ``turn_phases_unavailable`` (no per-turn label source survives at
    all -- existing turn_phases, if any, are left untouched, since this
    session was never a candidate for backfill in the first place),
    ``turn_phases_verification_failed`` (a label source was found, but its
    implied aggregation doesn't match the stored ``phase_tokens`` -- the
    do-not-write / self-heal-by-deletion path)."""
    turns = conn.execute(
        "SELECT turn_idx, output_tokens FROM session_turns WHERE session_id = ? ORDER BY turn_idx",
        (session_id,),
    ).fetchall()
    if not turns:
        return

    labels, version, input_sha256, scored_by = _find_turn_labels(conn, session_id, analysis_dir)
    if labels is None:
        counts["turn_phases_unavailable"] += 1
        return

    # Candidate aggregation implied by writing one turn_phases row per
    # session_turns entry with the same "other"-default convention
    # ``_upsert_turn_phases`` uses at live-ingest time -- computed BEFORE
    # anything is written, so a mismatch costs zero writes.
    candidate: Dict[str, int] = {}
    for t in turns:
        phase = labels.get(str(t["turn_idx"]), "other")
        candidate[phase] = candidate.get(phase, 0) + (t["output_tokens"] or 0)
    stored = costs.phase_tokens_for_session(conn, session_id, version)

    # Clear any existing rows for this (session, version) now -- whether
    # they're about to be replaced by a freshly-verified set, or (on a
    # verification failure below) simply removed as self-heal of a prior
    # buggy write. Scoped to this exact version, never other versions.
    conn.execute(
        "DELETE FROM turn_phases WHERE session_id = ? AND taxonomy_version = ?",
        (session_id, version),
    )

    if candidate != stored:
        counts["turn_phases_verification_failed"] += 1
        return

    for t in turns:
        phase = labels.get(str(t["turn_idx"]), "other")
        db.upsert(
            conn, "turn_phases",
            {
                "session_id": session_id, "turn_idx": t["turn_idx"], "phase": phase,
                "taxonomy_version": version, "input_sha256": input_sha256, "scored_by": scored_by,
            },
            ["session_id", "turn_idx", "taxonomy_version"],
        )
    counts["turn_phases_backfilled"] += 1


def backfill_turns(conn, *, analysis_dir=None, regenerate: bool = False) -> Dict[str, int]:
    """For every already-ingested session lacking ``session_turns`` rows,
    re-extract turns from ``sessions.transcript_path`` if that file still
    exists on disk (design.md D5 amendment, followup-4); a session with no
    recoverable transcript simply keeps session-level cost attribution
    unchanged (documented limitation, not an error).

    ``regenerate=True`` (design.md D5/D2 amendment, followup-5) widens the
    candidate set from "sessions lacking session_turns" to EVERY session
    with a ``transcript_path``, REPLACING existing ``session_turns`` rows
    (idempotent: old rows for that session are deleted before the fresh
    ones are written) rather than skipping sessions that already have
    them. Use this the first time ``backfill-turns`` runs after an
    ``extractors.py`` change that alters per-turn token deltas (like
    followup-5's silent-checkpoint reconciliation fix) -- the default
    ``regenerate=False`` behavior would otherwise never re-derive
    ``session_turns`` for a session it already backfilled under the OLD,
    leakier extractor logic. A session that already has ``session_turns``
    but whose transcript is no longer recoverable is left untouched (its
    existing rows, however out of date, are the best available) and
    counted under ``session_turns_regenerate_skipped``, not
    ``session_turns_unrecoverable`` (reserved for a session with NO
    ``session_turns`` at all and no way to get any).

    Refreshes ``sessions.n_turns`` from the freshly re-extracted
    ``len(rec.turns)`` for every session this touches (design.md D5/D2
    amendment, followup-5: many rows were stale from the original
    migration and never corrected since).

    Then: for every session that already has ``turn_phases`` rows (from
    live ingestion, an earlier backfill, or just-regenerated above),
    recomputes ``phase_tokens.output_tokens``/``.turns`` mechanically
    from those rows' existing phase labels joined against the (possibly
    just-changed) ``session_turns`` (``_recompute_phase_tokens_from_turn_phases``
    -- design.md D5/D2 amendment, followup-5: labels are the untouched
    agentic judgment, only the mechanical token weight they aggregate
    needs to track deltas that changed).

    Finally, for EVERY round-0 implementer session that has
    ``session_turns`` (whether just backfilled, backfilled by a prior
    run, or live-ingested), (re-)verifies and (re-)backfills
    ``turn_phases`` from a still-valid staged ``phase_labeling`` JSON if
    one exists, else the analysis dataset's per-turn
    ``phase_labels/<key>.json`` (see ``_find_turn_labels`` and
    ``_verify_and_backfill_turn_phases``) -- verified all-or-nothing
    against the session's own (by now up to date) ``phase_tokens`` before
    anything is written, which also self-heals any wrong rows a prior
    (fix-round-1-and-earlier) buggy backfill left behind. A session with
    no recoverable per-turn label source, or whose implied aggregation
    doesn't match ``phase_tokens``, is left (or reset to) empty
    ``turn_phases`` -- costs.py's documented fallback then scales the
    session-level ``phase_tokens`` ratios instead of computing shares
    directly per turn.

    Note a real recoverability limit this uncovered: the analysis dataset's
    rendered timelines (``timelines/<key>.md``) carry per-turn OUTPUT
    TOKENS only, never per-turn TIMESTAMPS (``extractors.render_timeline``'s
    header format never included them) -- so a migrated_v0 session whose
    raw transcript file has since been deleted cannot have ``session_turns``
    backfilled from the analysis dataset at all, even though its
    ``phase_tokens``/labels survive; only a still-present transcript file
    can supply the timestamps ``costs.py``'s turn split needs. This is why
    the analysis dataset is a ``turn_phases``-only fallback here, never a
    ``session_turns`` source.

    Returns a dict of counts: ``session_turns_backfilled`` (a session with
    NO prior session_turns got its first rows), ``session_turns_regenerated``
    (``regenerate=True`` only -- an existing session's rows were replaced),
    ``session_turns_zero_turns`` (transcript parsed cleanly but yielded
    zero turns -- reported separately from a genuine backfill, fix-round-1
    review finding 4), ``session_turns_unrecoverable`` (no session_turns at
    all, transcript missing or unparseable), ``session_turns_regenerate_skipped``
    (``regenerate=True`` only -- already had session_turns, but the
    transcript is no longer recoverable to refresh them from),
    ``phase_tokens_recomputed`` (sessions whose phase_tokens weights were
    mechanically recomputed from their existing turn_phases labels),
    ``turn_phases_backfilled``, ``turn_phases_unavailable`` (no per-turn
    label source at all), and ``turn_phases_verification_failed`` (a label
    source was found but its aggregation didn't match ``phase_tokens`` --
    nothing was written, and any pre-existing wrong rows for that session
    were removed)."""
    analysis_dir = Path(analysis_dir) if analysis_dir is not None else DEFAULT_ANALYSIS_DIR

    counts = {
        "session_turns_backfilled": 0,
        "session_turns_regenerated": 0,
        "session_turns_zero_turns": 0,
        "session_turns_unrecoverable": 0,
        "session_turns_regenerate_skipped": 0,
        "phase_tokens_recomputed": 0,
        "turn_phases_backfilled": 0,
        "turn_phases_unavailable": 0,
        "turn_phases_verification_failed": 0,
    }

    try:
        # Pass 1: sessions lacking session_turns (default), or every
        # session with a transcript_path at all (regenerate=True) -- see
        # this function's docstring.
        if regenerate:
            rows = conn.execute(
                "SELECT session_id, provider, transcript_path FROM sessions "
                "WHERE transcript_path IS NOT NULL"
            ).fetchall()
        else:
            rows = conn.execute(
                "SELECT session_id, provider, transcript_path FROM sessions "
                "WHERE session_id NOT IN (SELECT DISTINCT session_id FROM session_turns)"
            ).fetchall()

        for row in rows:
            session_id = row["session_id"]
            transcript_path = row["transcript_path"]
            had_turns_already = (
                conn.execute(
                    "SELECT 1 FROM session_turns WHERE session_id = ? LIMIT 1", (session_id,)
                ).fetchone()
                is not None
            )
            if not transcript_path or not Path(transcript_path).exists():
                if had_turns_already:
                    counts["session_turns_regenerate_skipped"] += 1
                else:
                    counts["session_turns_unrecoverable"] += 1
                continue
            try:
                rec = _extract(row["provider"], Path(transcript_path))
            except Exception:
                if had_turns_already:
                    counts["session_turns_regenerate_skipped"] += 1
                else:
                    counts["session_turns_unrecoverable"] += 1
                continue

            rec.verdict_boundaries = [
                t.ended_at for t in rec.turns if getattr(t, "is_verdict", False) and t.ended_at
            ]
            if had_turns_already and regenerate:
                # Idempotent replace: clear this session's existing rows
                # before writing the freshly re-extracted ones, rather
                # than relying on db.upsert's per-turn_idx overwrite (safe
                # even if the turn count ever differs from before).
                conn.execute("DELETE FROM session_turns WHERE session_id = ?", (session_id,))
            n_turns_written = _upsert_session_turns(conn, session_id, rec)
            # A plain UPDATE, not db.upsert: db.upsert's ON CONFLICT DO
            # UPDATE still builds a full INSERT row first (defaulting every
            # column not in `row` to NULL), which would violate sessions'
            # NOT NULL columns (provider, role) on this already-existing
            # row -- this call only ever touches an existing session, never
            # inserts a new one, so a targeted UPDATE is both correct and
            # simpler. Also refreshes n_turns from the authoritative
            # re-extraction (followup-5: many rows were stale from the
            # original migration) -- unconditionally, BEFORE the zero-turn
            # early-return below, so a session whose transcript re-parses
            # to zero turns also gets n_turns corrected to 0 rather than
            # left at a stale prior value (fix-round-1 review of
            # followup-5, High finding: the original code `continue`d on
            # n_turns_written == 0 before ever reaching this UPDATE).
            conn.execute(
                "UPDATE sessions SET verdict_boundaries_json = ?, n_turns = ? WHERE session_id = ?",
                (json.dumps(rec.verdict_boundaries), len(rec.turns), session_id),
            )
            if n_turns_written == 0:
                # Parsed cleanly but the transcript yielded no turns at all
                # (e.g. an aborted session with no assistant response) --
                # not a genuine backfill; counted separately so
                # session_turns_backfilled means what it says (fix-round-1
                # review, finding 4).
                counts["session_turns_zero_turns"] += 1
                continue
            if had_turns_already:
                counts["session_turns_regenerated"] += 1
            else:
                counts["session_turns_backfilled"] += 1

        # Pass 1.5: for every session with EXISTING turn_phases rows,
        # recompute phase_tokens mechanically from those (untouched)
        # labels joined against the (possibly just-regenerated)
        # session_turns -- MUST run before pass 2 below, whose
        # verify-against-phase_tokens check would otherwise compare
        # against stale weights. See
        # _recompute_phase_tokens_from_turn_phases's docstring.
        turn_phases_session_ids = [
            r[0] for r in conn.execute("SELECT DISTINCT session_id FROM turn_phases").fetchall()
        ]
        for session_id in turn_phases_session_ids:
            if _recompute_phase_tokens_from_turn_phases(conn, session_id) > 0:
                counts["phase_tokens_recomputed"] += 1

        # Pass 2: every round-0 implementer session with session_turns (from
        # pass 1 just now, an earlier backfill run, or live ingestion) gets
        # its turn_phases (re-)verified -- see _verify_and_backfill_turn_phases
        # for why this is unconditional (self-healing) rather than only for
        # sessions newly touched above.
        round0_session_ids = [
            r[0] for r in conn.execute(
                "SELECT session_id FROM sessions WHERE role = 'implementer' "
                "AND (review_round IS NULL OR review_round = 0) "
                "AND session_id IN (SELECT DISTINCT session_id FROM session_turns)"
            ).fetchall()
        ]
        for session_id in round0_session_ids:
            _verify_and_backfill_turn_phases(conn, session_id, analysis_dir, counts)

        conn.commit()
    except Exception:
        conn.rollback()
        raise

    return counts


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def _build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Idempotent ingest driver for the task-analyzer pipeline.")
    # Positional, optional, defaulting to the pre-existing (unnamed) ingest
    # behavior -- a bare `ingest.py [flags]` invocation is unchanged.
    # `rebuild-derived` (design.md D5/D9, costs.rebuild) recomputes
    # task_costs/task_arms from already-ingested raw+agentic rows; it takes
    # no flags of its own beyond --db. `backfill-turns` (design.md D5
    # amendment, followup-4) populates session_turns/turn_phases for
    # already-ingested sessions that predate those tables; takes --db and
    # --analysis-dir only. `verify-turn-phases` (fix-round-1 review, finding
    # 1) is a read-only audit of the turn_phases/phase_tokens aggregation
    # invariant against whatever's already committed -- takes --db only;
    # exits 1 if any violation is found, so it's usable as a CI/pre-commit
    # gate, not just an interactive report.
    p.add_argument(
        "command", nargs="?", default="ingest",
        choices=["ingest", "rebuild-derived", "backfill-turns", "verify-turn-phases"],
    )
    p.add_argument("--db", default=str(Path("data/agents/task-analyzer.sqlite")))
    p.add_argument("--repo", default=".")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--no-agents", action="store_true")
    p.add_argument("--rescore", default=None, help="table name to opt in to rescoring (complexity|grades|phase_tokens)")
    p.add_argument("--change", default=None)
    p.add_argument("--analysis-dir", default=None,
                    help="backfill-turns only: analysis dataset dir for the turn_phases fallback "
                         "(default: analysis/sdd-model-analysis/data)")
    p.add_argument("--regenerate", action="store_true",
                    help="backfill-turns only: REPLACE session_turns for every session with a "
                         "recoverable transcript, not just ones lacking session_turns -- use once "
                         "after an extractors.py change that alters per-turn token deltas "
                         "(followup-5)")
    p.add_argument("--strict", action="store_true",
                    help="ingest only (followup-7, defect 2): abort the whole run if any single "
                         "agentic gap's agent_runner produces no valid staged file (the pre-fix "
                         "behavior). Default off -- such an item now fails on its own (recorded "
                         "in the run report's failed/failed_items) and the run continues with "
                         "every other task. Has no effect on a correctly-declined 'ungradeable' "
                         "grading result (see prompts/grading.md), which is never an error.")
    return p


# followup-6, defect 1: a --dry-run's new_sessions list can legitimately be
# large (a fresh DB's first run over a big historical corpus) -- print the
# full list up to this many entries, else fall back to a count + a short
# sample, rather than either dumping thousands of session ids or (the
# pre-fix bug) silently showing nothing at all. changes/tasks/gaps are
# always printed in full: those are the actionable, typically-short lists
# a human reviewing a dry run actually needs to see completely.
_DRY_RUN_SESSION_LIST_LIMIT = 100


def _gap_json(gap: "GapItem") -> Dict[str, Any]:
    return {
        "entity_key": gap.entity_key,
        "kind": gap.kind,
        "version": gap.version,
        "reason": gap.reason,
        "staging_satisfied": gap.staging_satisfied,
    }


def _quarantine_json(q) -> Dict[str, Any]:
    return {"session_id": q.session_id, "reason": q.reason, "candidates": q.candidates}


def _plan_json(plan: WorkPlan) -> Dict[str, Any]:
    """The WorkPlan as JSON for ``--dry-run``'s stdout -- see the README's
    "--dry-run: prints the work plan" contract and
    ``_DRY_RUN_SESSION_LIST_LIMIT``'s docstring for the sessions-list
    size cutoff."""
    new_sessions: Any
    if len(plan.new_sessions) <= _DRY_RUN_SESSION_LIST_LIMIT:
        new_sessions = plan.new_sessions
    else:
        new_sessions = {
            "count": len(plan.new_sessions),
            "sample": plan.new_sessions[:20],
        }
    return {
        "ref_sha": plan.ref_sha,
        "new_changes": plan.new_changes,
        "new_tasks": plan.new_tasks,
        "new_sessions_count": len(plan.new_sessions),
        "new_sessions": new_sessions,
        "missing_complexity": [_gap_json(g) for g in plan.missing_complexity],
        "missing_grades": [_gap_json(g) for g in plan.missing_grades],
        "missing_phase_labels": [_gap_json(g) for g in plan.missing_phase_labels],
        "quarantined_count": len(plan.quarantined),
        "quarantined": [_quarantine_json(q) for q in plan.quarantined],
        "unlanded": plan.unlanded,
    }


def _connect_for_cli(db_path, dry_run: bool):
    """Open the target database -- except for ``--dry-run`` against a
    database that doesn't exist yet, which must not create/touch it (a
    plan-only command shouldn't have any filesystem side effect): plan
    against a throwaway in-memory database with the same (empty) schema
    instead."""
    if dry_run and not Path(db_path).exists():
        return db.connect(":memory:")
    return db.connect(db_path)


def main(argv=None) -> int:
    args = _build_arg_parser().parse_args(argv)
    conn = _connect_for_cli(args.db, args.dry_run)

    if args.command == "rebuild-derived":
        try:
            rows_written = costs.rebuild(conn)
        finally:
            conn.close()
        print(json.dumps({"command": "rebuild-derived", "rows_written": rows_written}, indent=2))
        return 0

    if args.command == "backfill-turns":
        try:
            stats = backfill_turns(conn, analysis_dir=args.analysis_dir, regenerate=args.regenerate)
        finally:
            conn.close()
        print(json.dumps({"command": "backfill-turns", **stats}, indent=2))
        return 0

    if args.command == "verify-turn-phases":
        try:
            violations = costs.verify_turn_phases_integrity(conn)
        finally:
            conn.close()
        print(json.dumps(
            {"command": "verify-turn-phases", "violation_count": len(violations), "violations": violations},
            indent=2, sort_keys=True,
        ))
        return 1 if violations else 0

    agent_runner = None
    if not args.dry_run and not args.no_agents:
        try:
            import agents  # Task 7's real dispatcher

            agent_runner = agents.claude_runner
        except ImportError:
            agent_runner = None

    try:
        report = run(
            conn, args.repo,
            dry_run=args.dry_run, no_agents=args.no_agents,
            rescore=args.rescore, agent_runner=agent_runner, change=args.change,
            strict=args.strict,
        )
    finally:
        conn.close()

    if report.dry_run:
        # followup-6, defect 1: a dry run must print the actual WorkPlan
        # (what WOULD happen), not a RunReport shell of all-zero write
        # counts and no plan content -- `run(dry_run=True)` never writes
        # anything, so every RunReport write-count field is meaninglessly
        # zero on this path; `report.plan` is where the real content is.
        print(json.dumps({
            "ref_sha": report.ref_sha, "dry_run": True,
            "plan": _plan_json(report.plan) if report.plan is not None else None,
        }, indent=2))
        return 0

    print(json.dumps({
        "ref_sha": report.ref_sha, "dry_run": report.dry_run,
        "changes_written": report.changes_written, "tasks_written": report.tasks_written,
        "sessions_written": report.sessions_written, "rows_written": report.rows_written,
        "dispatched": report.dispatched, "staged_used": report.staged_used,
        "stale": report.stale, "quarantined": len(report.quarantined),
        "unlanded": report.unlanded,
        "total_writes": report.total_writes,
        "ungradeable": report.ungradeable, "ungradeable_items": report.ungradeable_items,
        "failed": report.failed, "failed_items": report.failed_items,
        "healed_sessions": report.healed_sessions, "healed_session_ids": report.healed_session_ids,
        "superseded_grades": report.superseded_grades,
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
