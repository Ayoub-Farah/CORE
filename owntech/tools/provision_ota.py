#!/usr/bin/env python3
"""Initialize one USB board's OTA application using its existing bootloader."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import tempfile

from lead_update import CampaignError, USBConnection, ReceiverStatus, identity, json_value, prepare_manifest
from bootloader_upload import UploadError
from ota_artifact import load_profile
from smp_transport import ReceiverProbeTimeout, ProtocolError, CommandError


def _verify_image(info, manifest):
    """Reject an unexpected live application before any role change or reset."""
    if info.get("service") != "owntech-ota" or info.get("protocol") != manifest["protocol"]:
        raise CampaignError("incompatible live receiver; no automatic bootloader recovery")
    if info.get("image_class") != manifest["image_class"]:
        raise CampaignError("live image class differs from requested installation; no write authorized")
    identity(info.get("identity"))
    for field in ("hardware_id", "layout_id", "bootloader_id"):
        if info.get(field) != manifest[field]:
            raise CampaignError("receiver %s does not match the artifact profile" % field)
    if (info.get("slot_size") != manifest["profile"]["slot_size"]
            or info.get("useful_capacity", 0) < manifest["useful_size"]):
        raise CampaignError("receiver capacity does not match the signed artifact")
    image_hash = info.get("mcuboot_image_hash")
    if isinstance(image_hash, bytes):
        image_hash = image_hash.hex()
    version = info.get("version", "")
    if not isinstance(version, str):
        raise CampaignError("receiver reported an invalid application version")
    if "+" not in version:
        version += "+0"
    if (version != manifest["version"] or info.get("build_id") != manifest["build_id"]
            or image_hash != manifest["mcuboot_image_hash"]):
        raise CampaignError("a different OTA application is already running; use USB_LEAD lead_update to update it")


def _provision_readiness(info):
    """Local initialization may finish before a CAN peer joins the bus."""
    if info.get("phase") == "WAITING_CAN":
        expected = {"local_healthy": True, "healthy": False, "active_confirmed": True,
                    "slot_available": True, "available": False, "can_ready": False, "error": 0}
        if any(type(info.get(field)) is not type(value) or info[field] != value
               for field, value in expected.items()):
            raise CampaignError("WAITING_CAN receiver lacks coherent local health, confirmation or free slot; "
                                "inspect --status before any reset")
        return "WAITING_FOR_PEER"
    if info.get("phase") != "IDLE":
        return None
    # Older IDLE receivers do not expose these diagnostics. If present, each
    # must agree with the fully ready state before initialization can succeed.
    for field, value in (("local_healthy", True), ("healthy", True), ("can_ready", True), ("error", 0)):
        if field in info and (type(info[field]) is not type(value) or info[field] != value):
            raise CampaignError("IDLE receiver reports inconsistent local/CAN health (%s=%s); "
                                "inspect --status before any reset" % (field, info[field]))
    if all(info.get(field) is True for field in ("available", "active_confirmed", "slot_available")):
        return "READY"
    return None


def provision(connection, image, manifest, mcumgr, timeout=30, clock=time.monotonic,
              sleep=time.sleep, output=print, legacy_console=False, bootloader=False):
    """Initialize one board; ordinary uploads first preserve a live receiver."""
    bootstrapped = False
    if bootloader:
        transport = connection.bootstrap(image, mcumgr, enter_bootloader=False)
        bootstrapped = True
    elif legacy_console:
        # An SMP probe itself can overflow a legacy application's 16-byte
        # console buffer and block its USB workqueue before 1200-baud entry.
        # This explicit mode therefore checks topology without UART writes.
        # Count every interface of the selected physical board, even when
        # --port selected one interface of a current double-CDC OTA receiver.
        ports = [port for port in connection.enumerate()
                 if port.vid == 0x2FE3 and port.serial_number == connection.serial_number]
        if len(ports) != 1 or (connection.bootstrap_port and ports[0].device != connection.bootstrap_port):
            raise CampaignError("legacy-console initialization requires exactly one CDC interface on the selected "
                                "USB board; use ordinary OTA upload for a live OTA receiver. No reset/upload was sent")
        connection.bootstrap_port = ports[0].device
        # A minimal receiver also has one CDC. Safe line-coding status must
        # distinguish it from the operator-declared legacy console before a
        # reset: otherwise OTA -> USB_LEAD ota_init could cross-flash classes.
        candidate = ReceiverStatus(ports[0].device)
        try:
            info = candidate.request("info")
        except ReceiverProbeTimeout:
            candidate.close()
            output("Explicit legacy-console initialization on USB serial %s; entering at 1200 baud before any SMP probe"
                   % connection.serial_number)
            transport = connection.bootstrap(image, mcumgr)
            bootstrapped = True
        except Exception:
            candidate.close()
            raise
        else:
            try:
                _verify_image(info, manifest)
            except Exception:
                candidate.close()
                raise
            connection.transport = transport = candidate
    else:
        try:
            transport = connection.connect()
        except ReceiverProbeTimeout:
            raise CampaignError("unknown single-CDC application: use explicit --legacy-console for 1200-baud entry, "
                                "or --bootloader after BOOT + RESET; no SMP probe/reset/upload sent")
        except CommandError as error:
            if not error.unsupported:
                raise CampaignError("USB command rejected; no reset/upload was sent: %s" % error) from error
            output("OTA service unsupported on USB serial %s; checking the existing image service" % connection.serial_number)
            transport = connection.bootstrap(image, mcumgr, enter_bootloader=False)
            bootstrapped = True
        except ProtocolError as error:
            raise CampaignError("USB replied but the response is invalid; no reset/upload was sent: %s" % error) from error
    deadline = clock() + timeout
    expected_identity = None
    while True:
        info = transport.request("info", {})
        if info.get("service") != "owntech-ota" or info.get("protocol") != manifest["protocol"]:
            raise CampaignError("incompatible live receiver; no automatic bootloader recovery")
        # The USB service is available before the runtime publishes its first
        # identity/hash snapshot and completes the local startup health check.
        if info.get("phase") == "BOOT":
            if clock() >= deadline:
                raise CampaignError("receiver initialization did not finish; inspect --status before any reset")
            sleep(0.25)
            continue
        if info.get("phase") not in ("IDLE", "WAITING_CAN"):
            message = "receiver is not idle (%s)" % info.get("phase")
            if info.get("phase") == "FAILED":
                # A failed startup can leave the active hash unpublished. Report
                # the failure before interpreting that hash as another image.
                message += "; startup health failed"
                details = ["%s=%s" % (field, info[field]) for field in
                           ("error", "local_healthy", "healthy", "can_ready", "active_confirmed", "slot_available")
                           if field in info]
                if details:
                    message += " (" + ", ".join(details) + ")"
                if info.get("error") == -17:
                    message += "; check CAN wiring, termination and an active ACK-capable peer"
            raise CampaignError(message + "; stop and inspect --status before any reset "
                                "(an unconfirmed image can roll back)")
        _verify_image(info, manifest)
        actual_identity = identity(info["identity"])
        if expected_identity is not None and actual_identity != expected_identity:
            raise CampaignError("board identity changed during initialization")
        expected_identity = actual_identity
        can_status = _provision_readiness(info)
        if can_status is not None:
            break
        if clock() >= deadline:
            raise CampaignError("receiver is not healthy, confirmed and available; check the active CAN ACK peer "
                                "and --status before any reset (an unconfirmed image can roll back)")
        sleep(0.25)
    expected_role = "lead" if manifest["image_class"] == "lead" else "follower"
    if info.get("role") != expected_role:
        raise CampaignError("compiled image class/role mismatch")
    result = {"result": "PROVISIONED" if bootstrapped else "ALREADY_INITIALIZED",
              "usb_serial": connection.serial_number, "can_status": can_status, "info": info}
    output(json.dumps(result, default=json_value, indent=2, sort_keys=True))
    return result


def main(argv=None, *, raise_errors=False):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--serial", help="stable USB serial; required if several OwnTech boards are attached")
    parser.add_argument("--port", help="console interface for initial 1200-baud entry")
    parser.add_argument("--legacy-console", action="store_true",
                        help="explicitly initialize a legacy single-CDC application: enter at 1200 baud before any SMP probe")
    parser.add_argument("--mcumgr", type=Path, required=True, help="existing OwnTech mcumgr application uploader")
    parser.add_argument("--image-class", required=True, choices=("receiver", "lead"))
    parser.add_argument("--bootloader", action="store_true", help="explicit BOOT + RESET entry already performed")
    parser.add_argument("--version")
    parser.add_argument("--build-id")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args(argv)
    connection = None
    try:
        if args.timeout <= 0:
            raise CampaignError("timeout must be positive")
        if args.bootloader and args.legacy_console:
            raise CampaignError("choose either explicit bootloader or legacy console entry")
        artifact, manifest = prepare_manifest(args.image, profile=load_profile(args.profile) if args.profile else None,
                                              version=args.version, build_id=args.build_id, image_class=args.image_class, usb=True)
        # Keep the inspected, class-bound bytes immutable while USB enumeration
        # and bootloader checks run. A concurrent build cannot replace them.
        with tempfile.TemporaryDirectory(prefix="owntech-ota-install-") as temporary:
            image = Path(temporary) / args.image.name
            image.write_bytes(artifact)
            connection = USBConnection(args.serial, args.port, timeout=args.timeout)
            provision(connection, image, manifest, args.mcumgr, timeout=args.timeout,
                      legacy_console=args.legacy_console, bootloader=args.bootloader)
        return 0
    except (OSError, ValueError, CampaignError, UploadError, subprocess.SubprocessError) as error:
        if raise_errors:
            raise  # The desktop assistant displays the actual failure.
        print("OTA initialization failed: %s" % error, file=sys.stderr)
        return 1
    finally:
        if connection and connection.transport:
            connection.transport.close()


if __name__ == "__main__":
    raise SystemExit(main())
