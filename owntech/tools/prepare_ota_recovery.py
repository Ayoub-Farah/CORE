#!/usr/bin/env python3
"""Generate a campaign-bound recovery configuration; never open a device port."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile

from lead_update import CampaignError, identity, journal_campaign
from ota_artifact import ArtifactError, validate_profile


class RecoveryConfigError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise RecoveryConfigError(message)


def _object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def _uint(value, bits, name, minimum=0):
    require(type(value) is int and minimum <= value < 1 << bits, "invalid " + name)
    return value


def _digest(value, name):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None
            and value != "0" * 64, "missing or invalid " + name)
    return value


def _roster(values):
    require(isinstance(values, list) and 1 <= len(values) <= 16, "invalid bounded target roster")
    result = [identity(value) for value in values]
    require(len(set(result)) == len(result) and "0" * 16 not in result,
            "duplicate or zero target identity")
    return result


def _manifest(value):
    require(isinstance(value, dict), "missing campaign manifest")
    require(value.get("schema_version") == 1 and value.get("protocol") == 1
            and value.get("format") == "mcuboot-padded" and value.get("activation_trailer") is True,
            "unsupported campaign manifest")
    profile = value.get("profile")
    require(isinstance(profile, dict) and all(field in profile for field in (
        "slot_size", "useful_capacity", "header_size", "hardware_id", "layout_id", "bootloader_id")),
        "incomplete campaign hardware profile")
    profile = validate_profile(profile)
    for field in ("hardware_id", "layout_id", "bootloader_id"):
        require(_uint(value.get(field), 32, field, 1) == profile[field], "manifest/profile " + field + " mismatch")
    require(_uint(value.get("artifact_size"), 32, "artifact size", 1) == profile["slot_size"],
            "campaign must contain the exact padded slot image")
    require(profile["header_size"] < _uint(value.get("useful_size"), 32, "useful size", 1)
            <= profile["useful_capacity"], "campaign useful image exceeds its profile")
    _digest(value.get("artifact_sha256"), "artifact SHA256")
    _digest(value.get("mcuboot_image_hash"), "campaign MCUboot hash")
    version = value.get("version")
    require(isinstance(version, str) and re.fullmatch(r"\d+\.\d+\.\d+\+\d+", version) is not None,
            "invalid canonical MCUboot version")
    parts = [int(part) for part in re.split(r"[.+]", version)]
    require(all(part < limit for part, limit in zip(parts, (256, 256, 65536, 1 << 32))),
            "MCUboot version outside header bounds")
    build = value.get("build_id")
    require(isinstance(build, str) and re.fullmatch(r"[A-Za-z0-9_.+\-]{1,31}", build) is not None,
            "invalid campaign build identity")
    return value


def recovery_config(journal_path):
    """Extract immutable guards, requiring positive pre-COMMIT validation proof."""
    path = Path(journal_path).resolve()
    raw = path.read_bytes()
    require(raw and raw.endswith(b"\n"), "journal is empty or has an incomplete final record")
    records = [json.loads(line, object_pairs_hook=_object) for line in raw.decode("utf-8").splitlines()]
    require(all(isinstance(record, dict) for record in records), "journal records must be objects")
    # Keep the same frozen identity/serial semantics as campaign reconciliation.
    inventory, lead, serial = journal_campaign(path)
    require(path.read_bytes() == raw, "journal changed while preparing recovery; retry after capture stops")
    require(sum(record.get("event") == "DISCOVER" for record in records) == 1,
            "recovery requires exactly one frozen discovery")
    campaign = _uint(inventory.get("campaign"), 64, "campaign ID", 1)
    require(all(type(record.get("campaign")) is int and record["campaign"] == campaign for record in records),
            "mixed campaign IDs in journal")
    targets = _roster(inventory.get("targets"))
    require(lead in targets, "frozen roster omits its Lead")
    require(isinstance(serial, str) and re.fullmatch(r"[A-Za-z0-9_.\-]{1,128}", serial) is not None,
            "journal lacks a stable selected USB serial")
    manifest = _manifest(inventory.get("manifest"))
    rows = inventory.get("inventory")
    require(isinstance(rows, list) and all(isinstance(row, dict) for row in rows), "missing original inventory")
    require(set(_roster([row.get("identity") for row in rows])) == set(targets),
            "original inventory differs from frozen roster")
    by_id = {identity(row["identity"]): row for row in rows}
    addresses = []
    originals = []
    for target in targets:
        row = by_id[target]
        require(all(row.get(field) is True for field in ("healthy", "confirmed", "available", "compatible")),
                "original target was not healthy, confirmed and available: " + target)
        require(row.get("role") == ("lead" if target == lead else "follower"), "original target role mismatch")
        addresses.append(_uint(row.get("address"), 8, "CAN address", 1))
        originals.append({"identity": target,
                          "original_active_hash": _digest(row.get("mcuboot_image_hash"), "original active hash")})
    require(len(set(addresses)) == len(addresses) and all(address < 254 for address in addresses),
            "duplicate or reserved original CAN address")

    validated = False
    committed = False
    starts = stages = 0
    for record in records:
        event = record.get("event")
        require(event not in ("SUCCESS", "REBOOTING", "POSTBOOT_CHECK") and record.get("device_event") not in (10, 11),
                "journal already records reboot/postboot/success; this recovery is forbidden")
        if "manifest" in record:
            require(record["manifest"] == manifest, "campaign manifest changed within journal")
        if event in ("START_REQUEST", "COMMIT_REQUEST"):
            require(set(_roster(record.get("targets"))) == set(targets), "command target roster changed")
        if event == "USB_STAGE_REQUEST":
            require(not committed and not starts and not stages and identity(record.get("identity")) == lead,
                    "unexpected or repeated USB stage request")
            stages += 1
        if event == "START_REQUEST":
            require(stages == 1 and not starts and not committed, "unexpected or repeated campaign start")
            starts += 1
        status = record.get("status")
        if isinstance(status, dict):
            phase = status.get("phase", status.get("state"))
            require(phase not in ("SUCCESS", "SUCCEEDED", "REBOOTING", "COMMITTED", "COMMITTING", "POSTBOOT_CHECK", "RECOVERY_REQUIRED"),
                    "journal records activation progress; this recovery is forbidden")
            status_rows = status.get("targets", [status] if "identity" in status else [])
            require(isinstance(status_rows, list), "invalid status target list")
            for row in status_rows:
                require(isinstance(row, dict), "invalid status target")
                mask = _uint(row.get("event_mask", 0), 32, "device event mask")
                require(not mask & ((1 << 10) | (1 << 11))
                        and row.get("state") not in ("COMMITTED", "REBOOTING", "RECOVERY_REQUIRED", "SUCCESS", "SUCCEEDED"),
                        "a target already committed or rebooted; this recovery is forbidden")
            if phase == "ALL_VALIDATED":
                require(starts == 1 and not committed, "validation barrier must precede COMMIT")
                require(status.get("campaign") == campaign and status.get("target_count") == len(targets)
                        and set(_roster([row.get("identity") for row in status_rows])) == set(targets),
                        "validation barrier has an incomplete or different roster")
                require(all(row.get("campaign") == campaign and row.get("state") == "VALID"
                            and row.get("validated") is True and row.get("flash_complete") is True
                            and row.get("offset") == row.get("image_size") == manifest["artifact_size"]
                            and row.get("error", 0) == 0 and row.get("queue_depth") == 0
                            for row in status_rows), "not every frozen target durably validated the complete image")
                validated = True
        if event == "COMMIT_REQUEST":
            require(validated and not committed, "COMMIT lacks a preceding complete validation barrier or was repeated")
            committed = True
    require(stages == starts == 1 and validated and committed, "journal lacks the validated campaign and COMMIT request")
    return {"schema_version": 1, "journal_path": str(path), "journal_sha256": hashlib.sha256(raw).hexdigest(),
            "campaign_id": campaign, "lead_identity": lead, "usb_serial": serial,
            "targets": originals, "manifest": manifest,
            "guards": {"local_journal_state": "VALID", "local_journal_state_value": 6,
                       "local_commit_id": 0, "match_campaign_id": True, "match_lead_eui": True,
                       "match_campaign_image_hash": True, "match_image_size": True,
                       "match_device_eui": True, "match_original_secondary_image_hash": True,
                       "host_all_validated_before_commit": True, "host_no_reboot_or_success": True}}


def _bytes(value):
    return "{" + ", ".join("0x%02x" % byte for byte in bytes.fromhex(value)) + "}"


def render_header(config):
    targets = config["targets"]
    fields = {
        "CAMPAIGN_ID": "UINT64_C(0x%016x)" % config["campaign_id"],
        "LEAD_EUI_BYTES": _bytes(config["lead_identity"]),
        "IMAGE_HASH_BYTES": _bytes(config["manifest"]["mcuboot_image_hash"]),
        "IMAGE_SIZE": str(config["manifest"]["artifact_size"]) + "U",
        "TARGET_COUNT": str(len(targets)) + "U",
        "TARGET_EUIS": "{" + ", ".join(_bytes(target["identity"]) for target in targets) + "}",
        "ORIGINAL_HASHES": "{" + ", ".join(_bytes(target["original_active_hash"]) for target in targets) + "}",
        "JOURNAL_SHA256": '"' + config["journal_sha256"] + '"',
    }
    return ("/* Generated from a validated campaign journal; do not edit. */\n"
            "#ifndef OWNTECH_OTA_RECOVERY_CONFIG_H\n#define OWNTECH_OTA_RECOVERY_CONFIG_H\n"
            "#include <stdint.h>\n" + "".join("#define OWNTECH_OTA_RECOVERY_%s %s\n" % item for item in fields.items())
            + "#endif\n").encode("ascii")


def _replace(path, content):
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(content)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def generate(journal_path, output_dir):
    config = recovery_config(journal_path)
    header = render_header(config)
    config["header_sha256"] = hashlib.sha256(header).hexdigest()
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    stem = output / "owntech_ota_recovery_config"
    _replace(stem.with_suffix(".json"), (json.dumps(config, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    _replace(stem.with_suffix(".h"), header)
    return config


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--journal", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).resolve().parents[2] / ".pio" / "ota-recovery-config")
    args = parser.parse_args(argv)
    try:
        config = generate(args.journal, args.output_dir)
    except (OSError, ValueError, CampaignError) as error:
        print("Recovery configuration refused: %s" % error, file=sys.stderr)
        return 1
    print("Prepared recovery configuration for campaign %016x, %d exact identities in %s; no device accessed"
          % (config["campaign_id"], len(config["targets"]), args.output_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
