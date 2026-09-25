"""Guard the physical USB roundtrip with simulated SMP and exact artifacts."""
import copy
import hashlib
import io
import json
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
sys.path.insert(0, str(ROOT / "owntech/scripts"))
from test_pc_artifact import artifact
from ota_artifact import inspect_usb_image
from usb_artifact import inspect_plain_usb
from prepare_ota_transition import generate
from lead_update import CampaignError
from smp_transport import CommandError, TransportError
from bootloader_upload import UploadError
import transition_ota_usb as transition


def slot(number, manifest, *, active=False, confirmed=False, pending=False):
    return {"slot": number, "version": manifest["version"],
            "hash": bytes.fromhex(manifest["mcuboot_image_hash"]),
            "active": active, "confirmed": confirmed, "pending": pending,
            "bootable": True, "permanent": False}


class Device:
    def __init__(self, primary):
        self.images = [slot(0, primary, active=True, confirmed=True)]
        self.calls = []
        self.info_reply = None
        self.info_error = None
        self.reset_error = None
        self.erase_error = None
        self.before_state = None
        self.wire_flags = False
        self.after_erase = None

    def request(self, command):
        self.calls.append((command,))
        if self.info_error:
            raise self.info_error
        if self.info_reply is not None:
            return copy.deepcopy(self.info_reply)
        raise CommandError("unsupported", response={"rc": 8})

    def image_state(self):
        self.calls.append(("image_state",))
        if self.before_state:
            self.before_state()
        images = copy.deepcopy(self.images)
        if self.wire_flags:
            # MCUboot boot_serial.c (CONFIG_BOOT_SERIAL_IMG_GRP_IMAGE_STATE)
            # emits true flags only; TEST omits even primary 'active'.
            pending = any(row.get("slot") == 1 and row.get("pending") for row in images)
            for row in images:
                if pending and row.get("slot") == 0:
                    row.pop("active", None)
                for key in ("active", "confirmed", "pending", "permanent"):
                    if row.get(key) is False:
                        del row[key]
        return {"images": images}

    def _request(self, op, group, command, label, payload):
        self.calls.append((op, group, command, payload))
        if (op, group, command) == (2, 1, 5):
            if self.erase_error:
                raise self.erase_error
            self.images = self.images[:1]
            if self.after_erase:
                self.after_erase()
        elif (op, group, command) == (2, 0, 5):
            if self.reset_error:
                raise self.reset_error
        else:
            raise AssertionError("unexpected mutating request")
        return {"rc": 0}

    def close(self):
        self.calls.append(("close",))


class TransitionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source_data = artifact()
        self.source = inspect_usb_image(self.source_data, build_id="source-v2")
        self.info = {"service": "owntech-ota", "protocol": 2, "identity": "0011223344556677",
                     "usb_serial": "selected", "capture_id": "a" * 32, "image_class": "receiver",
                     "role": "follower", "phase": "IDLE", "maintenance": False,
                     "healthy": True, "local_healthy": True, "can_ready": True, "error": 0,
                     "available": True, "active_confirmed": True, "slot_available": True,
                     "slot_size": self.source["profile"]["slot_size"], "useful_capacity": 221184,
                     **{key: self.source[key] for key in ("version", "build_id", "hardware_id",
                        "layout_id", "bootloader_id", "mcuboot_image_hash")}}
        self.source_path = self.root / "source.bin"
        self.source_manifest_path = self.root / "source.json"
        self.info_path = self.root / "board.json"
        self.source_path.write_bytes(self.source_data)
        self.write_json(self.source_manifest_path, self.source)
        self.write_json(self.info_path, self.info)
        self.config = generate(self.source_path, self.source_manifest_path, self.info_path,
                               self.root / "config", no_campaign=True)
        self.config_path = self.root / "config/owntech_ota_recovery_config.json"
        helper = bytearray(artifact(image_class=None))
        struct.pack_into("<BBHI", helper, 20, 0, 0, 1, 0)
        helper[648:680] = hashlib.sha256(helper[:640]).digest()
        self.helper_data = bytes(helper)
        self.helper = inspect_usb_image(self.helper_data,
                                       build_id="transition-" + self.config["header_sha256"][:20])
        self.helper_path = self.root / "helper.bin"
        self.helper_manifest_path = self.root / "helper.json"
        self.helper_path.write_bytes(self.helper_data)
        self.write_json(self.helper_manifest_path, self.helper)
        self.usb_data = artifact(body_size=160, image_class=None)
        self.usb = inspect_plain_usb(self.usb_data, build_id="usb-" + "ab" * 10)
        self.usb["build_proof"] = {"ota_enabled": False, "recovery_enabled": False,
                                  "auto_confirmation": True, "elf_sha256": "ab" * 32,
                                  "config_sha256": "cd" * 32}
        self.usb_path, self.usb_manifest_path = self.root / "usb.bin", self.root / "usb.json"
        self.usb_path.write_bytes(self.usb_data)
        self.write_json(self.usb_manifest_path, self.usb)
        self.receipt = self.root / "receipt.json"
        self.write_json(self.receipt, transition.receipt_payload(self.config, self.helper, self.completion()))
        self.mcumgr = self.root / "mcumgr.exe"
        self.mcumgr.write_bytes(b"mock only; never execute")
        self.log = self.root / "transition.jsonl"
        self.port = SimpleNamespace(vid=0x2FE3, serial_number="selected", device="COM7")
        self.enumerate = Mock(return_value=[self.port])
        self.device = Device(self.source)
        self.factory = Mock(side_effect=lambda *args, **kwargs: self.device)
        self.candidate, self.candidate_data = self.helper, self.helper_data
        self.uploader = Mock(side_effect=self.upload)

    def write_json(self, path, value):
        path.write_text(json.dumps(value), encoding="utf-8")

    def completion(self, rc=0, confirmed=1):
        return ("OTA_TRANSITION rc=%d EUI=%s token=%s confirmed=%d; outputs inhibited\n" %
                (rc, self.config["identity"], self.config["token"], confirmed)).encode("ascii")

    def upload(self, base, snapshot):
        self.assertEqual(snapshot.read_bytes(), self.candidate_data)
        self.assertNotEqual(snapshot, self.helper_path)
        self.assertEqual(base, [str(self.mcumgr), "--conntype", "serial", "--connstring",
                               "dev=COM7,baud=115200,mtu=128", "--timeout", "10", "--tries", "1"])
        self.assertEqual(self.device.calls[-1], ("close",))
        self.device.images.append(slot(1, self.candidate, pending=True))

    def run_step(self, stage="install-helper", **options):
        kwargs = dict(image=self.candidate, data=self.candidate_data, usb=self.usb,
                      receipt=self.receipt, mcumgr=self.mcumgr, log_path=self.log,
                      enumerate_ports=self.enumerate, transport_factory=self.factory, uploader=self.uploader)
        if stage == "verify-usb":
            kwargs.update(image=None, data=None)
        kwargs.update(options)
        return transition.bootloader_step(self.config, self.helper, stage, **kwargs)

    def mutations(self):
        return [call for call in self.device.calls if call[0] == 2]

    def events(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def record_upload(self, token=None, image_hash=None, *, secondary_ready=True):
        events = ["TRANSITION_UPLOAD_REQUEST"]
        if secondary_ready:
            events.insert(0, "TRANSITION_SECONDARY_READY")
        self.log.write_text("".join(json.dumps({"token": token or self.config["token"], "event": event,
                                               "image_hash": image_hash or self.candidate["mcuboot_image_hash"]}) + "\n"
                                    for event in events))

    def test_capture_is_read_only_binds_serial_and_adds_fresh_operation_nonce(self):
        transport = Mock(request=Mock(return_value=self.info))
        factory = Mock(return_value=SimpleNamespace(connect=Mock(return_value=transport)))
        output = self.root / "captured.json"
        transition.capture("selected", output, port="COM7", connection_factory=factory)
        first = json.loads(output.read_bytes())
        transition.capture("selected", output, port="COM7", connection_factory=factory)
        second = json.loads(output.read_bytes())
        self.assertEqual(first["usb_serial"], "selected")
        self.assertEqual(first["identity"], self.info["identity"])
        self.assertNotEqual(first["capture_id"], second["capture_id"])
        self.assertEqual(len(first["capture_id"]), 32)
        self.assertEqual(transport.method_calls, [("request", ("info",), {}), ("close", (), {}),
                                                ("request", ("info",), {}), ("close", (), {})])
        factory.assert_called_with("selected", "COM7")

    def test_capture_rejects_non_v2_and_closes_without_writing(self):
        transport = Mock(request=Mock(return_value=dict(self.info, protocol=1)))
        factory = Mock(return_value=SimpleNamespace(connect=Mock(return_value=transport)))
        output = self.root / "rejected.json"
        with self.assertRaises(ValueError):
            transition.capture("selected", output, connection_factory=factory)
        transport.close.assert_called_once()
        self.assertFalse(output.exists())

    def test_inspection_default_is_read_only(self):
        self.assertEqual(self.run_step()["result"], "INSPECTED")
        self.assertEqual(self.device.calls, [("info",), ("image_state",), ("close",)])
        self.assertEqual(self.mutations(), [])
        self.uploader.assert_not_called()

    def test_stock_mcuboot_without_secondary_erase_never_uploads_even_empty_slot(self):
        # Stock MCUboot boot_serial can upload directly into primary. Its
        # missing group-1 command-5 handler must refuse before any upload,
        # including when image list initially has no secondary image.
        self.device.erase_error = CommandError("unsupported image erase", response={"rc": 8})
        with self.assertRaises(ValueError):
            self.run_step(apply=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])
        self.assertFalse(any(row["event"] == "TRANSITION_SECONDARY_READY" for row in self.events()))
        self.uploader.assert_not_called()

    def test_secondary_erase_readback_requires_unchanged_primary_and_empty_slot(self):
        for change in (lambda: self.device.images[0].update(version="different"),
                       lambda: self.device.images.append(slot(1, self.usb))):
            self.device = Device(self.source)
            self.device.after_erase = change
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.run_step(apply=True)
            self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])
        self.assertFalse(any(row["event"] == "TRANSITION_SECONDARY_READY" for row in self.events()))
        self.uploader.assert_not_called()

    def test_apply_uploads_snapshot_then_checks_pending_candidate_before_reset(self):
        result = self.run_step(apply=True)
        self.assertEqual(result["result"], "RESET_REQUESTED")
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])
        self.uploader.assert_called_once()
        self.assertEqual(self.factory.call_count, 2)
        events = [row["event"] for row in self.events()]
        self.assertLess(events.index("TRANSITION_SECONDARY_READY"), events.index("TRANSITION_UPLOAD_REQUEST"))
        self.assertLess(events.index("TRANSITION_AFTER_UPLOAD"), events.index("TRANSITION_RESET_REQUEST"))

    def test_actual_mcuboot_true_only_flags_allow_none_to_test_upload(self):
        self.device.wire_flags = True
        self.device.images.append(slot(1, self.usb))
        self.assertEqual(self.run_step(apply=True)["result"], "RESET_REQUESTED")
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])
        initial = next(row for row in self.events() if row["event"] == "TRANSITION_INSPECT")["image_state"]["images"]
        pending = next(row for row in self.events() if row["event"] == "TRANSITION_AFTER_UPLOAD")["image_state"]["images"]
        self.assertTrue(initial[0]["active"])
        self.assertTrue(initial[0]["confirmed"])
        self.assertNotIn("confirmed", initial[1])
        self.assertNotIn("pending", initial[1])
        self.assertNotIn("active", pending[0])
        self.assertTrue(pending[0]["confirmed"])
        self.assertTrue(pending[1]["pending"])
        self.assertNotIn("confirmed", pending[1])

    def test_actual_mcuboot_test_resume_requires_original_log_and_never_uploads(self):
        self.device.wire_flags = True
        self.device.images.append(slot(1, self.helper, pending=True))
        self.record_upload()
        self.assertEqual(self.run_step(resume=True)["result"], "RESUME_INSPECTED")
        self.assertEqual(self.mutations(), [])
        self.assertEqual(self.run_step(resume=True, apply=True)["result"], "RESET_REQUESTED")
        self.assertEqual(self.mutations(), [(2, 0, 5, {})])
        self.uploader.assert_not_called()

    def test_actual_mcuboot_sparse_flags_do_not_infer_bootable_or_primary_confirmation(self):
        normal = copy.deepcopy(self.device.images[0])
        cases = []
        for missing in ("bootable", "confirmed", "hash"):
            bad = dict(normal)
            bad.pop(missing)
            cases.append([bad])
        cases.append([dict(normal, bootable=1)])
        cases.append([normal, {"slot": 1, "version": self.helper["version"], "bootable": True,
                               "hash": bytes.fromhex(self.helper["mcuboot_image_hash"]), "pending": True}])
        self.device.wire_flags = True
        for rows in cases:
            self.device.images = rows
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                self.run_step(apply=True)
            self.assertEqual(self.mutations(), [])
        self.uploader.assert_not_called()

    def test_inactive_nonpending_backup_is_erased_only_after_primary_proof(self):
        self.device.images.append(slot(1, self.usb))
        self.run_step(apply=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])
        self.assertEqual(self.events()[-1]["event"], "TRANSITION_RESET_ACCEPTED")

    def test_empty_unknown_unconfirmed_or_malformed_state_never_mutates(self):
        normal = copy.deepcopy(self.device.images[0])
        states = [[], [slot(0, self.usb, active=True, confirmed=True)],
                  [dict(normal, confirmed=False)], [dict(normal, pending=True)],
                  [dict(normal, active=False)], [dict(normal, confirmed=1)],
                  [dict(normal, permanent=True)], [normal, slot(1, self.usb, active=True)],
                  [normal, dict(normal)], [dict(normal, slot=2)]]
        for images in states:
            with self.subTest(images=images), self.assertRaises(ValueError):
                self.device.images = images
                self.run_step(apply=True)
            self.assertEqual(self.mutations(), [])
        self.uploader.assert_not_called()

    def test_pending_secondary_is_not_erased_or_implicitly_resumed(self):
        for candidate in (self.helper, self.usb):
            self.device.images = [slot(0, self.source, active=True, confirmed=True), slot(1, candidate, pending=True)]
            with self.subTest(candidate=candidate["image_class"]), self.assertRaisesRegex(ValueError, "pending secondary"):
                self.run_step(apply=True)
        self.assertEqual(self.mutations(), [])
        self.uploader.assert_not_called()

    def test_live_app_and_nonunsupported_probe_errors_never_mutate(self):
        self.device.info_reply = self.info
        with self.assertRaisesRegex(CampaignError, "BOOT"):
            self.run_step(apply=True)
        self.device.info_reply = None
        for error in (CommandError("busy", response={"rc": 5}),
                      CommandError("malformed", response={"rc": True}), TransportError("timeout")):
            self.device.info_error = error
            with self.subTest(error=error), self.assertRaises((ValueError, TransportError)):
                self.run_step(apply=True)
        self.assertEqual(self.mutations(), [])
        self.uploader.assert_not_called()

    def test_usb_serial_is_exact_unique_and_rechecked_before_any_write(self):
        for ports in ([], [SimpleNamespace(vid=0x2FE3, serial_number="other", device="COM7")],
                      [self.port, SimpleNamespace(vid=0x2FE3, serial_number="selected", device="COM8")]):
            self.enumerate.return_value = ports
            with self.subTest(ports=ports), self.assertRaises(CampaignError):
                self.run_step(apply=True)
        self.factory.assert_not_called()
        self.enumerate.side_effect = [[self.port], [self.port], []]
        with self.assertRaises(CampaignError):
            self.run_step(apply=True)
        self.assertEqual(self.mutations(), [])
        self.uploader.assert_not_called()

    def test_slot_state_drift_before_mutation_stops(self):
        count = [0]
        def drift():
            count[0] += 1
            if count[0] == 2:
                self.device.images[0]["version"] = "9.9.9+0"
        self.device.before_state = drift
        with self.assertRaisesRegex(ValueError, "state changed"):
            self.run_step(apply=True)
        self.assertEqual(self.mutations(), [])
        self.uploader.assert_not_called()

    def test_failed_erase_stops_before_upload_and_reset(self):
        self.device.images.append(slot(1, self.usb))
        self.device.erase_error = TransportError("erase ACK lost")
        with self.assertRaises(TransportError):
            self.run_step(apply=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])
        self.uploader.assert_not_called()

    def test_failed_upload_stops_and_exact_interrupted_upload_can_resume(self):
        def upload_then_lose_ack(base, snapshot):
            self.upload(base, snapshot)
            raise UploadError("upload completed but process failed")
        self.uploader.side_effect = upload_then_lose_ack
        with self.assertRaises(UploadError):
            self.run_step(apply=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])
        self.assertEqual(self.events()[-1]["event"], "TRANSITION_STOPPED")
        self.uploader.reset_mock()
        result = self.run_step(apply=True, resume=True)
        self.assertEqual(result["result"], "RESET_REQUESTED")
        self.uploader.assert_not_called()
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])

    def test_resume_requires_same_operation_log_and_exact_pending_candidate(self):
        self.device.images.append(slot(1, self.helper, pending=True))
        with self.assertRaises(ValueError):
            self.run_step(apply=True, resume=True)
        self.factory.assert_not_called()
        for token, image_hash in (("f" * 64, self.helper["mcuboot_image_hash"]),
                                  (self.config["token"], self.usb["mcuboot_image_hash"])):
            self.record_upload(token, image_hash)
            with self.assertRaises(ValueError):
                self.run_step(apply=True, resume=True)
        self.record_upload()
        for secondary in (slot(1, self.usb, pending=True), slot(1, self.helper),
                          slot(1, self.helper, pending=True, confirmed=True)):
            self.device.images[1] = secondary
            with self.assertRaisesRegex(ValueError, "exact pending"):
                self.run_step(apply=True, resume=True)
        self.assertEqual(self.mutations(), [])
        self.uploader.assert_not_called()

    def test_resume_requires_proven_secondary_upload_path_not_only_upload_request(self):
        self.device.images.append(slot(1, self.helper, pending=True))
        self.record_upload(secondary_ready=False)
        with self.assertRaises(ValueError):
            self.run_step(apply=True, resume=True)
        self.factory.assert_not_called()
        self.uploader.assert_not_called()

    def test_changed_candidate_or_primary_after_upload_never_resets(self):
        for change in (lambda: self.device.images[1].update(pending=False),
                       lambda: self.device.images[1].update(hash=bytes(32)),
                       lambda: self.device.images[0].update(version="different")):
            self.device = Device(self.source)
            def upload_bad(base, snapshot):
                self.upload(base, snapshot)
                change()
            self.uploader.side_effect = upload_bad
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.run_step(apply=True)
            self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])

    def test_reset_ack_loss_is_not_retried_or_reported_success(self):
        self.device.reset_error = TransportError("reset ACK lost")
        with self.assertRaises(TransportError):
            self.run_step(apply=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])
        self.assertEqual(self.events()[-1]["event"], "TRANSITION_STOPPED")
        self.assertNotIn("TRANSITION_RESET_ACCEPTED", [row["event"] for row in self.events()])

    def test_install_usb_requires_exact_completed_receipt_and_confirmed_helper(self):
        self.candidate, self.candidate_data = self.usb, self.usb_data
        self.device = Device(self.helper)
        for receipt in (None, self.root / "missing.json"):
            with self.subTest(receipt=receipt), self.assertRaises((ValueError, OSError)):
                self.run_step("install-usb", apply=True, receipt=receipt)
        self.factory.assert_not_called()
        self.device.images[0]["confirmed"] = False
        with self.assertRaisesRegex(ValueError, "confirmed"):
            self.run_step("install-usb", apply=True)
        self.assertEqual(self.mutations(), [])
        self.device.images[0]["confirmed"] = True
        self.assertEqual(self.run_step("install-usb", apply=True)["result"], "RESET_REQUESTED")

    def test_receipt_rejects_failure_unconfirmed_wrong_board_token_and_tampering(self):
        for line in (self.completion(-1), self.completion(2), self.completion(0, 0),
                     self.completion().replace(self.config["identity"].encode(), b"9999999999999999"),
                     self.completion().replace(self.config["token"].encode(), b"f" * 64),
                     self.completion().rstrip(b"\n")):
            with self.subTest(line=line), self.assertRaises(ValueError):
                transition.receipt_payload(self.config, self.helper, line)
        for rc in (0, 1):
            receipt = transition.receipt_payload(self.config, self.helper, self.completion(rc))
            self.write_json(self.receipt, receipt)
            self.assertEqual(transition.verify_receipt(self.receipt, self.config, self.helper), receipt)
            receipt["helper_hash"] = "ab" * 32
            self.write_json(self.receipt, receipt)
            with self.assertRaises(ValueError):
                transition.verify_receipt(self.receipt, self.config, self.helper)

    def test_receipt_console_only_reads_and_rechecks_physical_serial(self):
        console = Mock(readline=Mock(side_effect=[b"boot log\n", self.completion(1)]))
        factory = Mock(return_value=console)
        result = transition.collect_receipt(self.config, self.helper, self.receipt,
                                             enumerate_ports=self.enumerate, console_factory=factory)
        self.assertEqual(result["result"], "CLEANUP_CONFIRMED")
        console.write.assert_not_called()
        console.close.assert_called_once()
        factory.assert_called_once_with("COM7", baudrate=115200, timeout=0.2, write_timeout=0.2)
        self.assertEqual(self.enumerate.call_count, 2)
        self.assertEqual(transition.verify_receipt(self.receipt, self.config, self.helper)["line"],
                         self.completion(1).decode())

    def test_receipt_disconnect_failure_and_timeout_never_write_success(self):
        self.receipt.unlink()
        console = Mock(readline=Mock(return_value=self.completion()))
        self.enumerate.side_effect = [[self.port], []]
        with self.assertRaises(CampaignError):
            transition.collect_receipt(self.config, self.helper, self.receipt,
                                         enumerate_ports=self.enumerate, console_factory=Mock(return_value=console))
        self.assertFalse(self.receipt.exists())
        console.close.assert_called_once()
        self.enumerate.side_effect = None
        for line in (self.completion(-1), b"OTA_TRANSITION " + b"x" * 250):
            console = Mock(readline=Mock(return_value=line))
            with self.assertRaises(ValueError):
                transition.collect_receipt(self.config, self.helper, self.receipt,
                                             enumerate_ports=self.enumerate, console_factory=Mock(return_value=console))
            self.assertFalse(self.receipt.exists())
            console.close.assert_called_once()
        with patch("transition_ota_usb.time.monotonic", side_effect=[0, 31]), self.assertRaises(CampaignError):
            transition.collect_receipt(self.config, self.helper, self.receipt,
                                         enumerate_ports=self.enumerate, console_factory=Mock(return_value=console))
        self.assertFalse(self.receipt.exists())

    def test_verify_usb_supports_development_backup_but_blocks_any_pending(self):
        self.device = Device(self.usb)
        self.device.images.append(slot(1, self.source))
        result = self.run_step("verify-usb")
        self.assertEqual(result["result"], "USB_VERIFIED")
        self.assertEqual(self.mutations(), [])
        self.assertTrue(transition._has_event(self.log, self.config["token"], "USB_VERIFIED", self.usb["mcuboot_image_hash"]))
        self.device.images[1]["pending"] = True
        with self.assertRaises(ValueError):
            self.run_step("verify-usb", apply=True)
        self.assertEqual(self.mutations(), [])

    def test_verify_usb_optional_reset_rechecks_state_and_never_confirms(self):
        self.device = Device(self.usb)
        result = self.run_step("verify-usb", apply=True)
        self.assertTrue(result["reset_requested"])
        self.assertEqual(self.mutations(), [(2, 0, 5, {})])
        self.uploader.assert_not_called()

    def test_return_ota_requires_same_current_verified_usb_and_cleanup_receipt(self):
        self.device = Device(self.usb)
        self.candidate, self.candidate_data = self.source, self.source_data
        with self.assertRaisesRegex(ValueError, "verify the confirmed"):
            self.run_step("return-ota", apply=True)
        self.factory.assert_not_called()
        self.run_step("verify-usb")
        self.assertEqual(self.run_step("return-ota", apply=True)["result"], "RESET_REQUESTED")
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])

    def test_new_usb_development_image_requires_new_exact_verification_before_return(self):
        self.device = Device(self.usb)
        self.run_step("verify-usb")
        old_usb = self.usb
        self.usb_data = artifact(body_size=176, image_class=None)
        self.usb = inspect_plain_usb(self.usb_data, build_id="usb-new")
        self.device = Device(self.usb)
        self.device.images.append(slot(1, old_usb))
        self.candidate, self.candidate_data = self.source, self.source_data
        with self.assertRaisesRegex(ValueError, "verify the confirmed"):
            self.run_step("return-ota", apply=True)
        self.assertEqual(self.mutations(), [])
        self.run_step("verify-usb")
        self.assertEqual(self.run_step("return-ota", apply=True)["result"], "RESET_REQUESTED")
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])

    def test_old_receipt_cannot_authorize_fresh_roundtrip(self):
        self.info["capture_id"] = "b" * 32
        self.write_json(self.info_path, self.info)
        fresh = generate(self.source_path, self.source_manifest_path, self.info_path,
                         self.root / "fresh", no_campaign=True)
        self.assertNotEqual(fresh["token"], self.config["token"])
        with self.assertRaisesRegex(ValueError, "different board or operation"):
            transition.verify_receipt(self.receipt, fresh, self.helper)

    def test_image_inputs_validate_source_snapshot_header_helper_and_board_before_usb(self):
        self.assertEqual(transition.inputs(self.config_path, self.helper_path, self.helper_manifest_path,
                                         "selected", self.config["identity"])[1], self.helper)
        for serial, identity in (("other", self.config["identity"]), ("selected", "ffeeddccbbaa9988")):
            with self.subTest(serial=serial), self.assertRaises(ValueError):
                transition.inputs(self.config_path, self.helper_path, self.helper_manifest_path, serial, identity)
        self.source_path.write_bytes(self.source_data[:-1] + b"x")
        with self.assertRaises(ValueError):
            transition.inputs(self.config_path, self.helper_path, self.helper_manifest_path, "selected", self.config["identity"])
        self.source_path.write_bytes(self.source_data)
        self.config_path.with_suffix(".h").write_text("changed header")
        with self.assertRaisesRegex(ValueError, "header"):
            transition.inputs(self.config_path, self.helper_path, self.helper_manifest_path, "selected", self.config["identity"])
        self.factory.assert_not_called()

    def test_helper_config_identity_and_plain_usb_provenance_are_checked(self):
        wrong = dict(self.helper, build_id="transition-wrong")
        self.write_json(self.helper_manifest_path, wrong)
        with self.assertRaisesRegex(ValueError, "different transition"):
            transition.inputs(self.config_path, self.helper_path, self.helper_manifest_path, "selected", self.config["identity"])
        actual, data = transition._image(self.usb_path, self.usb_manifest_path, self.source, "usb")
        self.assertEqual(actual["mcuboot_image_hash"], self.usb["mcuboot_image_hash"])
        self.assertEqual(data, self.usb_data)
        for field, value in (("ota_enabled", True), ("auto_confirmation", False), ("elf_sha256", "missing")):
            bad = copy.deepcopy(self.usb)
            bad["build_proof"][field] = value
            self.write_json(self.usb_manifest_path, bad)
            with self.subTest(field=field), self.assertRaises(ValueError):
                transition._image(self.usb_path, self.usb_manifest_path, self.source, "usb")
        self.write_json(self.usb_manifest_path, self.usb)
        self.usb_path.write_bytes(self.source_data)
        with self.assertRaises(ValueError):
            transition._image(self.usb_path, self.usb_manifest_path, self.source, "usb")

    def test_usb_artifact_must_match_manifest_profile_and_existing_key(self):
        for field, value in (("artifact_sha256", "ab" * 32), ("image_class", "receiver"),
                             ("execution_profile", "ota"), ("build_id", "usb-other")):
            self.write_json(self.usb_manifest_path, dict(self.usb, **{field: value}))
            with self.subTest(field=field), self.assertRaises(ValueError):
                transition._image(self.usb_path, self.usb_manifest_path, self.source, "usb")
        self.write_json(self.usb_manifest_path, self.usb)
        source = copy.deepcopy(self.source)
        source["signature"]["key_sha256"] = "ab" * 32
        with self.assertRaisesRegex(ValueError, "signing key"):
            transition._image(self.usb_path, self.usb_manifest_path, source, "usb")
        source = copy.deepcopy(self.source)
        source["profile"]["hardware_id"] += 1
        with self.assertRaisesRegex(ValueError, "hardware profile"):
            transition._image(self.usb_path, self.usb_manifest_path, source, "usb")

    def test_candidate_bytes_changed_before_step_rejects_without_serial_access(self):
        with self.assertRaisesRegex(ValueError, "snapshot mismatch"):
            self.run_step(apply=True, data=self.helper_data[:-1] + b"x")
        self.enumerate.assert_not_called()

    def test_incomplete_or_nonobject_transaction_log_blocks_before_usb(self):
        for data in (b'{"event":"unflushed"}', b'[]\n', b'42\n', b'{broken}\n'):
            self.log.write_bytes(data)
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.run_step(apply=True)
            self.assertEqual(self.log.read_bytes(), data)
        self.enumerate.assert_not_called()

    def test_verify_ota_is_read_only_and_accepts_confirmed_waiting_can(self):
        transport = Mock(request=Mock(return_value=self.info))
        factory = Mock(return_value=SimpleNamespace(connect=Mock(return_value=transport)))
        self.assertEqual(transition.verify_ota(self.config, self.source, connection_factory=factory)["result"], "OTA_VERIFIED")
        self.info.update(phase="WAITING_CAN", available=False, healthy=False, can_ready=False)
        self.assertEqual(transition.verify_ota(self.config, self.source, connection_factory=factory)["result"], "OTA_VERIFIED")
        self.assertEqual(transport.method_calls, [("request", ("info",), {}), ("close", (), {}),
                                                ("request", ("info",), {}), ("close", (), {})])

    def test_verify_ota_refuses_maintenance_wrong_image_identity_or_health(self):
        for field, value in (("maintenance", True), ("active_confirmed", False), ("phase", "PREPARED"),
                             ("mcuboot_image_hash", "ab" * 32), ("identity", "aabbccddeeff0011"),
                             ("error", -1), ("local_healthy", False)):
            info = dict(self.info, **{field: value})
            transport = Mock(request=Mock(return_value=info))
            factory = Mock(return_value=SimpleNamespace(connect=Mock(return_value=transport)))
            with self.subTest(field=field), self.assertRaises((ValueError, CampaignError)):
                transition.verify_ota(self.config, self.source, connection_factory=factory)
            transport.close.assert_called_once()

    def test_receiver_return_requires_explicit_clean_maintenance_status(self):
        for value in (None, 0, "false"):
            info = dict(self.info, maintenance=value)
            if value is None:
                del info["maintenance"]
            transport = Mock(request=Mock(return_value=info))
            factory = Mock(return_value=SimpleNamespace(connect=Mock(return_value=transport)))
            with self.subTest(value=value), self.assertRaises(ValueError):
                transition.verify_ota(self.config, self.source, connection_factory=factory)
            transport.close.assert_called_once()

    def test_cli_defaults_to_inspection_and_rejects_mutated_config_before_serial(self):
        args = ["install-helper", "--serial", "selected", "--identity", self.config["identity"],
                "--config", str(self.config_path), "--helper", str(self.helper_path),
                "--helper-manifest", str(self.helper_manifest_path)]
        with patch("transition_ota_usb.bootloader_step", return_value={"result": "INSPECTED"}) as step, redirect_stdout(io.StringIO()):
            self.assertEqual(transition.main(args), 0)
        self.assertIs(step.call_args.kwargs["apply"], False)
        self.source_path.write_bytes(b"changed")
        with patch("transition_ota_usb.bootloader_step") as step, redirect_stderr(io.StringIO()):
            self.assertEqual(transition.main(args + ["--apply"]), 1)
        step.assert_not_called()


if __name__ == "__main__":
    unittest.main()
