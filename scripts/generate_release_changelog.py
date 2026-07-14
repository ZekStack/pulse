#!/usr/bin/env python3
"""Generate release notes from CHANGELOG.md with a git-history fallback."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def run_git(args: list[str]) -> str:
    completed = subprocess.run(
        ["git", *args],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if completed.returncode != 0:
        return ""
    return completed.stdout.strip()


def previous_tag(target_ref: str) -> str:
    return run_git(["describe", "--tags", "--abbrev=0", f"{target_ref}^"])


def commits_between(previous: str, target_ref: str) -> list[str]:
    revision = f"{previous}..{target_ref}" if previous else target_ref
    output = run_git(["log", "--pretty=format:- %s", revision])
    return [line for line in output.splitlines() if line.strip()]


def changelog_section(version: str) -> list[str]:
    path = Path("CHANGELOG.md")
    if not path.exists():
        return []

    lines = path.read_text(encoding="utf-8").splitlines()
    heading = f"## {version}"
    start: int | None = None
    for index, line in enumerate(lines):
        if line.strip() == heading:
            start = index + 1
            break
    if start is None:
        return []

    end = len(lines)
    for index in range(start, len(lines)):
        if lines[index].startswith("## "):
            end = index
            break

    section = lines[start:end]
    while section and not section[0].strip():
        section.pop(0)
    while section and not section[-1].strip():
        section.pop()
    return section


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-ref", required=True)
    parser.add_argument("--tag-name", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    version = args.tag_name[1:] if args.tag_name.startswith("v") else args.tag_name
    curated = changelog_section(version)
    lines = [f"## {args.tag_name}", ""]

    if curated:
        lines.extend(curated)
    else:
        previous = previous_tag(args.target_ref)
        commits = commits_between(previous, args.target_ref)
        if previous:
            lines.append(f"Changes since `{previous}`.")
            lines.append("")
        if commits:
            lines.extend(commits)
        else:
            lines.append("- Initial release.")

    lines.append("")
    Path(args.output).write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
