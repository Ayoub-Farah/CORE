#!/usr/bin/env python3
"""Copy Core and an unchanged MMC main.cpp into an isolated sizing workspace."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mmc-source", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, default=Path(".pio/mms"),
                        help="new short path under this repository (Windows toolchain path limit)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    dest = (root / args.workspace).resolve()
    if root not in dest.parents or dest.exists():
        raise ValueError("choose a new sizing directory inside the repository; existing evidence is preserved")
    source = args.mmc_source.resolve()
    source_bytes = source.read_bytes()
    names = subprocess.check_output(["git", "ls-files", "-co", "--exclude-standard", "-z"], cwd=root).decode().split("\0")
    for name in sorted(set(names)):
        if not name or name.startswith(("Idea/", "docs/", "tests/", ".github/", ".vscode/")):
            continue
        original = root / name
        if original.is_file():
            target = dest / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, target)
    for environment in ("USB", "OTA"):
        library = root / "owntech/lib" / environment
        if library.is_dir():
            shutil.copytree(library, dest / "owntech/lib" / environment, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("__pycache__"))
    (dest / "src/main.cpp").write_bytes(source_bytes)
    profile = dest / "zephyr/profiles/ota_receiver.conf"
    profile.write_text(profile.read_text(encoding="utf-8") +
                       "\n# SIZING ONLY: include all arm/erase code. Never flash this build.\n"
                       "CONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED=y\n", encoding="utf-8")
    if source.read_bytes() != source_bytes:
        raise ValueError("MMC source changed while copying; discard this sizing attempt")
    evidence = {"mmc_sha256": hashlib.sha256(source_bytes).hexdigest(), "qualified": False,
                "sizing_only_arm_path_enabled": True,
                "warning": "Never flash this copy. MMC health/maintenance, heap, stacks, timing and hardware remain unqualified."}
    (dest / "SIZING_ONLY.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(dest)


if __name__ == "__main__":
    main()
