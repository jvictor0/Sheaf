#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from datetime import date
from pathlib import Path


x_ROOT = Path(__file__).resolve().parent.parent
x_WORKFLOWS_DIR = x_ROOT / "workflows"
x_MAIN_QUESTS_DIR = x_WORKFLOWS_DIR / "main_quests"
x_SIDE_QUESTS_DIR = x_WORKFLOWS_DIR / "side_quests"


def NormalizeName(raw_name: str) -> str:
    normalized = re.sub(r"[^a-z0-9]+", "_", raw_name.strip().lower())
    normalized = normalized.strip("_")
    if not normalized:
        raise ValueError("Name must contain at least one letter or number.")

    return normalized


def WriteFile(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")


def MakeMainQuestStatus(date_text: str, summary: str) -> str:
    return f"""# Status

- Stage: `planning`
- Updated: `{date_text}`
- Summary: {summary}
"""


def MakeMainQuestDecisions(date_text: str) -> str:
    return f"""# Decisions

- {date_text}: Main quest created.
"""


def MakeMainQuestIssues() -> str:
    return """# Issues

No issues have been recorded yet.

## Example Issue

Describe the issue here.

Status: `open`

Next Action: `fix`
"""


def MakeMainQuestSideQuests() -> str:
    return """# Side Quests

No side quests have been created for this main quest.
"""


def MakeMainQuestSpec(quest_name: str, date_text: str, summary: str) -> str:
    return f"""# Scope

## Quest

- Name: `{quest_name}`
- Created: `{date_text}`

## Summary

{summary}

## Goals

- TODO

## Non-Goals

- TODO

## Open Questions

- TODO
"""


def MakeSideQuestStatus(date_text: str, summary: str) -> str:
    return f"""# Status

- Stage: `planning`
- Updated: `{date_text}`
- Summary: {summary}
"""


def MakeSideQuestDecisions(date_text: str) -> str:
    return f"""# Decisions

- {date_text}: Side quest created.
"""


def MakeSideQuestIssues() -> str:
    return """# Issues

No issues have been recorded yet.

## Example Issue

Describe the issue here.

Status: `open`

Next Action: `fix`
"""


def MakeSideQuestMainQuest(main_quest_name: str, reason: str) -> str:
    return f"""# Main Quest

- Main Quest: `{main_quest_name}`
- Path: `../../main_quests/{main_quest_name}/`
- Reason Split Out: {reason}
- Source Issue: TODO
"""


def MakeRelatedSideQuests() -> str:
    return """# Related Side Quests

No related side quests have been recorded yet.
"""


def MakeSideQuestSpec(
    quest_name: str,
    main_quest_name: str,
    date_text: str,
    summary: str,
) -> str:
    return f"""# Scope

## Quest

- Name: `{quest_name}`
- Main Quest: `{main_quest_name}`
- Created: `{date_text}`

## Summary

{summary}

## Goals

- TODO

## Non-Goals

- TODO

## Open Questions

- TODO
"""


def EnsureQuestDoesNotExist(quest_dir: Path) -> None:
    if quest_dir.exists():
        raise FileExistsError(f"Quest already exists: {quest_dir}")


def CreateMainQuest(raw_name: str, summary: str) -> Path:
    quest_name = NormalizeName(raw_name)
    quest_dir = x_MAIN_QUESTS_DIR / quest_name
    spec_dir = quest_dir / "specs"
    date_text = date.today().isoformat()

    EnsureQuestDoesNotExist(quest_dir)

    spec_dir.mkdir(parents=True, exist_ok=False)
    WriteFile(quest_dir / "status.md", MakeMainQuestStatus(date_text, summary))
    WriteFile(quest_dir / "decisions.md", MakeMainQuestDecisions(date_text))
    WriteFile(quest_dir / "issues.md", MakeMainQuestIssues())
    WriteFile(quest_dir / "side_quests.md", MakeMainQuestSideQuests())
    WriteFile(
        spec_dir / "01_scope.md",
        MakeMainQuestSpec(quest_name, date_text, summary),
    )

    return quest_dir


def CreateSideQuest(raw_name: str, raw_main_quest_name: str, summary: str) -> Path:
    quest_name = NormalizeName(raw_name)
    main_quest_name = NormalizeName(raw_main_quest_name)
    quest_dir = x_SIDE_QUESTS_DIR / quest_name
    main_quest_dir = x_MAIN_QUESTS_DIR / main_quest_name
    spec_dir = quest_dir / "specs"
    date_text = date.today().isoformat()

    if not main_quest_dir.exists():
        raise FileNotFoundError(
            f"Main quest does not exist: {main_quest_dir}"
        )

    EnsureQuestDoesNotExist(quest_dir)

    spec_dir.mkdir(parents=True, exist_ok=False)
    WriteFile(quest_dir / "status.md", MakeSideQuestStatus(date_text, summary))
    WriteFile(quest_dir / "decisions.md", MakeSideQuestDecisions(date_text))
    WriteFile(quest_dir / "issues.md", MakeSideQuestIssues())
    WriteFile(
        quest_dir / "main_quest.md",
        MakeSideQuestMainQuest(main_quest_name, summary),
    )
    WriteFile(
        quest_dir / "related_side_quests.md",
        MakeRelatedSideQuests(),
    )
    WriteFile(
        spec_dir / "01_scope.md",
        MakeSideQuestSpec(quest_name, main_quest_name, date_text, summary),
    )

    return quest_dir


def BuildParser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Create workflow quest scaffolding."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    main_parser = subparsers.add_parser(
        "main",
        help="Create a main quest.",
    )
    main_parser.add_argument("name", help="Main quest name.")
    main_parser.add_argument(
        "--summary",
        default="TODO",
        help="Initial summary text.",
    )

    side_parser = subparsers.add_parser(
        "side",
        help="Create a side quest.",
    )
    side_parser.add_argument("name", help="Side quest name.")
    side_parser.add_argument(
        "--main-quest",
        required=True,
        help="Owning main quest name.",
    )
    side_parser.add_argument(
        "--summary",
        default="TODO",
        help="Initial summary text.",
    )

    return parser


def main() -> int:
    parser = BuildParser()
    args = parser.parse_args()

    x_MAIN_QUESTS_DIR.mkdir(parents=True, exist_ok=True)
    x_SIDE_QUESTS_DIR.mkdir(parents=True, exist_ok=True)

    try:
        if args.command == "main":
            created_dir = CreateMainQuest(args.name, args.summary)
            print(f"Created main quest at {created_dir}")
            return 0

        if args.command == "side":
            created_dir = CreateSideQuest(
                args.name,
                args.main_quest,
                args.summary,
            )
            print(f"Created side quest at {created_dir}")
            print(
                "Remember to add this side quest to the owning "
                "main quest's side_quests.md file."
            )
            return 0

        parser.error(f"Unknown command: {args.command}")
        return 2
    except (FileExistsError, FileNotFoundError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
