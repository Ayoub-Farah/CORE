#!/usr/bin/env python3
"""Inspect existing signed firmware.mcuboot.bin without modifying or signing it.

Transfer SHA256 covers the full padded file; MCUboot SHA256 covers its header,
program and protected TLVs. These checks do not attest signature acceptance by
the installed bootloader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

IMAGE_MAGIC = 0x96F3B83D
BOOT_MAGIC = bytes.fromhex("77c295f360d2ef7f3552500f2cb67980")
TLV_MAGIC = 0x6907
PROTECTED_TLV_MAGIC = 0x6908
SIGNATURES = {0x20: ("rsa2048", 256, 256), 0x22: ("ecdsa", 8, 104),
              0x23: ("rsa3072", 384, 384), 0x24: ("ed25519", 64, 64)}
DEFAULT_PROFILE = {
    "slot_size": 0x37800, "useful_capacity": 0x36000, "header_size": 0x200,
    "write_alignment": 8, "max_alignment": 8, "max_sectors": 128,
    "hardware_id": 0x01020142, "layout_id": 0x00010001, "bootloader_id": 0x00010100,
    "compatibility": {"board": "spin", "board_revision": "1_2_0",
                      "shield": "twist", "shield_revision": "1_4_2"},
    "bootloader_qualification": "unqualified: provisional swap-move capacity; installed binary unknown",
    "signing_key": "existing PlatformIO chain (resolve before release)",
}


class ArtifactError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ArtifactError(message)


def validate_profile(profile=None):
    result = dict(DEFAULT_PROFILE)
    result.update(profile or {})
    for key in ("slot_size", "useful_capacity", "header_size", "write_alignment",
                "max_alignment", "max_sectors", "hardware_id", "layout_id", "bootloader_id"):
        require(type(result[key]) is int and 0 < result[key] <= 0xFFFFFFFF,
                key + " must be a positive uint32")
    require(result["write_alignment"] == result["max_alignment"] == 8,
            "prototype supports the existing 8-byte alignment only")
    reserve = result["max_sectors"] * 3 * 8 + 4 * 8 + 16
    require(32 <= result["header_size"] < result["useful_capacity"]
            <= result["slot_size"] - reserve, "invalid separate useful/file capacities")
    require(result["header_size"] % 8 == result["slot_size"] % 8 == 0,
            "header and slot must be aligned")
    return result


def load_profile(path):
    return validate_profile(json.loads(Path(path).read_text(encoding="utf-8")))


def _tlvs(data, start, magic, expected_size=None):
    require(start + 4 <= len(data), "truncated TLV info")
    actual, total = struct.unpack_from("<HH", data, start)
    require(actual == magic and total >= 4, "invalid TLV info")
    require(expected_size is None or total == expected_size, "protected TLV length mismatch")
    end = start + total
    require(end <= len(data), "TLV area exceeds artifact")
    entries = []
    pos = start + 4
    while pos < end:
        require(pos + 4 <= end, "truncated TLV entry")
        kind, length = struct.unpack_from("<HH", data, pos)
        pos += 4
        require(length > 0 and pos + length <= end, "invalid TLV value bounds")
        entries.append((kind, data[pos:pos + length]))
        pos += length
    return end, entries


def inspect_image(data, profile=None, version=None, build_id=None):
    profile = validate_profile(profile)
    require(len(data) == profile["slot_size"],
            "expected the complete existing padded firmware.mcuboot.bin (slot-sized)")
    values = struct.unpack_from("<IIHHIIBBHII", data)
    magic, load, hsize, protected_size, bsize, flags, major, minor, revision, build, reserved = values
    require(magic == IMAGE_MAGIC, "invalid MCUboot magic")
    require(load == flags == reserved == 0, "unsupported image flags/load/reserved")
    require(hsize == profile["header_size"] and not any(data[32:hsize]), "invalid reserved MCUboot header")
    require(bsize > 0 and hsize + bsize + protected_size <= profile["useful_capacity"],
            "program/protected TLVs exceed useful capacity")
    image_version = "%d.%d.%d+%d" % (major, minor, revision, build)
    expected_version = version or profile.get("version")
    if expected_version:
        require(image_version == (expected_version if "+" in expected_version else expected_version + "+0"),
                "MCUboot version differs from requested version")
    pos = hsize + bsize
    if protected_size:
        pos, protected = _tlvs(data, pos, PROTECTED_TLV_MAGIC, protected_size)
        require(not any(kind in (1, 2, 0x10, 0x11, 0x12, 0x25) or kind in SIGNATURES
                        for kind, _ in protected), "authentication TLV in protected area")
    hash_end = pos
    useful_size, entries = _tlvs(data, pos, TLV_MAGIC)
    require(useful_size <= profile["useful_capacity"], "header/program/TLVs exceed useful capacity")
    hashes = [value for kind, value in entries if kind == 0x10]
    keys = [value for kind, value in entries if kind == 1]
    signatures = [(kind, value) for kind, value in entries if kind in SIGNATURES]
    require(len(hashes) == len(keys) == len(signatures) == 1,
            "exactly one SHA256, key hash and signature required")
    require(not any(kind in (2, 0x11, 0x12, 0x25) or 0x30 <= kind <= 0x33 for kind, _ in entries),
            "encrypted/full-key/pure-signature images are outside this prototype")
    require(hashes[0] == hashlib.sha256(data[:hash_end]).digest(), "MCUboot internal hash mismatch")
    require(len(keys[0]) == 32, "invalid public key hash")
    if profile.get("public_key_sha256"):
        require(keys[0].hex() == profile["public_key_sha256"], "unexpected signing key")
    sig_kind, signature = signatures[0]
    sig_name, low, high = SIGNATURES[sig_kind]
    require(low <= len(signature) <= high, "invalid signature length")
    require(data[-16:] == BOOT_MAGIC, "missing expected padded MCUboot activation magic")
    require(all(value == 0xFF for value in data[useful_size:-16]),
            "non-erased padding/trailer; only the existing --pad artifact is accepted")
    return {
        "schema_version": 1, "protocol": 1, "version": image_version,
        "build_id": build_id or profile.get("build_id") or image_version,
        "artifact_size": len(data), "useful_size": useful_size,
        "artifact_sha256": hashlib.sha256(data).hexdigest(), "mcuboot_image_hash": hashes[0].hex(),
        "artifact_hash_domain": "exact-file-including-header-tlvs-padding-trailer",
        "mcuboot_hash_domain": "header-program-protected-tlvs",
        "format": "mcuboot-padded", "activation_trailer": True,
        "header_size": hsize, "body_size": bsize, "protected_tlv_size": protected_size,
        "signature": {"type": sig_name, "key_sha256": keys[0].hex(), "verified": False,
                      "signing_key": profile["signing_key"]},
        "profile": profile,
        "hardware_id": profile["hardware_id"], "layout_id": profile["layout_id"],
        "bootloader_id": profile["bootloader_id"],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["inspect"])
    parser.add_argument("image", type=Path)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--build-id")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = inspect_image(args.image.read_bytes(), load_profile(args.profile) if args.profile else None,
                                 args.version, args.build_id)
        manifest["filename"] = args.image.name
        content = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(content, encoding="utf-8")
        else:
            print(content, end="")
    except (OSError, ValueError) as error:
        parser.exit(1, "OTA artifact: %s\n" % error)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
