#!/usr/bin/env python3
"""Inspect, or explicitly apply, a guarded recovery image through the existing bootloader."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile

from lead_update import CampaignError, Journal, identity, select_port
from ota_artifact import inspect_usb_image
from prepare_ota_recovery import _digest, _object, recovery_config, render_header, require
from smp_transport import CommandError, ProtocolError, SerialSMP, TransportError
from bootloader_upload import upload_image, UploadError


def verify_inputs(config_path, image_path, manifest_path, target_identity, *, staged_lead_only=False,
                  prepared_follower_only=False, compact_receiver_only=False, preprepare_receiver_only=False):
    config_path = Path(config_path)
    config_bytes = config_path.read_bytes()
    config = json.loads(config_bytes, object_pairs_hook=_object)
    require(isinstance(config, dict) and isinstance(config.get("journal_path"), str), "invalid recovery configuration")
    expected = recovery_config(config["journal_path"], staged_lead_only=staged_lead_only,
                               prepared_follower_only=prepared_follower_only, compact_receiver_only=compact_receiver_only,
                               preprepare_receiver_only=preprepare_receiver_only)
    header = render_header(expected)
    expected["header_sha256"] = hashlib.sha256(header).hexdigest()
    require(config == expected, "recovery configuration no longer matches its source journal and guards")
    require(config_path.with_suffix(".h").read_bytes() == header, "recovery header/config provenance mismatch")
    target_identity = identity(target_identity)
    target = next((row for row in config["targets"] if row["identity"] == target_identity), None)
    require(target is not None, "requested identity is outside the frozen recovery roster")
    if staged_lead_only:
        require(target_identity in config["repair_targets"], "staged-lead-only forbids recovery of a follower")
    if prepared_follower_only:
        require(target_identity in config["repair_targets"], "prepared-follower-only forbids recovery of the Lead")
    manifest = json.loads(Path(manifest_path).read_bytes(), object_pairs_hook=_object)
    require(isinstance(manifest, dict) and isinstance(manifest.get("profile"), dict), "invalid recovery image manifest")
    version = "0.0.1+0"
    build_id = "recovery-" + config["header_sha256"][:20]
    require(manifest.get("version") == version and manifest.get("build_id") == build_id,
            "image manifest does not identify this dedicated recovery configuration")
    data = Path(image_path).read_bytes()
    actual = inspect_usb_image(data, manifest["profile"], version, build_id, manifest.get("image_class", "receiver"))
    for field in ("protocol", "format", "activation_trailer", "artifact_size", "useful_size", "artifact_sha256",
                  "mcuboot_image_hash", "version", "build_id", "hardware_id", "layout_id", "bootloader_id"):
        require(manifest.get(field) == actual[field], "recovery image bytes contradict manifest " + field)
    for field in ("slot_size", "useful_capacity", "header_size", "hardware_id", "layout_id", "bootloader_id"):
        require(manifest["profile"].get(field) == config["manifest"]["profile"][field],
                "recovery image differs from campaign hardware profile " + field)
    require(isinstance(manifest.get("signature"), dict)
            and manifest["signature"].get("key_sha256") == actual["signature"]["key_sha256"],
            "recovery signing key contradicts its manifest")
    campaign_key = config["manifest"].get("signature", {}).get("key_sha256")
    require(isinstance(campaign_key, str) and actual["signature"]["key_sha256"] == campaign_key,
            "recovery image must use the existing campaign signing key")
    return config, target, actual, data, hashlib.sha256(config_bytes).hexdigest()


def _slots(state):
    require(isinstance(state, dict) and isinstance(state.get("images"), list), "invalid bootloader image state")
    require(bool(state["images"]), "empty image list: the bootloader recognizes no image; "
            "primary/secondary state cannot be verified; no recovery action is authorized from this state")
    result = {}
    for row in state["images"]:
        require(isinstance(row, dict) and type(row.get("slot")) is int and row["slot"] in (0, 1),
                "unexpected image slot")
        require(row["slot"] not in result and row.get("image", 0) == 0, "duplicate slot or unexpected image number")
        require(isinstance(row.get("hash"), bytes) and len(row["hash"]) == 32
                and isinstance(row.get("version"), str), "missing exact image identity")
        require(all(type(row.get(field)) is bool for field in ("active", "confirmed", "pending")),
                "incomplete image activation flags")
        require(row.get("bootable", True) is True and row.get("permanent", False) is False,
                "unexpected nonbootable/permanent image state")
        result[row["slot"]] = row
    return result


def verify_slots(state, original_hash, secondary_hash=None, primary=None, *, secondary_pending=True):
    slots = _slots(state)
    require(set(slots) == ({0, 1} if secondary_hash else {0}), "unexpected occupied or absent secondary slot")
    active = slots[0]
    require(active["hash"].hex() == original_hash and active["active"] is True
            and active["confirmed"] is True and active["pending"] is False,
            "primary image is not the exact confirmed original; no recovery action allowed")
    if primary is not None:
        require(active == primary, "primary image state changed during recovery")
    if secondary_hash:
        secondary = slots[1]
        require(secondary["hash"].hex() == secondary_hash and secondary["pending"] is secondary_pending
                and secondary["active"] is False and secondary["confirmed"] is False,
                "secondary image is not the exact %s nonactive image" % ("pending" if secondary_pending else "nonpending"))
    return active


def recover(config_path, image_path, manifest_path, serial_number, target_identity, *, port=None,
            apply=False, after_revert=False, staged_lead_only=False, prepared_follower_only=False, compact_receiver_only=False,
            preprepare_receiver_only=False, expected_secondary_hash=None,
            mcumgr=None, log_path=None, timeout=10, enumerate_ports=None,
            transport_factory=None, uploader=None):
    require(isinstance(serial_number, str) and serial_number, "an explicit stable USB serial is required")
    require(type(after_revert) is bool, "after-revert must be an explicit boolean mode")
    require(not ((compact_receiver_only or preprepare_receiver_only) and after_revert), "compact receiver repair is precommit only")
    require(not (prepared_follower_only and after_revert), "prepared-follower-only is incompatible with after-revert")
    if expected_secondary_hash is not None:
        require(preprepare_receiver_only is True,
                "an explicit secondary hash is allowed only for preprepare-receiver-only recovery")
        _digest(expected_secondary_hash, "expected secondary hash")
    require(isinstance(timeout, (int, float)) and not isinstance(timeout, bool)
            and math.isfinite(timeout) and 0 < timeout <= 60, "SMP timeout must be within 0..60 seconds")
    config, target, image, data, config_hash = verify_inputs(config_path, image_path, manifest_path, target_identity,
                                                          staged_lead_only=staged_lead_only,
                                                          prepared_follower_only=prepared_follower_only, compact_receiver_only=compact_receiver_only,
                                                          preprepare_receiver_only=preprepare_receiver_only)
    require(expected_secondary_hash != target["original_active_hash"],
            "expected secondary hash must differ from the confirmed original primary")
    if apply:
        require(mcumgr is not None and Path(mcumgr).is_file(), "apply requires the existing mcumgr executable")
    if enumerate_ports is None:
        from serial.tools.list_ports import comports
        enumerate_ports = comports
    transport_factory = transport_factory or SerialSMP
    uploader = uploader or upload_image
    selected = select_port(enumerate_ports(), serial_number, port)
    device = selected.device

    def check_port():
        current = select_port(enumerate_ports(), serial_number, device)
        require(current.device == device, "selected USB interface changed; inspect again")

    target_identity = target["identity"]
    if log_path is None:
        log_path = Path(__file__).resolve().parents[2] / ".pio" / (
            "ota-recovery-%016x-%s.jsonl" % (config["campaign_id"], target_identity))
    journal = Journal(log_path, config["campaign_id"])
    transport = None
    try:
        journal.emit("RECOVERY_INPUT", target_identity, apply=apply, after_revert=after_revert,
                     staged_lead_only=staged_lead_only,
                     prepared_follower_only=prepared_follower_only, compact_receiver_only=compact_receiver_only,
                     preprepare_receiver_only=preprepare_receiver_only,
                     expected_secondary_hash=expected_secondary_hash,
                     usb_serial=serial_number, port=device,
                     config_sha256=config_hash, source_journal_sha256=config["journal_sha256"],
                     original_active_hash=target["original_active_hash"],
                     campaign_image_hash=config["manifest"]["mcuboot_image_hash"], recovery_image=image)

        def open_bootloader():
            nonlocal transport
            check_port()
            transport = transport_factory(device, timeout=timeout)
            try:
                transport.request("info")
            except CommandError as error:
                require(error.unsupported, "bootloader service proof must be exactly unsupported OTA group rc=8")
            else:
                raise CampaignError("live OTA application replied; enter the existing bootloader physically, no reset sent")

        def state(event):
            value = transport.image_state()
            journal.emit(event, target_identity, image_state=value)
            return value

        open_bootloader()
        initial = state("RECOVERY_INSPECT")
        has_secondary = 1 in _slots(initial)
        secondary = None if prepared_follower_only or preprepare_receiver_only or (compact_receiver_only and not has_secondary) else config["manifest"]["mcuboot_image_hash"]
        if preprepare_receiver_only:
            secondary = expected_secondary_hash
        primary = verify_slots(initial, target["original_active_hash"], secondary,
                               secondary_pending=False if compact_receiver_only or preprepare_receiver_only else not after_revert)
        if not apply:
            journal.emit("RECOVERY_INSPECTION_PASSED", target_identity)
            return {"result": "INSPECTED", "identity": target_identity, "usb_serial": serial_number,
                    "port": device, "log": str(log_path)}

        erase_secondary = (expected_secondary_hash is not None or
                           (not prepared_follower_only and not preprepare_receiver_only and
                            (not compact_receiver_only or has_secondary)))
        if erase_secondary:
            check_port()
            if expected_secondary_hash is not None:
                # An unrelated old USB backup is authorized only by its exact
                # explicitly supplied hash, freshly reread before the erase.
                verify_slots(state("RECOVERY_BEFORE_ERASE"), target["original_active_hash"],
                             expected_secondary_hash, primary, secondary_pending=False)
            journal.emit("RECOVERY_ERASE_SECONDARY_REQUEST", target_identity, slot=1,
                         expected_secondary_hash=secondary)
            transport._request(2, 1, 5, "erase recovery secondary slot", {"slot": 1})
            verify_slots(state("RECOVERY_AFTER_ERASE"), target["original_active_hash"], primary=primary)
        transport.close()
        transport = None
        check_port()
        # Upload a private snapshot of the bytes inspected before the first
        # mutation, even if another build replaces the original artifact path.
        with tempfile.TemporaryDirectory(prefix="owntech-ota-recovery-") as temporary:
            snapshot = Path(temporary) / "firmware.mcuboot.bin"
            snapshot.write_bytes(data)
            base = [str(mcumgr), "--conntype", "serial", "--connstring",
                    "dev=%s,baud=115200,mtu=128" % device, "--timeout", "10", "--tries", "1"]
            journal.emit("RECOVERY_UPLOAD_REQUEST", target_identity, artifact_sha256=image["artifact_sha256"])
            uploader(base, snapshot)

        open_bootloader()
        after = state("RECOVERY_AFTER_UPLOAD")
        verify_slots(after, target["original_active_hash"], image["mcuboot_image_hash"], primary)
        version = _slots(after)[1]["version"]
        require(version + ("+0" if "+" not in version else "") == image["version"],
                "uploaded recovery version mismatch")
        check_port()
        journal.emit("RECOVERY_RESET_REQUEST", target_identity)
        # Exactly one reset, only after a fresh bootloader read proves the new
        # secondary and unchanged confirmed primary. Never confirm an app here.
        transport._request(2, 0, 5, "boot verified recovery image", {})
        journal.emit("RECOVERY_RESET_ACCEPTED", target_identity)
        return {"result": "RESET_REQUESTED", "identity": target_identity, "usb_serial": serial_number,
                "port": device, "log": str(log_path)}
    except Exception as error:
        journal.emit("RECOVERY_STOPPED", target_identity, error=str(error))
        raise
    finally:
        if transport:
            transport.close()
        journal.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--port")
    parser.add_argument("--mcumgr", type=Path)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--expected-secondary-hash",
                        help="preprepare recovery only: exact nonpending old secondary hash read during inspection")
    parser.add_argument("--after-revert", action="store_true",
                        help="inspect a completed revert: require the exact initial secondary to be nonpending; never initiate a revert")
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--staged-lead-only", action="store_true",
                       help="require the explicit staged-Lead configuration; followers are forbidden")
    scope.add_argument("--prepared-follower-only", action="store_true",
                       help="require a prepared follower with an absent secondary; Lead and after-revert are forbidden")
    scope.add_argument("--compact-receiver-only", action="store_true", help="explicit precommit v2 receiver repair; never an armed image")
    scope.add_argument("--preprepare-receiver-only", action="store_true",
                       help="one failed v2 receiver before preparation; secondary absent unless its exact hash is supplied")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--inspect", action="store_true", help="read-only slot inspection (default)")
    mode.add_argument("--apply", action="store_true", help="erase only the proven secondary, upload recovery and reset once")
    args = parser.parse_args(argv)
    try:
        result = recover(args.config, args.image, args.manifest, args.serial, args.identity,
                         port=args.port, apply=args.apply, after_revert=args.after_revert,
                         staged_lead_only=args.staged_lead_only,
                         prepared_follower_only=args.prepared_follower_only, compact_receiver_only=args.compact_receiver_only,
                         preprepare_receiver_only=args.preprepare_receiver_only,
                         expected_secondary_hash=args.expected_secondary_hash,
                         mcumgr=args.mcumgr, log_path=args.log, timeout=args.timeout)
    except (OSError, ValueError, CampaignError, UploadError) as error:
        print("Recovery stopped: %s" % error, file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
