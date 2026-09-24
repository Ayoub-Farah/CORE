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
from recover_ota import recover, main, verify_inputs
from prepare_ota_recovery import generate
from smp_transport import CommandError, TransportError
from bootloader_upload import UploadError
from ota_artifact import inspect_image
from test_pc_artifact import artifact
from test_pc_recovery_config import records, staged_records, IDS


def slot(number, image_hash, *, active=False, confirmed=False, pending=False, version="1.0.0"):
    return {"slot": number, "version": version, "hash": bytes.fromhex(image_hash),
            "active": active, "confirmed": confirmed, "pending": pending, "bootable": True, "permanent": False}


class Device:
    def __init__(self):
        self.images = [slot(0, "cc" * 32, active=True, confirmed=True), slot(1, "bb" * 32, pending=True)]
        self.calls = []
        self.app_reply = False
        self.info_error = None
        self.erase_wrong = False
        self.erase_error = None
        self.reset_error = None

    def request(self, name):
        self.calls.append(("info",))
        if self.info_error:
            raise self.info_error
        if self.app_reply:
            return {"service": "owntech-ota", "protocol": 1}
        raise CommandError("unsupported", response={"rc": 8})

    def image_state(self):
        self.calls.append(("image_state",))
        return {"images": copy.deepcopy(self.images)}

    def _request(self, op, group, command, label, payload):
        self.calls.append((op, group, command, payload))
        if (op, group, command) == (2, 1, 5):
            if self.erase_error:
                raise self.erase_error
            if not self.erase_wrong:
                self.images = self.images[:1]
        if (op, group, command) == (2, 0, 5) and self.reset_error:
            raise self.reset_error
        return {"rc": 0}

    def close(self):
        self.calls.append(("close",))


class RecoveryUploadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        events = records()
        events[2]["manifest"]["signature"] = {"key_sha256": "00" * 32}
        events[3]["manifest"] = copy.deepcopy(events[2]["manifest"])
        self.journal = self.root / "campaign.jsonl"
        self.journal.write_text("".join(json.dumps(event) + "\n" for event in events))
        config = generate(self.journal, self.root / "config")
        self.config = self.root / "config/owntech_ota_recovery_config.json"
        data = bytearray(artifact())
        struct.pack_into("<BBHI", data, 20, 0, 0, 1, 0)
        data[648:680] = hashlib.sha256(data[:640]).digest()
        self.image = self.root / "recovery.mcuboot.bin"
        self.image.write_bytes(data)
        self.manifest_value = inspect_image(data, version="0.0.1+0", build_id="recovery-" + config["header_sha256"][:20])
        self.manifest = self.root / "recovery.mcuboot.json"
        self.manifest.write_text(json.dumps(self.manifest_value))
        self.mcumgr = self.root / "mcumgr.exe"
        self.mcumgr.write_bytes(b"mock uploader only; never execute")
        self.log = self.root / "recovery.jsonl"
        self.device = Device()
        self.port = SimpleNamespace(vid=0x2FE3, serial_number="selected", device="COM7")
        self.enumerate = Mock(return_value=[self.port])
        self.factory = Mock(return_value=self.device)
        self.upload = Mock(side_effect=self.upload_image)

    def tearDown(self):
        self.temp.cleanup()

    def upload_image(self, base, snapshot):
        self.assertEqual(snapshot.read_bytes(), self.image.read_bytes())
        self.assertNotEqual(snapshot, self.image)
        self.assertEqual(base, [str(self.mcumgr), "--conntype", "serial", "--connstring",
                              "dev=COM7,baud=115200,mtu=128", "--timeout", "10", "--tries", "1"])
        self.assertEqual(self.device.calls[-1], ("close",))
        self.device.images.append(slot(1, self.manifest_value["mcuboot_image_hash"], pending=True, version="0.0.1"))

    def run_recovery(self, target_identity=IDS[0], **kwargs):
        return recover(self.config, self.image, self.manifest, "selected", target_identity,
                       mcumgr=self.mcumgr, log_path=self.log, enumerate_ports=self.enumerate,
                       transport_factory=self.factory, uploader=self.upload, **kwargs)

    def mutations(self):
        return [call for call in self.device.calls if isinstance(call[0], int)]

    def test_inspection_is_read_only_and_logs_public_slots(self):
        self.assertEqual(self.run_recovery()["result"], "INSPECTED")
        self.assertEqual(self.device.calls, [("info",), ("image_state",), ("close",)])
        self.assertEqual(self.mutations(), [])
        self.upload.assert_not_called()
        events = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual(events[1]["image_state"]["images"][0]["hash"], "cc" * 32)
        self.assertEqual(events[-1]["event"], "RECOVERY_INSPECTION_PASSED")

    def test_empty_initial_image_list_blocks_inspect_and_apply_without_mutation(self):
        self.device.images = []

        def empty_image_state():
            self.device.calls.append(("image_state",))
            return {"images": [], "splitStatus": 0}

        self.device.image_state = empty_image_state
        for apply in (False, True):
            self.device.calls.clear()
            with self.subTest(apply=apply), self.assertRaisesRegex(ValueError,
                    "empty image list:.*recognizes no image; primary/secondary state cannot be verified; "
                    "no recovery action is authorized from this state"):
                self.run_recovery(apply=apply)
            self.assertEqual(self.device.calls, [("info",), ("image_state",), ("close",)])
            self.assertEqual(self.mutations(), [])
            self.upload.assert_not_called()
            events = [json.loads(line) for line in self.log.read_text().splitlines()]
            self.assertEqual(events[-2]["event"], "RECOVERY_INSPECT")
            self.assertEqual(events[-2]["image_state"], {"images": [], "splitStatus": 0})
            self.assertEqual(events[-1]["event"], "RECOVERY_STOPPED")
            self.assertIn("empty image list", events[-1]["error"])

    def test_empty_list_after_mutation_preserves_the_logged_action_and_stops(self):
        for after_upload in (False, True):
            self.device = Device()
            self.factory.return_value = self.device
            if after_upload:
                self.upload.side_effect = lambda *args: self.device.images.clear()
            else:
                request = self.device._request

                def erase_then_unrecognized(*args):
                    reply = request(*args)
                    self.device.images.clear()
                    return reply

                self.device._request = erase_then_unrecognized
            with self.subTest(after_upload=after_upload), self.assertRaisesRegex(ValueError, "empty image list"):
                self.run_recovery(apply=True)
            self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])
            self.assertEqual(self.device.calls[-1], ("close",))
            events = [json.loads(line) for line in self.log.read_text().splitlines()]
            self.assertEqual(events[-2]["event"], "RECOVERY_AFTER_UPLOAD" if after_upload else "RECOVERY_AFTER_ERASE")
            self.assertEqual(events[-1]["event"], "RECOVERY_STOPPED")
            self.assertNotIn("erased", events[-1]["error"])
            self.assertNotIn("no mutation", events[-1]["error"])

    def test_apply_erases_only_secondary_and_resets_once_after_exact_reinspection(self):
        self.assertEqual(self.run_recovery(apply=True)["result"], "RESET_REQUESTED")
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])
        self.upload.assert_called_once()
        self.assertEqual(self.factory.call_count, 2)
        self.assertEqual(self.device.calls[-3:], [("image_state",), (2, 0, 5, {}), ("close",)])
        self.assertTrue(all(call.args == ("COM7",) and call.kwargs == {"timeout": 10} for call in self.factory.call_args_list))

    def test_after_revert_requires_explicit_matching_initial_pending_flag(self):
        for pending, after_revert in ((False, False), (True, True)):
            self.device.calls.clear()
            self.device.images[1]["pending"] = pending
            with self.subTest(pending=pending, after_revert=after_revert), self.assertRaisesRegex(ValueError, "exact .*nonactive image"):
                self.run_recovery(apply=True, after_revert=after_revert)
            self.assertEqual(self.mutations(), [])
            self.assertEqual(self.device.calls[-1], ("close",))
        self.upload.assert_not_called()

    def test_after_revert_keeps_exact_hash_and_confirmed_primary_guards(self):
        valid = copy.deepcopy(self.device.images)
        valid[1]["pending"] = False
        for index, field, value in ((0, "confirmed", False), (0, "hash", b"x" * 32),
                                    (1, "hash", b"x" * 32), (1, "active", True), (1, "bootable", False)):
            self.device.images = copy.deepcopy(valid)
            self.device.images[index][field] = value
            with self.subTest(index=index, field=field), self.assertRaises(ValueError):
                self.run_recovery(apply=True, after_revert=True)
            self.assertEqual(self.mutations(), [])
        self.upload.assert_not_called()

    def test_after_revert_apply_requires_pending_recovery_image_before_single_reset(self):
        self.device.images[1]["pending"] = False
        self.assertEqual(self.run_recovery(apply=True, after_revert=True)["result"], "RESET_REQUESTED")
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])
        self.upload.assert_called_once()
        events = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertIs(events[0]["after_revert"], True)
        self.assertIs(events[1]["image_state"]["images"][1]["pending"], False)
        after = next(event for event in events if event["event"] == "RECOVERY_AFTER_UPLOAD")
        self.assertIs(after["image_state"]["images"][1]["pending"], True)

    def test_after_revert_nonpending_uploaded_recovery_never_resets(self):
        self.device.images[1]["pending"] = False

        def upload(base, snapshot):
            self.upload_image(base, snapshot)
            self.device.images[1]["pending"] = False

        self.upload.side_effect = upload
        with self.assertRaisesRegex(ValueError, "exact pending nonactive image"):
            self.run_recovery(apply=True, after_revert=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])
        self.assertEqual(json.loads(self.log.read_text().splitlines()[-1])["event"], "RECOVERY_STOPPED")

    def test_live_ota_silence_and_other_errors_never_erase_upload_or_reset(self):
        for mode in ("app", "silent", "wrong_rc"):
            self.device.calls.clear()
            self.device.app_reply = mode == "app"
            self.device.info_error = (TransportError("no response") if mode == "silent" else
                                     CommandError("busy", response={"rc": 6}) if mode == "wrong_rc" else None)
            with self.subTest(mode=mode), self.assertRaises((ValueError, RuntimeError, OSError)):
                self.run_recovery(apply=True)
            self.assertEqual(self.mutations(), [])
            self.assertNotIn(("image_state",), self.device.calls)
        self.upload.assert_not_called()

    def test_primary_and_secondary_must_match_before_any_erase(self):
        valid = copy.deepcopy(self.device.images)
        variants = [(0, "hash", b"x" * 32), (0, "confirmed", False), (0, "active", False),
                    (0, "pending", True), (1, "hash", b"x" * 32), (1, "active", True),
                    (1, "pending", False), (1, "confirmed", True), (1, "permanent", True)]
        for index, field, value in variants:
            self.device.images = copy.deepcopy(valid)
            self.device.images[index][field] = value
            with self.subTest(index=index, field=field), self.assertRaises(ValueError):
                self.run_recovery(apply=True)
            self.assertEqual(self.mutations(), [])
        for bad in (valid[:1], valid + [copy.deepcopy(valid[1])], valid + [slot(2, "dd" * 32)]):
            self.device.images = bad
            with self.assertRaises(ValueError):
                self.run_recovery(apply=True)
        self.upload.assert_not_called()

    def test_erase_failure_or_remaining_secondary_never_uploads_or_resets(self):
        for error in (None, CommandError("erase refused", response={"rc": 6})):
            self.device.calls.clear()
            self.device.erase_wrong = True
            self.device.erase_error = error
            with self.subTest(error=error), self.assertRaises(ValueError):
                self.run_recovery(apply=True)
            self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])
        self.upload.assert_not_called()

    def test_upload_error_or_wrong_post_upload_hash_never_resets(self):
        self.upload.side_effect = UploadError("bounded upload failed")
        with self.assertRaises(UploadError):
            self.run_recovery(apply=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])
        self.device = Device()
        self.factory.return_value = self.device
        self.upload.side_effect = lambda *args: self.device.images.append(slot(1, "dd" * 32, pending=True))
        with self.assertRaisesRegex(ValueError, "exact pending"):
            self.run_recovery(apply=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])

    def test_changed_primary_after_upload_never_resets(self):
        def upload(base, snapshot):
            self.upload_image(base, snapshot)
            self.device.images[0]["version"] = "unexpected"
        self.upload.side_effect = upload
        with self.assertRaisesRegex(ValueError, "primary image state changed"):
            self.run_recovery(apply=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1})])

    def test_reset_ack_loss_is_not_retried_or_treated_as_success(self):
        self.device.reset_error = TransportError("USB disconnected before reset ACK")
        with self.assertRaises(TransportError):
            self.run_recovery(apply=True)
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])
        self.assertEqual(json.loads(self.log.read_text().splitlines()[-1])["event"], "RECOVERY_STOPPED")

    def test_serial_selection_is_exact_and_rechecked_before_mutation(self):
        for ports in ([], [SimpleNamespace(vid=0x2FE3, serial_number="other", device="COM7")],
                      [self.port, SimpleNamespace(vid=0x2FE3, serial_number="selected", device="COM8")]):
            self.enumerate.return_value = ports
            with self.assertRaises(RuntimeError):
                self.run_recovery(apply=True)
        self.factory.assert_not_called()
        self.enumerate.side_effect = [[self.port], [self.port], []]
        with self.assertRaises(RuntimeError):
            self.run_recovery(apply=True)
        self.assertEqual(self.mutations(), [])

    def test_artifact_manifest_config_and_target_are_verified_before_usb(self):
        original = self.image.read_bytes()
        self.image.write_bytes(original[:-1] + b"x")
        with self.assertRaises(ValueError):
            self.run_recovery(apply=True)
        self.image.write_bytes(original)
        manifest = copy.deepcopy(self.manifest_value)
        manifest["build_id"] = "recovery-wrong"
        self.manifest.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "dedicated recovery"):
            self.run_recovery(apply=True)
        self.manifest.write_text(json.dumps(self.manifest_value))
        with self.assertRaisesRegex(ValueError, "outside the frozen"):
            verify_inputs(self.config, self.image, self.manifest, "9999999999999999")
        self.config.with_suffix(".h").write_text("different guard header")
        with self.assertRaisesRegex(ValueError, "header/config provenance"):
            self.run_recovery(apply=True)
        self.enumerate.assert_not_called()

    def staged_config(self):
        events = staged_records()
        events[2]["manifest"]["signature"] = {"key_sha256": "00" * 32}
        events[3]["manifest"] = copy.deepcopy(events[2]["manifest"])
        self.journal.write_text("".join(json.dumps(event) + "\n" for event in events))
        config = generate(self.journal, self.config.parent, staged_lead_only=True)
        self.manifest_value = inspect_image(self.image.read_bytes(), version="0.0.1+0",
                                           build_id="recovery-" + config["header_sha256"][:20])
        self.manifest.write_text(json.dumps(self.manifest_value))

    def test_staged_lead_config_requires_mode_and_rejects_follower_before_usb(self):
        self.staged_config()
        with self.assertRaises(ValueError):
            self.run_recovery(apply=True)
        with self.assertRaisesRegex(ValueError, "forbids recovery of a follower"):
            recover(self.config, self.image, self.manifest, "selected", IDS[1],
                    staged_lead_only=True, apply=True, mcumgr=self.mcumgr,
                    enumerate_ports=self.enumerate, transport_factory=self.factory, uploader=self.upload)
        self.enumerate.assert_not_called()
        self.factory.assert_not_called()
        self.upload.assert_not_called()

    def test_staged_lead_after_revert_apply_keeps_exact_slot_guards(self):
        self.staged_config()
        self.device.images[1]["pending"] = False
        with self.assertRaisesRegex(ValueError, "exact pending"):
            self.run_recovery(staged_lead_only=True, apply=True)
        self.assertEqual(self.mutations(), [])
        self.upload.assert_not_called()
        self.assertEqual(self.run_recovery(staged_lead_only=True, after_revert=True, apply=True)["result"],
                         "RESET_REQUESTED")
        self.assertEqual(self.mutations(), [(2, 1, 5, {"slot": 1}), (2, 0, 5, {})])
        events = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertTrue(next(event for event in reversed(events) if event["event"] == "RECOVERY_INPUT")["staged_lead_only"])

    def test_staged_repair_target_and_guards_cannot_be_widened(self):
        self.staged_config()
        original = self.config.read_bytes()
        config = json.loads(original)
        config["repair_targets"].append(IDS[1])
        self.config.write_text(json.dumps(config))
        with self.assertRaisesRegex(ValueError, "no longer matches"):
            self.run_recovery(staged_lead_only=True, apply=True)
        self.enumerate.assert_not_called()

    def prepared_config(self):
        self.staged_config()
        config = generate(self.journal, self.config.parent, prepared_follower_only=True)
        self.manifest_value = inspect_image(self.image.read_bytes(), version="0.0.1+0",
                                           build_id="recovery-" + config["header_sha256"][:20])
        self.manifest.write_text(json.dumps(self.manifest_value))
        self.device.images = [slot(0, "bb" * 32, active=True, confirmed=True)]

    def test_prepared_follower_inspection_is_readonly_with_absent_secondary(self):
        self.prepared_config()
        result = self.run_recovery(IDS[1], prepared_follower_only=True)
        self.assertEqual(result["result"], "INSPECTED")
        self.assertEqual(result["identity"], IDS[1])
        self.assertEqual(self.device.calls, [("info",), ("image_state",), ("close",)])
        self.upload.assert_not_called()
        self.assertEqual(self.mutations(), [])

    def test_prepared_follower_apply_uploads_without_erase_and_resets_only_after_proof(self):
        self.prepared_config()
        result = self.run_recovery(IDS[1], prepared_follower_only=True, apply=True)
        self.assertEqual(result["result"], "RESET_REQUESTED")
        self.assertEqual(self.mutations(), [(2, 0, 5, {})])
        self.upload.assert_called_once()
        events = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertTrue(events[0]["prepared_follower_only"])
        self.assertEqual(events[0]["identity"], IDS[1])
        names = [event["event"] for event in events]
        self.assertNotIn("RECOVERY_ERASE_SECONDARY_REQUEST", names)
        self.assertLess(names.index("RECOVERY_AFTER_UPLOAD"), names.index("RECOVERY_RESET_REQUEST"))

    def test_prepared_mode_refuses_lead_wrong_mode_and_after_revert_before_usb(self):
        self.prepared_config()
        for target, options in (
            (IDS[0], {"prepared_follower_only": True}),
            (IDS[1], {}),
            (IDS[1], {"prepared_follower_only": True, "after_revert": True}),
            (IDS[1], {"prepared_follower_only": True, "staged_lead_only": True}),
        ):
            with self.subTest(target=target, options=options), self.assertRaises(ValueError):
                self.run_recovery(target, apply=True, **options)
        self.enumerate.assert_not_called()
        self.factory.assert_not_called()
        self.upload.assert_not_called()

    def test_prepared_mode_refuses_any_secondary_or_wrong_primary_without_mutation(self):
        self.prepared_config()
        for images in (
            [slot(0, "bb" * 32, active=True, confirmed=True), slot(1, "bb" * 32)],
            [slot(0, "bb" * 32, active=True, confirmed=True), slot(1, "ee" * 32, pending=True)],
            [slot(0, "bb" * 32, active=True, confirmed=False)],
            [slot(0, "ee" * 32, active=True, confirmed=True)],
        ):
            self.device.images = images
            self.device.calls.clear()
            with self.subTest(images=images), self.assertRaises(ValueError):
                self.run_recovery(IDS[1], prepared_follower_only=True, apply=True)
            self.assertEqual(self.device.calls, [("info",), ("image_state",), ("close",)])
            self.assertEqual(self.mutations(), [])
        self.upload.assert_not_called()

    def test_prepared_nonpending_helper_after_upload_never_resets(self):
        self.prepared_config()

        def upload_without_pending(base, snapshot):
            self.upload_image(base, snapshot)
            self.device.images[1]["pending"] = False

        self.upload.side_effect = upload_without_pending
        with self.assertRaisesRegex(ValueError, "exact pending"):
            self.run_recovery(IDS[1], prepared_follower_only=True, apply=True)
        self.upload.assert_called_once()
        self.assertEqual(self.mutations(), [])
        self.assertEqual(self.device.calls[-1], ("close",))

    def test_cli_defaults_to_inspect_and_requires_explicit_identity_and_serial(self):
        args = ["--config", str(self.config), "--image", str(self.image), "--manifest", str(self.manifest),
                "--serial", "selected", "--identity", IDS[0]]
        with patch("recover_ota.recover", return_value={"result": "INSPECTED"}) as run, redirect_stdout(io.StringIO()):
            self.assertEqual(main(args), 0)
        self.assertIs(run.call_args.kwargs["apply"], False)
        self.assertIs(run.call_args.kwargs["after_revert"], False)
        self.assertIs(run.call_args.kwargs["staged_lead_only"], False)
        self.assertIs(run.call_args.kwargs["prepared_follower_only"], False)
        with patch("recover_ota.recover", return_value={"result": "INSPECTED"}) as run, redirect_stdout(io.StringIO()):
            self.assertEqual(main(args + ["--after-revert"]), 0)
        self.assertIs(run.call_args.kwargs["apply"], False)
        self.assertIs(run.call_args.kwargs["after_revert"], True)
        with patch("recover_ota.recover", return_value={"result": "INSPECTED"}) as run, redirect_stdout(io.StringIO()):
            self.assertEqual(main(args + ["--prepared-follower-only"]), 0)
        self.assertIs(run.call_args.kwargs["prepared_follower_only"], True)
        self.assertIs(run.call_args.kwargs["staged_lead_only"], False)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            main(args + ["--prepared-follower-only", "--staged-lead-only"])
        self.assertEqual(error.exception.code, 2)
        with patch("recover_ota.recover", return_value={"result": "INSPECTED"}) as run, redirect_stdout(io.StringIO()):
            self.assertEqual(main(args + ["--staged-lead-only", "--after-revert"]), 0)
        self.assertIs(run.call_args.kwargs["staged_lead_only"], True)
        self.assertIs(run.call_args.kwargs["after_revert"], True)


if __name__ == "__main__":
    unittest.main()
