"""Asset loading for the task-analyzer pipeline: rubric/prompt frontmatter
and prompt bodies.

Rubrics (``rubrics/*.md``) and prompts (``prompts/*.md``) are versioned
markdown assets with a small frontmatter block (``---\\nkey: value\\n...\\n---\\n``,
see Task 1's ``rubrics/*.md``/``prompts/*.md``). ``read_asset_version`` reads
any such asset's ``version`` field -- this is the cache-key version ingest.py
compares against stored ``rubric_version``/``taxonomy_version`` rows (design.md
D3). ``load_prompt`` reads a prompt asset and returns its body (frontmatter
stripped, ``{{...}}`` placeholders intact for the caller -- Task 7's
Claude dispatch wrapper -- to substitute), plus its ``version`` and
``model_hint``.

Stdlib only.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Dict, Tuple

_ROOT = Path(__file__).resolve().parent
PROMPTS_DIR = _ROOT / "prompts"
RUBRICS_DIR = _ROOT / "rubrics"

_FRONTMATTER_RE = re.compile(r"\A---\n(.*?)\n---\n(.*)\Z", re.S)


def _parse_frontmatter(text: str) -> Tuple[Dict[str, str], str]:
    m = _FRONTMATTER_RE.match(text)
    if not m:
        raise ValueError("asset missing a --- frontmatter block")
    fm: Dict[str, str] = {}
    for line in m.group(1).splitlines():
        if not line.strip():
            continue
        key, _, value = line.partition(":")
        fm[key.strip()] = value.strip()
    return fm, m.group(2)


def read_asset_version(path) -> str:
    """Return the ``version`` frontmatter field of the asset at ``path``.

    Works for both rubric and prompt assets (both use the same frontmatter
    shape); this is the primitive ``ingest.py`` uses to compare a rubric's
    *current* version against the version recorded on a stored judgment row.
    """
    text = Path(path).read_text(encoding="utf-8")
    fm, _body = _parse_frontmatter(text)
    if "version" not in fm:
        raise ValueError(f"{path}: asset missing 'version' frontmatter field")
    return fm["version"]


def load_prompt(name: str) -> Tuple[str, str, str]:
    """Load ``prompts/<name>.md`` (``name`` with or without the ``.md`` suffix).

    Returns ``(text, version, model_hint)``: ``text`` is the prompt body with
    frontmatter stripped and ``{{...}}`` placeholders left intact for the
    caller to substitute; ``version``/``model_hint`` come from the
    frontmatter.
    """
    filename = name if name.endswith(".md") else f"{name}.md"
    path = PROMPTS_DIR / filename
    text = path.read_text(encoding="utf-8")
    fm, body = _parse_frontmatter(text)
    if "version" not in fm:
        raise ValueError(f"{path}: prompt missing 'version' frontmatter field")
    if "model_hint" not in fm:
        raise ValueError(f"{path}: prompt missing 'model_hint' frontmatter field")
    return body.strip("\n"), fm["version"], fm["model_hint"]
