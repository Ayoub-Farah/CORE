"""Recovery assistants use real artifact/journal guards and simulated USB only."""
import copy
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
import ota_recovery_workflow as recovery
from ota_workflow import Workflow
from ota_workflow_dialogs import Cancelled
from lead_update import CampaignError
from ota_artifact import inspect_usb_image
from test_pc_artifact import artifact
from test_pc_compact_recovery import compact_records
from test_pc_recover import Device, slot
from test_pc_recovery_config import IDS
from test_ota_workflow import FakeUI


class RecoveryWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.ui = FakeUI()
        self.port = SimpleNamespace(device="COM7", vid=0x2FE3, serial_number="selected")
        self.enumerate = Mock(return_value=[self.port])
        self.mcumgr = self.root / "mcumgr.exe"
        self.mcumgr.write_bytes(b"mock; never execute")
        self.runner = Mock(side_effect=self.build)
        self.w = Workflow(self.root, "OTA_RECOVERY", self.mcumgr, self.ui,
                          enumerate_ports=self.enumerate, runner=self.runner)
        self.r = recovery.RecoveryWorkflow(self.w)
        self.original_data = artifact()
        self.original = inspect_usb_image(self.original_data, build_id="original")
        original_dir = self.root / "ota-artifacts/history/OTA/original"
        original_dir.mkdir(parents=True)
        (original_dir / "firmware.bin").write_bytes(self.original_data)
        (original_dir / "firmware.json").write_text(json.dumps(self.original))
        self.info = dict(identity=IDS[1], usb_serial="selected", image_class="receiver", active_confirmed=True,
                         local_healthy=True, maintenance=True, phase="RECOVERY_REQUIRED",
                         mcuboot_image_hash=self.original["mcuboot_image_hash"], build_id="original",
                         version=self.original["version"])
        self.w.info = Mock(return_value=self.info)
        self.journal = self.root / "ota-journals/failed.jsonl"
        self.journal.parent.mkdir()
        self.events = compact_records()
        self.events[2]["inventory"][0]["mcuboot_image_hash"] = self.original["mcuboot_image_hash"]
        self.write_events()
        self.ui.answers["Interrupted CAN campaign"] = str(self.journal)
        (self.root / "platformio.ini").write_text("[platformio]\nextra_configs = owntech/pio_extra.ini\n    src/app.ini\n[env]\nboard = spin\n")
        self.silence = redirect_stdout(io.StringIO())
        self.silence.__enter__()
        self.addCleanup(self.silence.__exit__, None, None, None)
        no_input = patch("builtins.input", side_effect=AssertionError("No terminal prompts"))
        no_input.start()
        self.addCleanup(no_input.stop)

    def write_events(self):
        self.journal.write_text("".join(json.dumps(event) + "\n" for event in self.events))

    def build(self, command, **kwargs):
        build_config = Path(command[command.index("-c") + 1])
        config = json.loads((build_config.parent / "config/owntech_ota_recovery_config.json").read_text())
        data = bytearray(artifact(image_class=None))
        struct.pack_into("<BBHI", data, 20, 0, 0, 1, 0)
        data[648:680] = hashlib.sha256(data[:640]).digest()
        record = inspect_usb_image(data, version="0.0.1+0", build_id="recovery-" + config["header_sha256"][:20])
        record["filename"] = "firmware.mcuboot.bin"
        folder = self.root / "ota-artifacts/OTA_RECOVERY"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "firmware.mcuboot.bin").write_bytes(data)
        (folder / "firmware.mcuboot.json").write_text(json.dumps(record))

    def prepared(self, phase="PREPARED"):
        path, state = self.r.prepare("selected")
        self.w.save(path, state, phase)
        self.ui.answers["Receiver recovery archive"] = str(path)
        return path, state

    def result(self, serial, target, output, **kwargs):
        record = dict(identity=target, usb_serial=serial, line="confirmed result")
        recovery.transition._save(output, record)
        return record

    def verification(self):
        return dict(result="OTA_VERIFIED", identity=IDS[1], info=dict(self.info, phase="IDLE", maintenance=False))

    def test_preparation_archives_exact_receiver_and_scopes_nested_build_without_editing_project(self):
        before = (self.root / "platformio.ini").read_bytes()
        path, state = self.prepared()
        config, helper, receiver, data = self.r.context(path, state)
        self.assertEqual(data, self.original_data)
        self.assertEqual((path / "campaign.jsonl").read_bytes(), self.journal.read_bytes())
        self.assertEqual((self.root / "platformio.ini").read_bytes(), before)
        self.assertFalse((self.root / ".pio/ota-recovery-config").exists())
        self.assertIn((path / "build-override.ini").as_posix(), (path / "platformio.ini").read_text())
        self.assertIn((path / "config").as_posix(), (path / "build-override.ini").read_text())
        self.assertEqual(self.runner.call_args.args[0][-4:], ["-e", "OTA_RECOVERY", "-t", "mcuboot-image"])
        self.assertEqual(config["targets"][0]["identity"], IDS[1])

    def test_wrong_board_or_activated_campaign_cannot_prepare_or_build(self):
        for update in ({"image_class": "lead"}, {"identity": IDS[0]}, {"active_confirmed": False},
                       {"maintenance": False}, {"mcuboot_image_hash": "ff" * 32}):
            with self.subTest(update=update):
                self.w.info.return_value = dict(self.info, **update)
                # Browse explicitly so the guard is exercised even if discovery filters it out.
                self.ui.answers["Interrupted CAN campaign"] = "browse"
                self.ui.file.side_effect = None
                self.ui.file.return_value = self.journal
                with self.assertRaises(CampaignError):
                    self.r.prepare("selected")
                self.runner.assert_not_called()
        self.w.info.return_value = self.info
        self.events.insert(-1, dict(self.events[-1], event="COMMIT_REQUEST"))
        self.write_events()
        with self.assertRaises(ValueError):
            self.r.prepare("selected")
        self.runner.assert_not_called()

    def test_inspection_never_applies_or_uploads(self):
        path, _ = self.prepared()
        with patch.object(recovery, "recover", return_value={"result": "INSPECTED"}) as operation, \
                patch.object(recovery, "restore_receiver") as restore:
            self.r.run("recovery-inspect", "selected")
        self.assertEqual(operation.call_count, 1)
        self.assertFalse(operation.call_args.kwargs["apply"])
        restore.assert_not_called()
        self.assertEqual(json.loads((path / "operation.json").read_text())["phase"], "INSPECTED")

    def test_full_workflow_requires_inspection_result_and_restores_before_success(self):
        path, _ = self.prepared()
        order = []
        def operation(*args, **kwargs):
            order.append("apply" if kwargs["apply"] else "inspect")
        def result(*args, **kwargs):
            order.append("receipt")
            return self.result(*args, **kwargs)
        with patch.object(recovery, "recover", side_effect=operation), \
                patch.object(recovery, "collect_result", side_effect=result), \
                patch.object(recovery, "restore_receiver", side_effect=lambda *a, **kw: order.append("restore")), \
                patch.object(recovery.transition, "verify_ota", side_effect=lambda *a: (order.append("verify") or self.verification())):
            self.r.run("recovery-run", "selected")
        self.assertEqual(order, ["inspect", "apply", "receipt", "restore", "verify"])
        self.assertEqual(json.loads((path / "operation.json").read_text())["phase"], "COMPLETE")
        self.assertTrue((path / "verification.json").is_file())

    def test_cancellation_after_inspection_keeps_reusable_archive_without_flash(self):
        path, _ = self.prepared()
        self.ui.cancel_title = "Apply receiver recovery"
        with patch.object(recovery, "recover") as operation, self.assertRaises(Cancelled):
            self.r.run("recovery-run", "selected")
        self.assertEqual(operation.call_count, 1)
        self.assertFalse(operation.call_args.kwargs["apply"])
        self.assertEqual(json.loads((path / "operation.json").read_text())["phase"], "INSPECTED")

    def test_uncertain_helper_upload_is_observed_and_never_replayed(self):
        path, _ = self.prepared("HELPER_INSTALLING")
        with patch.object(recovery, "recover") as operation, \
                patch.object(recovery, "collect_result", side_effect=CampaignError("no result")), \
                patch.object(recovery, "restore_receiver") as restore, self.assertRaises(CampaignError):
            self.r.run("recovery-finish", "selected")
        operation.assert_not_called()
        restore.assert_not_called()
        self.assertEqual(json.loads((path / "operation.json").read_text())["phase"], "HELPER_INSTALLING")

    def test_uncertain_receiver_upload_only_verifies_and_cannot_report_false_success(self):
        path, _ = self.prepared("RECEIVER_INSTALLING")
        with patch.object(recovery, "recover") as operation, patch.object(recovery, "restore_receiver") as restore, \
                patch.object(recovery.transition, "verify_ota", side_effect=CampaignError("wrong firmware")), \
                self.assertRaises(CampaignError):
            self.r.run("recovery-finish", "selected")
        operation.assert_not_called()
        restore.assert_not_called()
        self.ui.show_status.assert_not_called()
        self.assertEqual(json.loads((path / "operation.json").read_text())["phase"], "RECEIVER_INSTALLING")

    def test_archive_tampering_stops_before_any_device_action(self):
        path, _ = self.prepared()
        record = json.loads((path / "board-info.json").read_text())
        record["usb_serial"] = "another-board"
        (path / "board-info.json").write_text(json.dumps(record))
        with patch.object(recovery, "recover") as operation, self.assertRaisesRegex(CampaignError, "identity or USB serial"):
            self.r.run("recovery-run", "selected")
        operation.assert_not_called()

    def test_helper_result_requires_correct_identity_confirmation_and_success(self):
        good = ("OTA_RECOVERY RECOVERED rc=0 EUI=" + IDS[1] + " confirmed=1; outputs inhibited").encode()
        for line in (good, good.replace(IDS[1].encode(), IDS[0].encode()), good.replace(b"confirmed=1", b"confirmed=0"),
                     good.replace(b"RECOVERED rc=0", b"REFUSED rc=-1")):
            console = Mock()
            console.__enter__ = Mock(return_value=console)
            console.__exit__ = Mock(return_value=False)
            console.readline.return_value = line
            factory = Mock(return_value=console)
            output = self.root / "receipt.json"
            with self.subTest(line=line):
                if line == good:
                    recovery.collect_result("selected", IDS[1], output, enumerate_ports=self.enumerate, console_factory=factory)
                    self.assertTrue(output.is_file())
                else:
                    with self.assertRaises(CampaignError):
                        recovery.collect_result("selected", IDS[1], output, enumerate_ports=self.enumerate, console_factory=factory)
                console.write.assert_not_called()

    def test_restore_guards_slots_and_verifies_uploaded_candidate_before_reset(self):
        path, state = self.prepared()
        _, helper, receiver, data = self.r.context(path, state)
        device = Device()
        def reset_slots():
            device.calls.clear()
            device.images = [slot(0, helper["mcuboot_image_hash"], active=True, confirmed=True, version="0.0.1"),
                             slot(1, receiver["mcuboot_image_hash"], version=receiver["version"])]
        def upload(base, snapshot):
            self.assertEqual(snapshot.read_bytes(), data)
            device.images.append(slot(1, receiver["mcuboot_image_hash"], pending=True, version=receiver["version"]))
        uploader = Mock(side_effect=upload)
        def run():
            recovery.restore_receiver("selected", IDS[1], helper, receiver, data, path / "restore.jsonl", self.mcumgr,
                                      enumerate_ports=self.enumerate, transport_factory=Mock(return_value=device), uploader=uploader)
        reset_slots()
        run()
        mutations = [c[:3] for c in device.calls if isinstance(c[0], int)]
        self.assertEqual(mutations, [(2, 1, 5), (2, 0, 5)])
        for fault in ("wrong_primary", "pending", "wrong_secondary", "unconfirmed", "app_reply"):
            reset_slots()
            uploader.reset_mock()
            if fault == "wrong_primary":
                device.images[0]["hash"] = bytes(32)
            if fault == "pending":
                device.images[1]["pending"] = True
            if fault == "wrong_secondary":
                device.images[1]["hash"] = bytes(32)
            if fault == "unconfirmed":
                device.images[0]["confirmed"] = False
            device.app_reply = fault == "app_reply"
            with self.subTest(fault=fault), self.assertRaises((ValueError, CampaignError)):
                run()
            uploader.assert_not_called()
            self.assertFalse(any(isinstance(c[0], int) for c in device.calls))

    def test_restore_wrong_upload_never_resets(self):
        path, state = self.prepared()
        _, helper, receiver, data = self.r.context(path, state)
        device = Device()
        device.images = [slot(0, helper["mcuboot_image_hash"], active=True, confirmed=True),
                         slot(1, receiver["mcuboot_image_hash"])]
        def upload(*args):
            device.images.append(slot(1, "aa" * 32, pending=True))
        with self.assertRaises(ValueError):
            recovery.restore_receiver("selected", IDS[1], helper, receiver, data, path / "restore.jsonl", self.mcumgr,
                                      enumerate_ports=self.enumerate, transport_factory=lambda *a, **kw: device, uploader=upload)
        self.assertNotIn((2, 0, 5, {}), device.calls)


if __name__ == "__main__":
    unittest.main()
