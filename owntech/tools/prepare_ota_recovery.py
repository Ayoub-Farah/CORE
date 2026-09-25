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


def _manifest(value, compact=False):
    require(isinstance(value, dict), "missing campaign manifest")
    if compact:
        require(value.get("schema_version") == 2 and value.get("protocol") == 2
                and value.get("format") == "mcuboot-compact" and value.get("activation_trailer") is False
                and value.get("image_class") == "receiver", "unsupported compact receiver manifest")
    else:
        require(value.get("schema_version") == 1 and value.get("protocol") == 1
                and value.get("format") == "mcuboot-padded" and value.get("activation_trailer") is True,
                "unsupported campaign manifest; v2 requires explicit --compact-receiver-only")
    profile = value.get("profile")
    require(isinstance(profile, dict) and all(field in profile for field in (
        "slot_size", "useful_capacity", "header_size", "hardware_id", "layout_id", "bootloader_id")),
        "incomplete campaign hardware profile")
    profile = validate_profile(profile)
    for field in ("hardware_id", "layout_id", "bootloader_id"):
        require(_uint(value.get(field), 32, field, 1) == profile[field], "manifest/profile " + field + " mismatch")
    require(_uint(value.get("artifact_size"), 32, "artifact size", 1) == (value.get("useful_size") if compact else profile["slot_size"]),
            "campaign artifact size differs from its explicit format")
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


def recovery_config(journal_path, *, staged_lead_only=False, prepared_follower_only=False,
                    compact_receiver_only=False, preprepare_receiver_only=False):
    """Extract immutable guards, requiring positive pre-COMMIT validation proof."""
    require(type(staged_lead_only) is bool, "staged-lead-only must be an explicit boolean mode")
    require(type(prepared_follower_only) is bool, "prepared-follower-only must be an explicit boolean mode")
    require(type(compact_receiver_only) is bool, "compact-receiver-only must be an explicit boolean mode")
    require(type(preprepare_receiver_only) is bool, "preprepare-receiver-only must be an explicit boolean mode")
    require(sum((staged_lead_only, prepared_follower_only, compact_receiver_only, preprepare_receiver_only)) <= 1,
            "recovery modes are mutually exclusive")
    early_failure = staged_lead_only or prepared_follower_only
    mode_name = "staged-lead-only" if staged_lead_only else "prepared-follower-only"
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
    if compact_receiver_only or preprepare_receiver_only:
        return _compact_config(path, raw, records, inventory, lead, serial, campaign,
                               preprepare_receiver_only=preprepare_receiver_only)
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

    def previous_success(row):
        """Only the exact successful baseline frozen by DISCOVER is history."""
        original = by_id.get(row.get("identity"))
        if not original or type(row.get("campaign")) is not int:
            return False
        if not 0 < row["campaign"] < 1 << 64 or row["campaign"] == campaign:
            return False
        fields = ("campaign", "state", "mcuboot_image_hash", "version", "build_id", "role",
                  "confirmed", "healthy", "available", "compatible", "offset", "image_size",
                  "validated", "flash_complete", "pass", "error", "event_mask", "event_ms", "event_order")
        if (row.get("state") != "SUCCESS" or row.get("error") != 0
                or any(field not in row or field not in original
                       or type(row[field]) is not type(original[field]) or row[field] != original[field]
                       for field in fields)):
            return False
        mask, times, order = row["event_mask"], row["event_ms"], row["event_order"]
        if (type(mask) is not int or not 0 <= mask < 1 << 12 or not isinstance(times, list)
                or not isinstance(order, list) or len(times) != 12 or len(order) != 12):
            return False
        present = [index for index in range(12) if mask & (1 << index)]
        if (any(type(value) is not int or not 0 <= value <= 0xFFFFFFFF for value in times)
                or any(type(value) is not int for value in order)
                or sorted(order[index] for index in present) != list(range(1, len(present) + 1))
                or any(times[index] or order[index] for index in range(12) if index not in present)):
            return False
        return True

    validated = False
    lead_staged = False
    committed = False
    starts = stages = 0
    for record in records:
        event = record.get("event")
        require(event not in ("SUCCESS", "REBOOTING", "POSTBOOT_CHECK") and record.get("device_event") not in (10, 11),
                "journal already records reboot/postboot/success; this recovery is forbidden")
        if early_failure:
            require(event not in ("CAN_TRANSFER_BEGIN", "CAN_TRANSFER_END", "ALL_VALIDATED")
                    and record.get("device_event") not in (4, 5, 9),
                    mode_name + " forbids CAN transfer or fleet validation progress")
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
            if early_failure:
                require(lead_staged, "START lacks a preceding complete Lead STAGED proof")
            starts += 1
        statuses = [record["status"]] if isinstance(record.get("status"), dict) else []
        if event == "STATUS_REJECTED":
            # Rejected snapshots are evidence of activation too, never a
            # source of positive STAGED/ALL_VALIDATED authorization.
            statuses.extend(value for value in (record.get("response"), record.get("rejected_row"))
                            if isinstance(value, dict))
            pages = record.get("page_responses", [])
            require(isinstance(pages, list), "invalid rejected status pages")
            statuses.extend(page["response"] for page in pages if isinstance(page, dict)
                            and isinstance(page.get("response"), dict))
        for status in statuses:
            status_rows = status.get("targets", [status] if "identity" in status else [])
            require(isinstance(status_rows, list), "invalid status target list")
            require(all(isinstance(row, dict) for row in status_rows), "invalid status target")
            historical = [previous_success(row) for row in status_rows]
            if ("identity" in status and "targets" not in status and historical == [True]
                    and status.get("phase", "SUCCESS") == "SUCCESS"):
                continue
            phase = status.get("phase", status.get("state"))
            if (not stages and phase == "SUCCESS" and status_rows and all(historical)
                    and all(status.get("campaign") == row["campaign"] for row in status_rows)):
                continue
            require(phase not in ("SUCCESS", "SUCCEEDED", "REBOOTING", "COMMITTED", "COMMITTING", "POSTBOOT_CHECK", "RECOVERY_REQUIRED"),
                    "journal records activation progress; this recovery is forbidden")
            if early_failure:
                require(phase not in ("BEGIN_PASS", "CAN_TRANSFER", "END_PASS", "ALL_VALIDATED"),
                        mode_name + " forbids CAN transfer or fleet validation progress")
            for row, prior in zip(status_rows, historical):
                if prior:
                    continue
                mask = _uint(row.get("event_mask", 0), 32, "device event mask")
                require(not mask & ((1 << 10) | (1 << 11))
                        and row.get("state") not in ("COMMITTED", "REBOOTING", "RECOVERY_REQUIRED", "SUCCESS", "SUCCEEDED"),
                        "a target already committed or rebooted; this recovery is forbidden")
                if early_failure:
                    require(not mask & ((1 << 4) | (1 << 5) | (1 << 9))
                            and row.get("state") not in ("PASS_OPEN", "PASS_CLOSED", "ALL_VALIDATED"),
                            mode_name + " forbids CAN transfer or fleet validation progress")
            if early_failure and event == "STATUS" and phase == "STAGED":
                require(stages == 1 and not starts and not committed,
                        "Lead STAGED proof must follow its stage request and precede START")
                require(status.get("campaign") == campaign and status.get("target_count") == len(targets)
                        and set(_roster([row.get("identity") for row in status_rows])) == set(targets),
                        "Lead STAGED proof has an incomplete or different roster")
                local = next(row for row in status_rows if identity(row["identity"]) == lead)
                require(local.get("campaign") == campaign and local.get("state") == "VALID"
                        and local.get("validated") is True and local.get("flash_complete") is True
                        and local.get("offset") == local.get("image_size") == manifest["artifact_size"]
                        and local.get("error", 0) == 0 and local.get("queue_depth") == 0
                        and status.get("error", 0) == 0,
                        "Lead did not durably validate the complete staged image")
                # This field identifies the running original, not the staged
                # image. The latter is bound by the exact USB_STAGE manifest,
                # nonzero campaign and complete durable validation above.
                require(local.get("mcuboot_image_hash") == by_id[lead]["mcuboot_image_hash"],
                        "Lead active image changed before START")
                lead_staged = True
            if phase == "ALL_VALIDATED" and event != "STATUS_REJECTED":
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
            require(not early_failure, mode_name + " forbids any COMMIT request")
            require(validated and not committed, "COMMIT lacks a preceding complete validation barrier or was repeated")
            committed = True
    if early_failure:
        require(stages == starts == 1 and lead_staged and not committed and records[-1].get("event") == "FAILED",
                mode_name + " requires one stage, one START, a complete Lead STAGED proof and final FAILED")
    else:
        require(stages == starts == 1 and validated and committed, "journal lacks the validated campaign and COMMIT request")
    config = {"schema_version": 1, "journal_path": str(path), "journal_sha256": hashlib.sha256(raw).hexdigest(),
            "campaign_id": campaign, "lead_identity": lead, "usb_serial": serial,
            "targets": originals, "manifest": manifest,
            "guards": {"local_journal_state": "VALID", "local_journal_state_value": 6,
                       "local_commit_id": 0, "match_campaign_id": True, "match_lead_eui": True,
                       "match_campaign_image_hash": True, "match_image_size": True,
                       "match_device_eui": True, "match_original_secondary_image_hash": True,
                       "host_all_validated_before_commit": True, "host_no_reboot_or_success": True}}
    if early_failure:
        repairs = [lead] if staged_lead_only else [target for target in targets if target != lead]
        require(bool(repairs), "prepared-follower-only requires a frozen follower")
        config.update(repair_targets=repairs)
        config["staged_lead_only" if staged_lead_only else "prepared_follower_only"] = True
        guards = config["guards"]
        for field in ("local_journal_state", "local_journal_state_value", "host_all_validated_before_commit"):
            del guards[field]
        guards.update(host_staged_lead_before_start=True, host_no_commit_request=True, host_final_failed=True)
        if staged_lead_only:
            guards.update(local_journal_states=["VALID", "ABORTED"], local_journal_state_values=[6, 10],
                          local_event_mask_allowed=[207, 463], fleet_journal_required=True,
                          fleet_journal_magic="OTA2", fleet_journal_states=["PREPARING", "FAILED"],
                          fleet_journal_state_values=[1, 9], fleet_commit_id=(campaign & 0xFFFFFFFF) or 1,
                          match_frozen_fleet_roster=True)
        else:
            guards.update(local_journal_states=["PREPARING", "READY"], local_journal_state_values=[1, 2],
                          local_event_mask_allowed=[1, 3], fleet_journal_absent=True,
                          host_initial_secondary_absent=True, match_nonlead_device_eui=True)
    return config


def _compact_config(path, raw, records, inventory, lead, serial, campaign, *, preprepare_receiver_only=False):
    targets = _roster(inventory.get("targets"))
    require(lead not in targets, "compact recovery roster must exclude the dedicated Lead")
    require(isinstance(serial, str) and re.fullmatch(r"[A-Za-z0-9_.\-]{1,128}", serial),
            "journal lacks a stable selected USB serial")
    manifest = _manifest(inventory.get("manifest"), compact=True)
    rows = inventory.get("inventory")
    require(isinstance(rows, list) and all(isinstance(row, dict) for row in rows), "missing original inventory")
    require(set(_roster([row.get("identity") for row in rows])) == set(targets), "original inventory differs from roster")
    addresses, originals = [], []
    for row in rows:
        require(row.get("role") == "follower" and all(row.get(key) is True for key in
                ("healthy", "confirmed", "available", "compatible")), "original receiver was not healthy and confirmed")
        addresses.append(_uint(row.get("address"), 8, "CAN address", 1))
        originals.append({"identity": identity(row["identity"]),
                          "original_active_hash": _digest(row.get("mcuboot_image_hash"), "original active hash")})
    require(len(set(addresses)) == len(addresses) and all(address < 254 for address in addresses), "duplicate/reserved CAN address")
    stages = starts = 0
    forbidden = {"COMMIT_REQUEST", "COMMITTED", "COMMITTING", "COMMIT_INTENT", "REBOOTING", "POSTBOOT_CHECK", "SUCCESS", "RECOVERY_REQUIRED"}
    for record in records:
        event = record.get("event")
        require(event not in forbidden and record.get("device_event") not in (10, 11),
                "compact recovery requires a campaign before any commit or reboot")
        if "manifest" in record:
            require(record["manifest"] == manifest, "campaign manifest changed within journal")
        if event == "PC_SOURCE_OPEN":
            require(not stages and not starts and identity(record.get("identity")) == lead, "unexpected source open")
            stages += 1
        if event == "START_REQUEST":
            require(stages == 1 and not starts and set(_roster(record.get("targets"))) == set(targets), "unexpected campaign start")
            starts += 1
        if stages:
            statuses = [record.get("status"), record.get("response"), record.get("rejected_row")]
            pages = record.get("page_responses", [])
            require(isinstance(pages, list), "invalid rejected status pages")
            statuses += [page.get("response") for page in pages if isinstance(page, dict)]
            for status in statuses:
                if not isinstance(status, dict):
                    continue
                require(status.get("phase", status.get("state")) not in forbidden, "snapshot already records activation progress")
                candidates = status.get("targets", [status] if "identity" in status else [])
                require(isinstance(candidates, list) and all(isinstance(row, dict) for row in candidates), "invalid status rows")
                for row in candidates:
                    require(row.get("state") not in forbidden and not _uint(row.get("event_mask", 0), 32, "event mask") & ((1 << 10) | (1 << 11)),
                            "target already records activation progress")
    require(stages == starts == 1 and records[-1].get("event") in ("FAILED", "ABORTED"),
            "compact recovery requires one source, one START and an explicit failed/aborted result")
    config = {"schema_version": 1, "compact_receiver_only": True, "journal_path": str(path),
            "journal_sha256": hashlib.sha256(raw).hexdigest(), "campaign_id": campaign,
            "lead_identity": lead, "usb_serial": serial, "targets": originals, "manifest": manifest,
            "guards": {"local_journal_magic": "OTL2", "local_journal_version": 2, "local_commit_id": 0,
                       "local_journal_state_values": [1, 2, 6, 9, 10], "maintenance_required": True,
                       "fleet_journal_absent": True, "match_campaign_id": True, "match_lead_eui": True,
                       "match_campaign_image_hash": True, "match_artifact_hash": True,
                       "match_image_size": True, "match_device_eui": True,
                       "host_no_commit_request": True, "host_no_reboot_or_success": True}}
    if preprepare_receiver_only:
        _preprepare_proof(records, targets, rows, manifest, campaign)
        config["preprepare_receiver_only"] = True
        config["guards"].update(local_journal_state_values=[9, 10], maintenance_required=False,
                               maintenance_absent_or_valid_false=True, host_preprepare_failed=True,
                               host_initial_secondary_absent_or_explicit_hash=True, host_no_transfer_or_validation=True)
    return config


def _preprepare_proof(records, targets, originals, manifest, campaign):
    """Only a legacy refusal before ERASE_END can use an inactive maintenance marker."""
    require(len(targets) == 1, "preprepare recovery requires exactly one receiver")
    original = originals[0]
    require(original.get("image_class") == "receiver" and original.get("state") == "IDLE"
            and type(original.get("campaign")) is int and original["campaign"] == 0
            and original.get("event_mask") == 0 and original.get("offset") == 0
            and original.get("pass") == 0 and original.get("validated") is False
            and original.get("flash_complete") is False,
            "preprepare recovery requires a clean idle original receiver")
    proof = False
    started = False
    forbidden = {"ERASE_END", "USB_TRANSFER_BEGIN", "USB_TRANSFER_END", "CAN_TRANSFER_BEGIN", "CAN_TRANSFER_END",
                 "FLASH_COMPLETE", "VERIFY_BEGIN", "VERIFY_END", "ALL_VALIDATED", "BEGIN_PASS", "CAN_TRANSFER",
                 "END_PASS", "PASS_OPEN", "PASS_CLOSED", "VALID", "READY", "COMMIT_REQUEST", "COMMIT_INTENT",
                 "COMMITTED", "COMMITTING", "REBOOTING", "POSTBOOT_CHECK", "SUCCESS", "RECOVERY_REQUIRED"}
    for record in records:
        event = record.get("event")
        require(event not in forbidden and record.get("device_event", 0) == 0,
                "preprepare recovery forbids preparation completion or transfer progress")
        if event == "START_REQUEST":
            started = True
        if not started:
            continue
        statuses = [record.get("status"), record.get("response"), record.get("rejected_row")]
        statuses += [page.get("response") for page in record.get("page_responses", []) if isinstance(page, dict)]
        for status in statuses:
            if not isinstance(status, dict):
                continue
            require(status.get("phase") not in forbidden and status.get("state") not in forbidden,
                    "preprepare snapshot already records preparation or transfer progress")
            for field in ("offset", "source_offset", "source_length"):
                require(field not in status or type(status[field]) is int and status[field] == 0,
                        "preprepare snapshot records source/transfer bytes")
            rows = status.get("targets", [status] if "identity" in status else [])
            for row in rows:
                require(row.get("identity") == targets[0] and row.get("state") in ("IDLE", "PREPARING", "FAILED", "ABORTED")
                        and not _uint(row.get("event_mask", 0), 32, "event mask") & ~1
                        and row.get("validated") is not True and row.get("flash_complete") is not True
                        and all(field not in row or type(row[field]) is int and row[field] == 0
                                for field in ("offset", "pass", "queue_depth")),
                        "preprepare target already records preparation or transfer progress")
                if event != "STATUS" or status.get("phase") != "FAILED" or row.get("state") == "ABORTED":
                    continue
                require(type(status.get("campaign")) is int and status["campaign"] == campaign
                        and status.get("target_count") == 1 and len(rows) == 1
                        and row.get("campaign") == campaign and row.get("state") == "FAILED"
                        and row.get("image_size") == manifest["artifact_size"]
                        and row.get("role") == "follower" and row.get("image_class") == "receiver"
                        and row.get("mcuboot_image_hash") == original["mcuboot_image_hash"]
                        and all(row.get(key) is True for key in ("healthy", "confirmed", "compatible"))
                        and row.get("available") is False and row.get("error") == -7
                        and row.get("event_mask") == 1 and row.get("event_order") == [1] + [0] * 11
                        and row.get("offset") == row.get("pass") == row.get("queue_depth") == 0
                        and row.get("validated") is False and row.get("flash_complete") is False,
                        "missing exact legacy preprepare FAILED proof")
                proof = True
    require(proof and records[-1].get("event") in ("FAILED", "ABORTED"),
            "preprepare recovery requires a positive failed STATUS proof")


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
    if config.get("compact_receiver_only") is True:
        fields["COMPACT_RECEIVER_ONLY"] = "1"
        fields["ARTIFACT_HASH_BYTES"] = _bytes(config["manifest"]["artifact_sha256"])
        fields["USEFUL_CAPACITY"] = str(config["manifest"]["profile"]["useful_capacity"]) + "U"
    if config.get("preprepare_receiver_only") is True:
        fields["PREPREPARE_RECEIVER_ONLY"] = "1"
    if config.get("staged_lead_only") is True:
        fields["STAGED_LEAD_ONLY"] = "1"
    if config.get("prepared_follower_only") is True:
        fields["PREPARED_FOLLOWER_ONLY"] = "1"
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


def generate(journal_path, output_dir, *, staged_lead_only=False, prepared_follower_only=False,
             compact_receiver_only=False, preprepare_receiver_only=False):
    config = recovery_config(journal_path, staged_lead_only=staged_lead_only,
                             prepared_follower_only=prepared_follower_only, compact_receiver_only=compact_receiver_only,
                             preprepare_receiver_only=preprepare_receiver_only)
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
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--staged-lead-only", action="store_true",
                      help="repair only a fully staged Lead after a failed START without any COMMIT")
    mode.add_argument("--prepared-follower-only", action="store_true",
                      help="repair only a prepared follower before CAN transfer; its secondary must be absent")
    mode.add_argument("--compact-receiver-only", action="store_true", help="guarded v2 receiver repair before any commit request")
    mode.add_argument("--preprepare-receiver-only", action="store_true",
                      help="one legacy v2 receiver that failed before maintenance or secondary preparation")
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).resolve().parents[2] / ".pio" / "ota-recovery-config")
    args = parser.parse_args(argv)
    try:
        config = generate(args.journal, args.output_dir, staged_lead_only=args.staged_lead_only,
                          prepared_follower_only=args.prepared_follower_only, compact_receiver_only=args.compact_receiver_only,
                          preprepare_receiver_only=args.preprepare_receiver_only)
    except (OSError, ValueError, CampaignError) as error:
        print("Recovery configuration refused: %s" % error, file=sys.stderr)
        return 1
    print("Prepared recovery configuration for campaign %016x, %d exact identities in %s; no device accessed"
          % (config["campaign_id"], len(config["targets"]), args.output_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
