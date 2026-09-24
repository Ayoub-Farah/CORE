#!/usr/bin/env python3
"""Initialize one USB board's OTA application using its existing bootloader."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

from lead_update import CampaignError, USBConnection, identity, json_value, prepare_manifest
from ota_artifact import load_profile
from smp_transport import ReceiverProbeTimeout, ProtocolError


def _verify_image(info, manifest):
    """Reject an unexpected live application before any role change or reset."""
    if info.get("service") != "owntech-ota" or info.get("protocol") != manifest["protocol"]:
        raise CampaignError("incompatible live receiver; no automatic bootloader recovery")
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


def provision(connection, image, manifest, mcumgr, timeout=30, clock=time.monotonic,
              sleep=time.sleep, output=print):
    """One explicit initialization; a live service is never reset or uploaded."""
    bootstrapped = False
    try:
        transport = connection.connect()
    except ReceiverProbeTimeout:
        output("No application receiver on USB serial %s; initializing through the existing bootloader" % connection.serial_number)
        transport = connection.bootstrap(image, mcumgr)
        bootstrapped = True
    except ProtocolError as error:
        raise CampaignError("USB replied but is not an accepted application receiver; no reset/upload was sent. "
                            "If it is already in MCUboot, use the established explicit recovery procedure: %s" % error) from error
    deadline = clock() + timeout
    expected_identity = None
    while True:
        info = transport.request("info", {})
        if info.get("service") != "owntech-ota" or info.get("protocol") != manifest["protocol"]:
            raise CampaignError("incompatible live receiver; no automatic bootloader recovery")
        # The USB service is available before the runtime publishes its first
        # identity/hash snapshot and completes the bounded CAN health check.
        if info.get("phase") == "BOOT":
            if clock() >= deadline:
                raise CampaignError("receiver initialization did not finish; inspect --status before any reset")
            sleep(0.25)
            continue
        _verify_image(info, manifest)
        actual_identity = identity(info["identity"])
        if expected_identity is not None and actual_identity != expected_identity:
            raise CampaignError("board identity changed during initialization")
        expected_identity = actual_identity
        if info.get("phase") != "IDLE":
            raise CampaignError("receiver is not idle (%s); stop and inspect --status before any reset" % info.get("phase"))
        if info.get("available") and info.get("active_confirmed") and info.get("slot_available"):
            break
        if clock() >= deadline:
            raise CampaignError("receiver is not healthy, confirmed and available; check the active CAN ACK peer "
                                "and --status before any reset (an unconfirmed image can roll back)")
        sleep(0.25)
    if info.get("role") not in ("lead", "follower"):
        raise CampaignError("receiver reported an invalid role")
    if info["role"] != "follower":
        transport.request("set_role", {"role": "follower"})
        info = transport.request("info", {})
        _verify_image(info, manifest)
        if (identity(info["identity"]) != expected_identity or info.get("role") != "follower"
                or info.get("phase") != "IDLE" or not info.get("available")
                or not info.get("active_confirmed") or not info.get("slot_available")):
            raise CampaignError("follower role/readiness could not be verified; inspect --status")
    result = {"result": "PROVISIONED" if bootstrapped else "ALREADY_INITIALIZED",
              "usb_serial": connection.serial_number, "info": info}
    output(json.dumps(result, default=json_value, indent=2, sort_keys=True))
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--serial", help="stable USB serial; required if several OwnTech boards are attached")
    parser.add_argument("--port", help="console interface for initial 1200-baud entry")
    parser.add_argument("--mcumgr", type=Path, required=True, help="existing OwnTech mcumgr application uploader")
    parser.add_argument("--version")
    parser.add_argument("--build-id")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args(argv)
    connection = None
    try:
        if args.timeout <= 0:
            raise CampaignError("timeout must be positive")
        _, manifest = prepare_manifest(args.image, profile=load_profile(args.profile) if args.profile else None,
                                       version=args.version, build_id=args.build_id)
        connection = USBConnection(args.serial, args.port, timeout=args.timeout)
        provision(connection, args.image, manifest, args.mcumgr, timeout=args.timeout)
        return 0
    except (OSError, ValueError, CampaignError, subprocess.SubprocessError) as error:
        print("OTA initialization failed: %s" % error, file=sys.stderr)
        return 1
    finally:
        if connection and connection.transport:
            connection.transport.close()


if __name__ == "__main__":
    raise SystemExit(main())
