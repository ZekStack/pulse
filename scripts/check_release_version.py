#!/usr/bin/env python3
"""Validate Pulse package versions and, for releases, the Git tag."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

SEMVER = re.compile(
    r"^(0|[1-9]\d*)\."
    r"(0|[1-9]\d*)\."
    r"(0|[1-9]\d*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)


def library_properties_version(path: Path) -> str:
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() == "version":
            return value.strip()
    raise ValueError(f"missing version in {path}")


def require_semver(label: str, version: str) -> None:
    if not SEMVER.fullmatch(version):
        raise ValueError(f"{label} is not valid SemVer: {version!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--tag",
        help="Release tag to validate, for example v0.1.0. Omit on branches and pull requests.",
    )
    args = parser.parse_args()

    properties_version = library_properties_version(Path("library.properties"))
    library_json = json.loads(Path("library.json").read_text(encoding="utf-8"))
    json_version = str(library_json.get("version", ""))

    require_semver("library.properties version", properties_version)
    require_semver("library.json version", json_version)

    if properties_version != json_version:
        raise ValueError(
            "package version mismatch: "
            f"library.properties={properties_version}, library.json={json_version}"
        )

    if args.tag:
        if not args.tag.startswith("v"):
            raise ValueError(f"release tag must start with 'v': {args.tag!r}")
        tag_version = args.tag[1:]
        require_semver("release tag version", tag_version)
        if tag_version != properties_version:
            raise ValueError(
                f"release tag {args.tag!r} does not match package version {properties_version!r}"
            )

    print(f"Pulse version check passed: {properties_version}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Pulse version check failed: {error}")
        raise SystemExit(1)
