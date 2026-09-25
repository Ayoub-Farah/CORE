#!/usr/bin/env python3
"""Prepare an exact, terminal-v2 USB roundtrip guard; never access a board."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from ota_artifact import inspect_image, inspect_usb_image, validate_profile
from ota_build_identity import normalize_version


class TransitionConfigError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise TransitionConfigError(message)


def _object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def _json(raw):
    return json.loads(raw.decode("utf-8"), object_pairs_hook=_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(TransitionConfigError("invalid JSON constant: " + value)))


def _sha(raw):
    return hashlib.sha256(raw).hexdigest()


def _uint(value, bits, name, minimum=0):
    require(type(value) is int and minimum <= value < 1 << bits, "invalid " + name)
    return value


def _hex(value, size, name):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{%d}" % (size * 2), value)
            and value != "0" * (size * 2), "invalid " + name)
    return value


def _identity(value):
    return _hex(value, 8, "EUI-64 identity")


def _text(value, name):
    require(isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.+\-]{1,31}", value), "invalid " + name)
    return value


def _version(value):
    _text(value, "version")
    result = normalize_version(value)
    require(value == result or value + "+0" == result, "noncanonical image version")
    return result


def _roster(values):
    require(isinstance(values, list) and 1 <= len(values) <= 16, "invalid frozen target roster")
    result = [_identity(value) for value in values]
    require(len(set(result)) == len(result), "duplicate target identity")
    return result


def _manifest(manifest, *, campaign=False):
    require(isinstance(manifest, dict), "missing image manifest")
    require(manifest.get("schema_version") == 2 and manifest.get("protocol") == 2, "only OTA v2 manifests are supported")
    role = manifest.get("image_class")
    require(role in ("receiver", "lead") and (not campaign or role == "receiver"), "invalid image class")
    profile = manifest.get("profile")
    require(isinstance(profile, dict) and all(key in profile for key in
            ("slot_size", "useful_capacity", "header_size", "hardware_id", "layout_id", "bootloader_id")),
            "incomplete image profile")
    checked = validate_profile(profile)
    require(checked["useful_capacity"] == 221184 and checked["slot_size"] == 227328,
            "transition supports only the qualified-layout working bounds")
    for key in ("hardware_id", "layout_id", "bootloader_id"):
        require(_uint(manifest.get(key), 32, key, 1) == checked[key], "manifest/profile mismatch: " + key)
    useful = _uint(manifest.get("useful_size"), 32, "useful size", 1)
    size = _uint(manifest.get("artifact_size"), 32, "artifact size", 1)
    require(checked["header_size"] < useful <= checked["useful_capacity"], "invalid useful image bounds")
    compact = manifest.get("format") == "mcuboot-compact"
    if compact:
        require(size == useful and manifest.get("activation_trailer") is False, "invalid compact artifact")
    else:
        require(not campaign and manifest.get("format") == "mcuboot-usb-padded"
                and manifest.get("activation_trailer") is True and size == checked["slot_size"], "invalid USB artifact")
    _hex(manifest.get("artifact_sha256"), 32, "artifact hash")
    _hex(manifest.get("mcuboot_image_hash"), 32, "MCUboot hash")
    require(_version(manifest.get("version")) == manifest["version"], "manifest version must include its build number")
    _text(manifest.get("build_id"), "build identity")
    signature = manifest.get("signature")
    require(isinstance(signature, dict) and isinstance(signature.get("key_sha256"), str)
            and re.fullmatch(r"[0-9a-f]{64}", signature["key_sha256"]), "missing signing key identity")
    return manifest


def _source(data, manifest):
    _manifest(manifest)
    inspector = inspect_image if manifest["format"] == "mcuboot-compact" else inspect_usb_image
    actual = inspector(data, manifest["profile"], manifest["version"], manifest["build_id"],
                       manifest["image_class"], require_class=True)
    for key, value in actual.items():
        require(manifest.get(key) == value, "source bytes contradict manifest " + key)
    return manifest


def _board(info, manifest, no_campaign):
    require(isinstance(info, dict), "board info must be an object")
    require(info.get("service") == "owntech-ota" and info.get("protocol") == 2, "board is not an OTA v2 application")
    eui = _identity(info.get("identity"))
    serial = info.get("usb_serial")
    require(isinstance(serial, str) and re.fullmatch(r"[A-Za-z0-9_.\-]{1,128}", serial), "capture requires a stable USB serial")
    require(info.get("image_class") == manifest["image_class"], "board/source image class mismatch")
    for key in ("hardware_id", "layout_id", "bootloader_id"):
        require(type(info.get(key)) is int and info[key] == manifest[key], "board/source profile mismatch: " + key)
    for field, key in (("slot_size", "slot_size"), ("useful_capacity", "useful_capacity")):
        require(type(info.get(field)) is int and info[field] == manifest["profile"][key], "board/source capacity mismatch")
    require(info.get("mcuboot_image_hash") == manifest["mcuboot_image_hash"]
            and _version(info.get("version")) == manifest["version"]
            and info.get("build_id") == manifest["build_id"], "source is not the captured running image")
    require(info.get("active_confirmed") is True and info.get("slot_available") is True,
            "running image must be confirmed with its slot available")
    require(info.get("healthy") is True or info.get("local_healthy") is True, "board local health is not established")
    require(type(info.get("error")) is int and info["error"] == 0, "board reports an error")
    allowed = ("IDLE", "WAITING_CAN") if no_campaign else ("IDLE", "WAITING_CAN", "SUCCESS", "SUCCEEDED")
    require(info.get("phase") in allowed, "board is not in a terminal phase")
    if "maintenance" in info:
        require(info["maintenance"] is False, "board remains in maintenance")
    if "role" in info:
        require(info["role"] == ("lead" if manifest["image_class"] == "lead" else "follower"), "board role/class mismatch")
    return eui, serial


def _optional_state(info, campaign, commit):
    for key, expected in (("campaign", campaign), ("commit_id", commit)):
        if key in info:
            require(type(info[key]) is int and info[key] == expected, "captured " + key + " disagrees with terminal evidence")
    if "state" in info:
        require(info["state"] in (("IDLE",) if not campaign else ("SUCCESS", "SUCCEEDED")), "captured state is not terminal")


def _rows(status, targets, manifest, campaign, commit, *, terminal):
    require(isinstance(status, dict) and type(status.get("campaign")) is int and status["campaign"] == campaign
            and type(status.get("target_count")) is int and status["target_count"] == len(targets), "incomplete campaign snapshot")
    rows = status.get("targets")
    require(isinstance(rows, list) and all(isinstance(row, dict) for row in rows)
            and set(_roster([row.get("identity") for row in rows])) == set(targets), "snapshot differs from frozen roster")
    require(type(status.get("error", 0)) is int and status.get("error", 0) == 0, "campaign snapshot reports failure")
    if "commit_id" in status:
        require(type(status["commit_id"]) is int and status["commit_id"] == commit, "snapshot commit mismatch")
    for row in rows:
        require(type(row.get("campaign")) is int and row["campaign"] == campaign,
                "target campaign mismatch")
        require(type(row.get("error")) is int and row["error"] == 0, "target reports failure")
        require(row.get("state") in (("SUCCESS", "SUCCEEDED") if terminal else ("VALID",)), "target is not in the required durable state")
        require(type(row.get("image_size")) is int and row["image_size"] == manifest["artifact_size"], "target image size mismatch")
        if "commit_id" in row:
            require(type(row["commit_id"]) is int and row["commit_id"] == (commit if terminal else 0), "target commit mismatch")
        if "image_class" in row:
            require(row["image_class"] == "receiver", "target is not a receiver")
        if "role" in row:
            require(row["role"] == "follower", "Lead must not be a target")
        if terminal:
            require(row.get("healthy") is True and row.get("confirmed") is True
                    and row.get("mcuboot_image_hash") == manifest["mcuboot_image_hash"]
                    and _version(row.get("version")) == manifest["version"]
                    and row.get("build_id") == manifest["build_id"], "terminal target did not boot the expected healthy confirmed image")
        else:
            require(row.get("validated") is True and row.get("flash_complete") is True
                    and type(row.get("offset")) is int and row["offset"] == manifest["artifact_size"], "commit lacks complete validation")
    return rows


def _campaign(raw, board_info, source, data):
    require(raw and raw.endswith(b"\n"), "journal is empty or has an incomplete final record")
    records = [_json(line) for line in raw.splitlines()]
    require(all(isinstance(row, dict) for row in records), "journal records must be objects")
    discoveries = [row for row in records if row.get("event") == "DISCOVER"]
    require(len(discoveries) == 1, "transition needs exactly one frozen discovery")
    frozen = discoveries[0]
    campaign = _uint(frozen.get("campaign"), 64, "campaign ID", 1)
    require(all(type(row.get("campaign")) is int and row["campaign"] == campaign for row in records), "mixed campaign journal")
    commit = (campaign & 0xFFFFFFFF) or 1  # v2 coordinator's explicit commit contract
    lead = _identity(frozen.get("lead_identity"))
    targets = _roster(frozen.get("targets"))
    require(lead not in targets, "frozen receiver roster includes the Lead")
    manifest = _manifest(frozen.get("manifest"), campaign=True)
    for key in ("hardware_id", "layout_id", "bootloader_id"):
        require(manifest[key] == source[key], "campaign/source profile mismatch")
    for key in ("slot_size", "useful_capacity", "header_size"):
        require(manifest["profile"][key] == source["profile"][key], "campaign/source layout mismatch")
    require(manifest["signature"]["key_sha256"] == source["signature"]["key_sha256"], "campaign/source signing key mismatch")
    if source["image_class"] == "receiver":
        require(board_info["identity"] in targets, "receiver is outside the frozen roster")
        for key in ("mcuboot_image_hash", "version", "build_id", "useful_size"):
            require(manifest[key] == source[key], "receiver source differs from campaign " + key)
        require(_sha(data[:source["useful_size"]]) == manifest["artifact_sha256"], "receiver source differs from exact campaign bytes")
    else:
        require(board_info["identity"] == lead, "captured board is not the frozen Lead")
    inventory = frozen.get("inventory")
    require(isinstance(inventory, list) and all(isinstance(row, dict) for row in inventory)
            and set(_roster([row.get("identity") for row in inventory])) == set(targets), "original inventory differs from frozen roster")
    source_open = start = committed = reconcile = False
    barrier = None
    discovered = False
    success_count = 0
    serials = set()
    for index, record in enumerate(records):
        event = record.get("event")
        require(isinstance(event, str), "invalid journal event")
        if "manifest" in record:
            require(record["manifest"] == manifest, "campaign manifest changed within journal")
        if "commit_id" in record:
            require(type(record["commit_id"]) is int and record["commit_id"] == commit, "journal commit mismatch")
        if event == "USB_SELECTED":
            serial = record.get("usb_serial")
            require(isinstance(serial, str) and re.fullmatch(r"[A-Za-z0-9_.\-]{1,128}", serial), "missing Lead USB serial")
            serials.add(serial)
        if event == "PROBE_LEAD" and record.get("identity") is not None:
            require(record["identity"] == lead, "journal probed a different Lead")
        if event == "DISCOVER":
            require(not discovered and not source_open, "unexpected discovery order")
            discovered = True
        elif event == "PC_SOURCE_OPEN":
            require(discovered and not source_open and not start and record.get("identity") == lead, "unexpected source open")
            source_open = True
        elif event == "START_REQUEST":
            require(source_open and not start and record.get("targets") == targets, "unexpected START or changed roster order")
            start = True
        elif event == "STATUS" and isinstance(record.get("status"), dict):
            status = record["status"]
            # COMMIT is queued. A status poll may still observe ALL_VALIDATED
            # before its worker runs; only a snapshot preceding COMMIT can
            # establish the mandatory validation barrier.
            if start and not committed and status.get("phase") == "ALL_VALIDATED":
                _rows(status, targets, manifest, campaign, commit, terminal=False)
                barrier = status
        elif event == "COMMIT_REQUEST":
            require(start and not committed and barrier is not None and record.get("targets") == targets,
                    "COMMIT lacks the complete frozen validation barrier")
            committed = True
        elif event == "RECONCILE_BEGIN":
            require(committed and record.get("targets") == targets, "reconciliation lacks commit or changed its roster")
            reconcile = True
        elif event == "SUCCESS":
            require(committed and reconcile and record.get("targets") == targets and index == len(records) - 1,
                    "SUCCESS is not the final frozen campaign result")
            require(index > 0 and records[index - 1].get("event") == "STATUS", "SUCCESS lacks a final complete status")
            status = records[index - 1].get("status")
            require(isinstance(status, dict) and status.get("phase") == "SUCCESS", "Lead did not report terminal SUCCESS")
            _rows(status, targets, manifest, campaign, commit, terminal=True)
            success_count += 1
    require(success_count == 1, "journal has no unique terminal SUCCESS")
    require(len(serials) == 1, "journal has no stable unique Lead USB serial")
    if source["image_class"] == "lead":
        require(board_info["usb_serial"] in serials, "captured USB serial is not the journal's Lead")
    _optional_state(board_info, campaign, commit)
    return campaign, commit, lead, targets, manifest


def _cstring(value):
    data = value.encode("ascii")
    return data + bytes(32 - len(data))


def _crc(data):
    return data + struct.pack("<I", zlib.crc32(data))


def _local(campaign, commit, lead, manifest):
    data = struct.pack("<IHHQII4B", 0x324C544F, 2, 168, campaign, commit,
                       manifest["artifact_size"], 12, 2, 1, 0)
    data += bytes.fromhex(lead + manifest["artifact_sha256"] + manifest["mcuboot_image_hash"])
    data += _cstring(manifest["version"]) + _cstring(manifest["build_id"])
    require(len(data) == 164, "internal local record layout mismatch")
    return _crc(data)


def _fleet(campaign, commit, targets, manifest):
    data = struct.pack("<IIQIIIIIB", 0x3341544F, 308, campaign, manifest["artifact_size"],
                       manifest["useful_size"], manifest["hardware_id"], manifest["layout_id"],
                       manifest["bootloader_id"], 2)
    data += bytes.fromhex(manifest["artifact_sha256"] + manifest["mcuboot_image_hash"])
    data += _cstring(manifest["version"]) + _cstring(manifest["build_id"]) + b"\x01"
    data += b"".join(bytes.fromhex(target) for target in targets) + bytes(8 * (16 - len(targets)))
    data += struct.pack("<BBB3xI", len(targets), 0, 12, commit)
    require(len(data) == 304, "internal fleet record layout mismatch")
    return _crc(data)


def _token(config):
    # File locations and whitespace are provenance, not operation semantics.
    semantic = {key: value for key, value in config.items() if key not in
                ("inputs", "input_sha256", "token", "header_sha256", "build_id")}
    return _sha(json.dumps(semantic, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii"))


def transition_config(image_path, manifest_path, board_info_path, *, journal_path=None, no_campaign=False):
    """Derive exact NVS bytes from stable files, supporting unchanged v2 status."""
    require(type(no_campaign) is bool and (bool(journal_path) != no_campaign), "choose --journal or explicit --no-campaign")
    inputs = {"source_image": Path(image_path).resolve(), "source_manifest": Path(manifest_path).resolve(),
              "board_info": Path(board_info_path).resolve()}
    if journal_path:
        inputs["journal"] = Path(journal_path).resolve()
    raw = {key: path.read_bytes() for key, path in inputs.items()}
    source = _source(raw["source_image"], _json(raw["source_manifest"]))
    info = _json(raw["board_info"])
    identity, serial = _board(info, source, no_campaign)
    local = fleet = b""
    campaign = commit = 0
    lead = None
    targets = []
    manifest = None
    if no_campaign:
        _optional_state(info, 0, 0)
    else:
        campaign, commit, lead, targets, manifest = _campaign(raw["journal"], info, source, raw["source_image"])
        if source["image_class"] == "receiver":
            local = _local(campaign, commit, lead, manifest)
        else:
            fleet = _fleet(campaign, commit, targets, manifest)
    config = {"schema_version": 1, "mode": "transition_v2", "identity": identity, "usb_serial": serial,
              "image_class": source["image_class"], "role": 1 if source["image_class"] == "receiver" else 2,
              "version": "0.0.1+0", "no_campaign": no_campaign, "campaign_id": campaign, "commit_id": commit,
              "lead_identity": lead, "targets": targets, "campaign_manifest": manifest,
              "source_manifest": source, "board_info": info, "source_image_sha256": _sha(raw["source_image"]),
              "journal_sha256": _sha(raw["journal"]) if journal_path else None,
              "original_hash": source["mcuboot_image_hash"], "useful_capacity": 221184,
              "local_length": len(local), "fleet_length": len(fleet),
              "local_record_hex": local.hex(), "fleet_record_hex": fleet.hex(),
              "inputs": {key: str(path) for key, path in inputs.items()},
              "input_sha256": {key: _sha(value) for key, value in raw.items()}}
    config["token"] = _token(config)
    for key, path in inputs.items():
        require(path.read_bytes() == raw[key], "input changed while preparing transition: " + key)
    return config


def _bytes(value):
    return "{" + (", ".join("0x%02x" % byte for byte in bytes.fromhex(value)) if value else "0") + "}"


def render_header(config):
    require(config.get("mode") == "transition_v2" and config.get("token") == _token(config), "invalid transition config/token")
    fields = {"": "1", "EUI_BYTES": _bytes(config["identity"]),
              "ORIGINAL_HASH_BYTES": _bytes(config["original_hash"]), "TOKEN_BYTES": _bytes(config["token"]),
              "TOKEN_HEX": '"' + config["token"] + '"', "ROLE": str(config["role"]),
              "LOCAL_LENGTH": str(config["local_length"]), "FLEET_LENGTH": str(config["fleet_length"]),
              "LOCAL_BYTES": _bytes(config["local_record_hex"]), "FLEET_BYTES": _bytes(config["fleet_record_hex"]),
              "USEFUL_CAPACITY": str(config["useful_capacity"]) + "U"}
    return ("/* Generated exact terminal-v2 transition guards; never edit. */\n#pragma once\n#include <stdint.h>\n" +
            "".join("#define OWNTECH_OTA_TRANSITION%s %s\n" % (("_" + key) if key else "", value)
                    for key, value in fields.items())).encode("ascii")


def _replace(path, data):
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def generate(image_path, manifest_path, board_info_path, output_dir, *, journal_path=None, no_campaign=False):
    config = transition_config(image_path, manifest_path, board_info_path, journal_path=journal_path, no_campaign=no_campaign)
    header = render_header(config)
    config["header_sha256"] = _sha(header)
    config["build_id"] = "transition-" + config["header_sha256"][:20]
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    stem = output / "owntech_ota_recovery_config"
    _replace(stem.with_suffix(".h"), header)
    _replace(stem.with_suffix(".json"), (json.dumps(config, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    return config


def verify_config(config_path):
    """Re-derive every guard from archived inputs and verify the compiled header."""
    path = Path(config_path)
    original = path.read_bytes()
    config = _json(original)
    require(isinstance(config, dict) and config.get("mode") == "transition_v2", "not a transition configuration")
    inputs = config.get("inputs")
    require(isinstance(inputs, dict) and all(isinstance(inputs.get(key), str) for key in
            ("source_image", "source_manifest", "board_info")), "missing transition input provenance")
    expected = transition_config(inputs["source_image"], inputs["source_manifest"], inputs["board_info"],
                                 journal_path=inputs.get("journal"), no_campaign=config.get("no_campaign"))
    header = render_header(expected)
    expected["header_sha256"] = _sha(header)
    expected["build_id"] = "transition-" + expected["header_sha256"][:20]
    require(config == expected, "transition configuration differs from its saved inputs")
    require(path.with_suffix(".h").read_bytes() == header, "transition header differs from its configuration")
    require(path.read_bytes() == original, "transition configuration changed while verifying")
    return config


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--board-info", required=True, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--journal", type=Path)
    mode.add_argument("--no-campaign", action="store_true")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parents[2] / ".pio/ota-transition-config")
    args = parser.parse_args(argv)
    try:
        config = generate(args.image, args.manifest, args.board_info, args.output_dir,
                          journal_path=args.journal, no_campaign=args.no_campaign)
    except (OSError, ValueError) as error:
        print("Transition configuration refused: %s" % error, file=sys.stderr)
        return 1
    print("Prepared terminal-v2 transition for %s (%s), token %s; no device accessed" %
          (config["identity"], config["image_class"], config["token"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
