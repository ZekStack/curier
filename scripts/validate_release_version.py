#!/usr/bin/env python3

import json
import os
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def property_value(name: str) -> str:
    for line in (ROOT / "library.properties").read_text().splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() == name:
            return value.strip()
    raise SystemExit(f"missing {name} in library.properties")


manifest_version = json.loads((ROOT / "library.json").read_text())["version"]
properties_version = property_value("version")

if manifest_version != properties_version:
    raise SystemExit(
        f"version mismatch: library.json={manifest_version}, "
        f"library.properties={properties_version}"
    )

changelog = (ROOT / "CHANGELOG.md").read_text()
if not re.search(
    rf"^## {re.escape(manifest_version)}\s*$",
    changelog,
    flags=re.MULTILINE,
):
    raise SystemExit(f"CHANGELOG.md has no {manifest_version} heading")

github_ref = os.environ.get("GITHUB_REF", "")
if github_ref.startswith("refs/tags/"):
    tag = github_ref.removeprefix("refs/tags/")
    if tag != f"v{manifest_version}":
        raise SystemExit(
            f"release tag {tag} does not match library version "
            f"v{manifest_version}"
        )

print(f"Curier release metadata is consistent at {manifest_version}")
