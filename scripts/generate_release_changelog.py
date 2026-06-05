#!/usr/bin/env python3
"""Generate a small release changelog from git history."""

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
    tag = run_git(["describe", "--tags", "--abbrev=0", f"{target_ref}^"])
    return tag


def commits_between(previous: str, target_ref: str) -> list[str]:
    revision = f"{previous}..{target_ref}" if previous else target_ref
    output = run_git(["log", "--pretty=format:- %s", revision])
    return [line for line in output.splitlines() if line.strip()]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-ref", required=True)
    parser.add_argument("--tag-name", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    previous = previous_tag(args.target_ref)
    commits = commits_between(previous, args.target_ref)

    lines = [f"## {args.tag_name}", ""]
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
