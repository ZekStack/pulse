#!/usr/bin/env python3
"""Extract one release section from CHANGELOG.md."""

from __future__ import annotations

import argparse
from pathlib import Path


def extract_section(changelog: str, version: str) -> str:
    heading = f"## {version}"
    lines = changelog.splitlines()
    try:
        start = lines.index(heading)
    except ValueError as error:
        raise ValueError(f"missing {heading!r} in CHANGELOG.md") from error

    end = len(lines)
    for index in range(start + 1, len(lines)):
        if lines[index].startswith("## "):
            end = index
            break

    section = "\n".join(lines[start:end]).strip()
    if not section:
        raise ValueError(f"empty changelog section for {version}")
    return section + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-ref", required=False)
    parser.add_argument("--tag-name", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    if not args.tag_name.startswith("v"):
        raise ValueError("release tag must start with 'v'")

    version = args.tag_name[1:]
    changelog = Path("CHANGELOG.md").read_text(encoding="utf-8")
    release_body = extract_section(changelog, version)
    Path(args.output).write_text(release_body, encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"Release changelog generation failed: {error}")
        raise SystemExit(1)
