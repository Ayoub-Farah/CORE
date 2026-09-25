"""Inspect plain USB artifacts and attest the local USB build configuration.

Image inspection alone cannot prove which application is compiled. The build
hook adds separate evidence from the actual Kconfig and defined ELF symbols.
Neither check replaces the installed bootloader's signature verification.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

from ota_artifact import PROTECTED_TLV_MAGIC, _tlvs, inspect_usb_image, require
from ota_pio import artifact_options


def inspect_plain_usb(data, profile=None, version=None, build_id=None):
    """Require a padded signed image without any signed OTA image-class tag."""
    result = inspect_usb_image(data, profile, version, build_id, require_class=False)
    if result["protected_tlv_size"]:
        start = result["header_size"] + result["body_size"]
        _, entries = _tlvs(data, start, PROTECTED_TLV_MAGIC, result["protected_tlv_size"])
        require(not any(kind == 0xA0 for kind, _ in entries),
                "ordinary USB image must not contain an OTA image class TLV")
    result["image_class"] = "usb"
    result["execution_profile"] = "usb"
    return result


def verify_usb_build(config_bytes, elf_bytes, symbols):
    """Validate build evidence; returned hashes refer to these exact input bytes."""
    require(bool(config_bytes) and bool(elf_bytes), "missing USB Kconfig or ELF evidence")
    require(elf_bytes.startswith(b"\x7fELF"), "USB build evidence is not an ELF file")
    options = {}
    for line in config_bytes.decode("utf-8").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            name, value = line.split("=", 1)
            require(name not in options, "duplicate Kconfig option " + name)
            options[name] = value
    require(options.get("CONFIG_BOOTLOADER_MCUBOOT") == "y",
            "ordinary USB build must use the existing MCUboot bootloader")
    require(not any(name.startswith("CONFIG_OWNTECH_OTA") and value in ("y", "m")
                    for name, value in options.items()),
            "ordinary USB build must disable OTA, recovery and transition profiles")

    functions = []
    for line in symbols.splitlines():
        match = re.fullmatch(r"\s*[0-9a-fA-F]+\s+([tTwW])\s+(.+?)\s*", line)
        if match:
            functions.append(match[2])
    require(any(re.fullmatch(r"_img_validation(?:\(\))?(?:\s+\[clone .+\])?", name)
                for name in functions),
            "ordinary USB ELF lacks the automatic image confirmation function")
    forbidden = ("initialize_runtime(", "ota_runtime_", "ota_service_", "ota_receiver_",
                 "ota_lead_", "ota_recovery_", "ota_transition_")
    require(not any(part in name for name in functions for part in forbidden),
            "ordinary USB ELF contains an OTA, recovery or transition entry point")
    return {
        "config_sha256": hashlib.sha256(config_bytes).hexdigest(),
        "elf_sha256": hashlib.sha256(elf_bytes).hexdigest(),
        "ota_enabled": False,
        "recovery_enabled": False,
        "auto_confirmation": True,
    }


def usb_artifact_post_action(source, target, env):
    """Inspect the signed USB output and archive its verified local build proof."""
    try:
        require(env.subst("$PIOENV") == "USB", "plain USB artifact hook requires the USB environment")
        build = Path(env.subst("$BUILD_DIR"))
        project = Path(env.subst("$PROJECT_DIR"))
        image_path = Path(env.subst("$BUILD_DIR/${PROGNAME}.mcuboot.bin"))
        elf_path = Path(env.subst("$BUILD_DIR/${PROGNAME}.elf"))
        config_path = build / "zephyr" / ".config"
        data = image_path.read_bytes()
        config_bytes, elf_bytes = config_path.read_bytes(), elf_path.read_bytes()
        toolchain = Path(env.PioPlatform().get_package_dir("toolchain-gccarmnoneeabi"))
        nm = toolchain / "bin" / ("arm-none-eabi-nm.exe" if os.name == "nt" else "arm-none-eabi-nm")
        symbols = subprocess.run([str(nm), "-C", "--defined-only", str(elf_path)],
                                 capture_output=True, text=True, check=True, timeout=30).stdout
        require(config_path.read_bytes() == config_bytes and elf_path.read_bytes() == elf_bytes,
                "USB build evidence changed during inspection")
        proof = verify_usb_build(config_bytes, elf_bytes, symbols)
        require(len(data) >= 32, "truncated signed USB image")
        major, minor, revision, build_number = struct.unpack_from("<BBHI", data, 20)
        env["OWNTECH_OTA_VERSION"] = "%d.%d.%d+%d" % (major, minor, revision, build_number)
        env["OWNTECH_OTA_BUILD_ID"] = "usb-" + proof["elf_sha256"][:20]
        # Reuse existing signing-key/profile resolution, without generating keys
        # or claiming that the ordinary USB runtime publishes an OTA identity.
        profile, _, _, build_id = artifact_options(env)
        manifest = inspect_plain_usb(data, profile, build_id=build_id)
        manifest.update(filename=image_path.name, build_proof=proof)
        content = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        manifest_name = env.subst("${PROGNAME}.usb.json")
        (build / manifest_name).write_text(content, encoding="utf-8")
        snapshots = project / "ota-artifacts" / "USB"
        snapshots.mkdir(parents=True, exist_ok=True)
        (snapshots / image_path.name).write_bytes(data)
        (snapshots / manifest_name).write_text(content, encoding="utf-8")
        print("USB artifact checked: %d useful / %d transmitted bytes" %
              (manifest["useful_size"], manifest["artifact_size"]))
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("USB artifact rejected: %s" % error)
        return 1
