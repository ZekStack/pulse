#!/usr/bin/env python3
"""Validate Pulse package versions on branches and release tags."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

SEMVER = re.compile(
    r"^(0|[1-9]\d*)\."
    r"(0|[1-9]\d*)\."
    r"(0|[1-9]\d*)"
    r"(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)


def read_properties_version(path: Path) -> str:
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() == "version":
            return value.strip()
    raise ValueError(f"{path} does not contain a version entry")


def require_semver(label: str, version: str) -> None:
    if not SEMVER.fullmatch(version):
        raise ValueError(f"{label} is not valid SemVer: {version!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--tag",
        help="Release tag to validate. Omit on branches and pull requests.",
    )
    args = parser.parse_args()

    properties_version = read_properties_version(Path("library.properties"))
    package = json.loads(Path("library.json").read_text(encoding="utf-8"))
    json_version = package.get("version")
    if not isinstance(json_version, str):
        raise ValueError("library.json version must be a string")

    require_semver("library.properties version", properties_version)
    require_semver("library.json version", json_version)
    if properties_version != json_version:
        raise ValueError(
            "package version mismatch: "
            f"library.properties={properties_version}, library.json={json_version}"
        )

    if args.tag is not None:
        if not args.tag.startswith("v"):
            raise ValueError(f"release tag must start with 'v': {args.tag!r}")
        tag_version = args.tag[1:]
        require_semver("release tag version", tag_version)
        if tag_version != properties_version:
            raise ValueError(
                f"release tag {args.tag} does not match package version {properties_version}"
            )

    print(f"Pulse version metadata is consistent: {properties_version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
