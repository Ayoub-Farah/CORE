#!/usr/bin/env python3
"""Archive static MMC/OTA sizing evidence; never claim runtime qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


def map_sizes(path):
    text = path.read_text(encoding="utf-8")
    linked = text.split("Linker script and memory map", 1)[1]
    def symbol(name):
        found = re.search(r"^\s*(0x[0-9a-fA-F]+)\s+" + re.escape(name) + r"\s*=", linked, re.M)
        if not found:
            raise ValueError(f"missing linker symbol {name} in {path}")
        return int(found[1], 16)
    return {
        "flash": symbol("_flash_used"),
        "ram": symbol("_image_ram_size"),
        "ram_capacity": symbol("__kernel_ram_end") - symbol("_image_ram_start"),
        "fleet_symbols_present": bool(re.search(r"ota_coordinator|ota_lead_runtime|_ZL9inventory|_ZL6frozen", linked)),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-build", type=Path, required=True)
    parser.add_argument("--receiver-build", type=Path, required=True)
    parser.add_argument("--mmc-source", type=Path, required=True)
    parser.add_argument("--copied-source", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    original = args.mmc_source.read_bytes()
    if original != args.copied_source.read_bytes():
        raise ValueError("MMC source differs from its sizing copy")
    maps = {name: build / "zephyr/zephyr_final.map" for name, build in
            (("USB", args.baseline_build), ("OTA", args.receiver_build))}
    sizes = {name: map_sizes(path) for name, path in maps.items()}
    manifest_file = args.receiver_build / "firmware.can.json"
    image_file = args.receiver_build / "firmware.can.bin"
    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    image = image_file.read_bytes()
    if len(image) != manifest["artifact_size"] or hashlib.sha256(image).hexdigest() != manifest["artifact_sha256"]:
        raise ValueError("signed artifact does not match its manifest")
    config_file = args.receiver_build / "zephyr/.config"
    config = config_file.read_text(encoding="utf-8")
    if "CONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED=y" not in config:
        raise ValueError("sizing must include the complete arm/erase implementation; use an isolated build only")
    delta = {kind: sizes["OTA"][kind] - sizes["USB"][kind] for kind in ("flash", "ram")}
    static_budget_passed = (delta["flash"] <= 83000 and delta["ram"] <= 28000
                            and len(image) <= manifest["profile"]["useful_capacity"]
                            and not sizes["OTA"]["fleet_symbols_present"])
    result = {
        "qualified": False,
        "sizing_only_arm_path_enabled": True,
        "mmc_sha256": hashlib.sha256(original).hexdigest(),
        "linker": sizes, "ota_delta": delta, "signed_receiver_bytes": len(image),
        "known_scope_allocation": 57680,
        "headroom_before_other_allocations": sizes["OTA"]["ram_capacity"] - sizes["OTA"]["ram"] - 57680,
        "static_budget_passed": static_budget_passed,
        "unmeasured": ["other heap allocations and allocator overhead", "stack high-water marks",
                       "HRTIM jitter and latency", "RS485 losses and latency", "power-loss and USB hardware cycles"],
    }
    args.archive.mkdir(parents=True, exist_ok=True)
    for name, path in maps.items():
        shutil.copy2(path, args.archive / (name + ".map"))
    for path in (manifest_file, image_file):
        shutil.copy2(path, args.archive / path.name)
    shutil.copy2(config_file, args.archive / "OTA.config")
    shutil.copy2(args.baseline_build / "zephyr/.config", args.archive / "USB.config")
    (args.archive / "SIZING_ONLY_DO_NOT_FLASH.txt").write_text(
        "Isolated MMC sizing build with arm code compiled in. Not qualified. Do not flash this artifact.\n", encoding="utf-8")
    (args.archive / "sizes.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if static_budget_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
