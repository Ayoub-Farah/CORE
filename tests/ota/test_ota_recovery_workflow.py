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
        with self.assertRaisesRegex(CampaignError, "Cannot use campaign journal"):
            self.r.prepare("selected")
        self.runner.assert_not_called()

    def test_mixed_journal_error_names_selected_file_and_valid_alternative(self):
        wrong = self.journal.with_name("old-mixed.jsonl")
        events = copy.deepcopy(self.events)
        events[-1]["campaign"] += 1
        wrong.write_text("".join(json.dumps(event) + "\n" for event in events))
        original = wrong.read_bytes()
        self.ui.answers["Interrupted CAN campaign"] = "browse"
        self.ui.file.side_effect = None
        self.ui.file.return_value = wrong
        with self.assertRaises(CampaignError) as stopped:
            self.r.prepare("selected")
        self.assertIn("mixed campaign IDs", str(stopped.exception))
        self.assertIn(str(wrong), str(stopped.exception))
        self.assertIn(str(self.journal), str(stopped.exception))
        self.assertEqual(self.ui.file.call_args.args[1], self.w.operations)
        self.assertEqual(wrong.read_bytes(), original)
        self.runner.assert_not_called()

    def test_already_recovered_receiver_stops_before_journal_selection(self):
        self.w.info.return_value = dict(self.info, phase="IDLE", maintenance=False)
        with self.assertRaisesRegex(CampaignError, "connect the next affected receiver"):
            self.r.prepare("selected")
        self.ui.choose.assert_not_called()
        self.ui.file.assert_not_called()
        self.runner.assert_not_called()

    def test_usb_only_receiver_in_maintenance_can_be_prepared_and_inspected(self):
        self.info.update(phase="WAITING_CAN", can_ready=False, healthy=False, error=0)
        path, state = self.prepared()
        config, _, _, _ = self.r.context(path, state)
        self.assertEqual(config["targets"][0]["identity"], IDS[1])
        with patch.object(recovery, "recover") as operation, patch.object(recovery, "restore_receiver") as restore:
            self.r.run("recovery-inspect", "selected")
        self.assertEqual(operation.call_count, 1)
        self.assertFalse(operation.call_args.kwargs["apply"])
        restore.assert_not_called()

    def test_waiting_can_does_not_bypass_maintenance_health_or_campaign_guards(self):
        self.info.update(phase="WAITING_CAN", can_ready=False, healthy=False, error=0)
        config = recovery.recovery_config(self.journal, compact_receiver_only=True)
        for change in ({"maintenance": False}, {"maintenance": None}, {"local_healthy": False},
                       {"active_confirmed": False}, {"mcuboot_image_hash": "ff" * 32},
                       {"identity": IDS[0]}, {"can_ready": True}, {"healthy": True},
                       {"error": -17}, {"error": False}):
            with self.subTest(change=change), self.assertRaises(CampaignError):
                self.r.match_board(dict(self.info, **change), config)
        self.events.insert(-1, dict(self.events[-1], event="COMMIT_REQUEST"))
        self.write_events()
        self.ui.answers["Interrupted CAN campaign"] = "browse"
        self.ui.file.side_effect = None
        self.ui.file.return_value = self.journal
        with self.assertRaisesRegex(CampaignError, "before any commit or reboot"):
            self.r.prepare("selected")
        self.runner.assert_not_called()

    def test_maintenance_with_ineligible_phase_is_not_described_as_already_recovered(self):
        for phase in ("PASS_OPEN", "VERIFYING", "COMMIT_INTENT", "COMMITTED", "REBOOTING", "IDLE"):
            with self.subTest(phase=phase), self.assertRaisesRegex(CampaignError, "still in maintenance") as stopped:
                self.r.require_interrupted_receiver(dict(self.info, phase=phase))
            self.assertNotIn("already recovered", str(stopped.exception))

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
                patch.object(recovery, "verify_restored_receiver", side_effect=lambda *a, **kw: (order.append("verify") or self.verification())):
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
                patch.object(recovery, "verify_restored_receiver", side_effect=CampaignError("wrong firmware")), \
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
            console.readline.return_value = line + b"\n"
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
                console.reset_input_buffer.assert_not_called()

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

    def result_reader(self, chunks):
        """A serial timeout advances simulated time, even when no bytes arrive."""
        from test_pc_campaign import Clock
        clock = Clock()
        values = iter(chunks)
        console = Mock()
        console.__enter__ = Mock(return_value=console)
        console.__exit__ = Mock(return_value=False)
        def read(_):
            clock.sleep(0.2)
            value = next(values, b"")
            if isinstance(value, Exception):
                raise value
            return value
        console.readline.side_effect = read
        return clock, console

    def test_result_reader_preserves_partial_lines_and_startup_output(self):
        line = ("OTA_RECOVERY RECOVERED rc=0 EUI=" + IDS[1] + " confirmed=1; outputs inhibited").encode()
        clock, console = self.result_reader([b"boot banner\r\n", line[:5], b"", line[5:40], line[40:], b"\r\n"])
        output = self.root / "result.json"
        result = recovery.collect_result("selected", IDS[1], output, enumerate_ports=self.enumerate,
                                         console_factory=Mock(return_value=console), clock=clock, sleep=clock.sleep)
        self.assertEqual(result["line"], line.decode())
        self.assertIn("boot banner", output.with_name("result-console.log").read_text())
        console.reset_input_buffer.assert_not_called()
        console.write.assert_not_called()

    def test_result_reader_reopens_silent_interface_without_replaying_upload_or_reset(self):
        line = ("OTA_RECOVERY ALREADY_RECOVERED rc=1 EUI=" + IDS[1] + " confirmed=1; outputs inhibited\n").encode()
        clock, silent = self.result_reader([])
        active = Mock()
        active.__enter__ = Mock(return_value=active)
        active.__exit__ = Mock(return_value=False)
        active.readline.return_value = line
        new_port = SimpleNamespace(device="COM9", vid=0x2FE3, serial_number="selected")
        enumerate_ports = Mock(side_effect=[[self.port], [new_port], [new_port]])
        factory = Mock(side_effect=[silent, active])
        recovery.collect_result("selected", IDS[1], self.root / "result.json", enumerate_ports=enumerate_ports,
                                console_factory=factory, clock=clock, sleep=clock.sleep)
        self.assertEqual([call.args[0] for call in factory.call_args_list], ["COM7", "COM9"])
        self.assertTrue(all(call.kwargs["baudrate"] == 115200 for call in factory.call_args_list))
        self.assertGreaterEqual(clock.now, 3)
        self.assertLess(clock.now, 60)
        for console in (silent, active):
            console.write.assert_not_called()
            console.__exit__.assert_called_once()

    def test_result_reader_waits_for_same_serial_and_recovers_from_disconnection(self):
        line = ("OTA_RECOVERY RECOVERED rc=0 EUI=" + IDS[1] + " confirmed=1; outputs inhibited\n").encode()
        clock, disconnected = self.result_reader([OSError("USB detached")])
        active = Mock()
        active.__enter__ = Mock(return_value=active)
        active.__exit__ = Mock(return_value=False)
        active.readline.return_value = line
        new_port = SimpleNamespace(device="COM10", vid=0x2FE3, serial_number="selected")
        enumerate_ports = Mock(side_effect=[[], [self.port], [], [new_port], [new_port]])
        factory = Mock(side_effect=[disconnected, active])
        output = self.root / "result.json"
        recovery.collect_result("selected", IDS[1], output, enumerate_ports=enumerate_ports,
                                console_factory=factory, clock=clock, sleep=clock.sleep)
        self.assertIn("USB detached", output.with_name("result-console.log").read_text())
        self.assertEqual([call.args[0] for call in factory.call_args_list], ["COM7", "COM10"])
        active.write.assert_not_called()
        disconnected.write.assert_not_called()

    def test_silent_or_unrelated_output_times_out_with_diagnostics_without_success(self):
        for chunks, expected in (([], "No console bytes"), ([b"bootloader banner\n"], "Console data received")):
            clock, console = self.result_reader(chunks)
            output = self.root / "silent.json"
            with self.subTest(chunks=chunks), self.assertRaisesRegex(CampaignError, expected) as stopped:
                recovery.collect_result("selected", IDS[1], output, enumerate_ports=self.enumerate, timeout=2,
                                        console_factory=Mock(return_value=console), clock=clock, sleep=clock.sleep)
            self.assertIn("Inspect recovery boot state" if not chunks else "preserve the console log", str(stopped.exception))
            self.assertFalse(output.exists())
            self.assertTrue(output.with_name("silent-console.log").is_file())
            self.assertLessEqual(clock.now, 2.3)
            console.write.assert_not_called()

    def test_result_reader_does_not_open_a_different_usb_board(self):
        clock, console = self.result_reader([])
        other = SimpleNamespace(device="COM20", vid=0x2FE3, serial_number="other")
        factory = Mock(return_value=console)
        with self.assertRaisesRegex(CampaignError, "No console bytes"):
            recovery.collect_result("selected", IDS[1], self.root / "result.json", enumerate_ports=lambda: [other],
                                    timeout=1, console_factory=factory, clock=clock, sleep=clock.sleep)
        factory.assert_not_called()

    def test_result_reader_rejects_overlong_or_wrong_identity_fragmented_message(self):
        wrong = ("OTA_RECOVERY RECOVERED rc=0 EUI=" + IDS[0] + " confirmed=1; outputs inhibited\n").encode()
        for chunks in ([wrong[:10], wrong[10:]], [b"x" * 513]):
            clock, console = self.result_reader(chunks)
            output = self.root / "result.json"
            with self.subTest(chunks=chunks), self.assertRaises(CampaignError):
                recovery.collect_result("selected", IDS[1], output, enumerate_ports=self.enumerate,
                                        console_factory=Mock(return_value=console), clock=clock, sleep=clock.sleep)
            self.assertFalse(output.exists())
            console.write.assert_not_called()

    def test_boot_state_inspection_classifies_images_without_any_mutation(self):
        path, state = self.prepared("HELPER_STARTED")
        _, helper, receiver, _ = self.r.context(path, state)
        h, r = helper["mcuboot_image_hash"], receiver["mcuboot_image_hash"]
        cases = [
            ([slot(0, r, active=True, confirmed=True)], "ORIGINAL_ONLY"),
            ([slot(0, r, active=True, confirmed=True), slot(1, h, pending=True)], "HELPER_PENDING"),
            ([slot(0, r, active=False, confirmed=True), slot(1, h, pending=True)], "HELPER_PENDING"),
            ([slot(0, r, active=True, confirmed=True), slot(1, h)], "ORIGINAL_WITH_HELPER_BACKUP"),
            ([slot(0, h, active=True, confirmed=True), slot(1, r)], "HELPER_CONFIRMED"),
            ([slot(0, h, active=True), slot(1, r)], "HELPER_UNCONFIRMED"),
            ([slot(0, "dd" * 32, active=True, confirmed=True), slot(1, r)], "UNEXPECTED"),
        ]
        for images, expected in cases:
            device = Device()
            device.images = images
            output = path / "diagnostic.json"
            with self.subTest(expected=expected):
                result = recovery.inspect_boot_state("selected", IDS[1], helper, receiver, output,
                                                     enumerate_ports=self.enumerate, transport_factory=lambda *a, **kw: device)
                self.assertEqual(result["diagnosis"], expected)
                self.assertEqual(json.loads(output.read_text())["diagnosis"], expected)
                self.assertEqual(device.calls, [("info",), ("image_state",), ("close",)])
                if expected == "HELPER_CONFIRMED":
                    self.assertIn("confirmation alone is insufficient", result["explanation"])

    def test_boot_state_inspection_preserves_empty_list_for_diagnosis(self):
        path, state = self.prepared("HELPER_STARTED")
        _, helper, receiver, _ = self.r.context(path, state)
        device = Device()
        device.images = []
        output = path / "diagnostic.json"
        with self.assertRaisesRegex(ValueError, "empty or invalid"):
            recovery.inspect_boot_state("selected", IDS[1], helper, receiver, output,
                                        enumerate_ports=self.enumerate, transport_factory=lambda *a, **kw: device)
        self.assertEqual(json.loads(output.read_text())["image_state"]["images"], [])
        self.assertEqual(device.calls, [("info",), ("image_state",), ("close",)])

    def test_boot_state_action_preserves_phase_and_never_starts_build_or_recovery(self):
        path, state = self.prepared("HELPER_INSTALLING")
        before = (path / "operation.json").read_bytes()
        self.runner.reset_mock()
        with patch.object(recovery, "inspect_boot_state", return_value={"diagnosis": "HELPER_PENDING", "explanation": "Pending"}) as inspect, \
                patch.object(recovery, "recover") as apply, patch.object(recovery, "restore_receiver") as restore, \
                patch.object(recovery, "collect_result") as collect:
            self.r.run("recovery-boot-state", "selected")
        inspect.assert_called_once()
        self.assertEqual(inspect.call_args.args[:2], ("selected", IDS[1]))
        self.assertEqual((path / "operation.json").read_bytes(), before)
        self.runner.assert_not_called()
        apply.assert_not_called()
        restore.assert_not_called()
        collect.assert_not_called()
        self.assertEqual(self.ui.notice.call_args.args[0], "Recovery boot state")

    def test_cancelled_boot_state_prompt_never_opens_port(self):
        self.prepared("HELPER_STARTED")
        self.ui.cancel_title = "Inspect recovery boot state"
        with patch.object(recovery, "inspect_boot_state") as inspect, self.assertRaises(Cancelled):
            self.r.run("recovery-boot-state", "selected")
        inspect.assert_not_called()

    def test_boot_state_action_rejects_live_application_and_wrong_serial(self):
        path, state = self.prepared("HELPER_STARTED")
        _, helper, receiver, _ = self.r.context(path, state)
        device = Device()
        device.app_reply = True
        factory = Mock(return_value=device)
        with self.assertRaisesRegex(CampaignError, "application still replies"):
            recovery.inspect_boot_state("selected", IDS[1], helper, receiver, path / "diagnostic.json",
                                        enumerate_ports=self.enumerate, transport_factory=factory)
        self.assertEqual(device.calls, [("info",), ("close",)])
        factory.reset_mock()
        with self.assertRaises(CampaignError):
            recovery.inspect_boot_state("other", IDS[1], helper, receiver, path / "diagnostic.json",
                                        enumerate_ports=self.enumerate, transport_factory=factory)
        factory.assert_not_called()

    def ready_info(self, **changes):
        return dict(self.info, **dict(dict(service="owntech-ota", protocol=2, phase="IDLE", maintenance=False,
                                         healthy=True, can_ready=True, error=0, available=True, slot_available=True,
                                         hardware_id=self.original["hardware_id"], layout_id=self.original["layout_id"],
                                         bootloader_id=self.original["bootloader_id"],
                                         slot_size=self.original["profile"]["slot_size"],
                                         useful_capacity=self.original["profile"]["useful_capacity"]), **changes))

    def test_final_verification_waits_for_usb_then_boot_then_confirmed_receiver(self):
        from test_pc_campaign import Clock
        clock = Clock()
        transport = Mock()
        transport.request.side_effect = [self.ready_info(phase="BOOT"), self.ready_info()]
        factory = Mock(return_value=transport)
        enumerate_ports = Mock(side_effect=[[], [], [self.port], [self.port], [self.port], [self.port]])
        result = recovery.verify_restored_receiver("selected", IDS[1], self.original, enumerate_ports=enumerate_ports,
                                                  receiver_factory=factory, clock=clock, sleep=clock.sleep)
        self.assertEqual(result["result"], "OTA_VERIFIED")
        self.assertGreaterEqual(clock.now, 0.75)
        self.assertEqual(factory.call_count, 2)
        self.assertEqual(transport.method_calls, [("request", ("info",), {}), ("close", (), {}),
                                                ("request", ("info",), {}), ("close", (), {})])

    def test_final_verification_retries_disconnection_and_new_com_for_same_serial(self):
        from test_pc_campaign import Clock
        clock = Clock()
        transport = Mock(request=Mock(side_effect=[OSError("detached"), self.ready_info(), self.ready_info()]))
        new_port = SimpleNamespace(device="COM12", vid=0x2FE3, serial_number="selected")
        enumerate_ports = Mock(side_effect=[[self.port], [self.port], [], [new_port], [new_port]])
        factory = Mock(return_value=transport)
        result = recovery.verify_restored_receiver("selected", IDS[1], self.original, enumerate_ports=enumerate_ports,
                                                  receiver_factory=factory, clock=clock, sleep=clock.sleep)
        self.assertEqual(result["result"], "OTA_VERIFIED")
        self.assertEqual([c.args[0] for c in factory.call_args_list], ["COM7", "COM7", "COM12"])
        self.assertEqual(transport.close.call_count, 3)

    def test_final_verification_accepts_clean_receiver_waiting_for_can(self):
        info = self.ready_info(phase="WAITING_CAN", healthy=False, can_ready=False, available=False)
        transport = Mock(request=Mock(return_value=info))
        result = recovery.verify_restored_receiver("selected", IDS[1], self.original, enumerate_ports=self.enumerate,
                                                  receiver_factory=Mock(return_value=transport))
        self.assertEqual(result["info"], info)
        transport.close.assert_called_once()

    def test_final_verification_rejects_wrong_image_identity_maintenance_and_health_without_retry(self):
        for change in ({"identity": IDS[0]}, {"mcuboot_image_hash": "dd" * 32}, {"maintenance": True},
                       {"phase": "FAILED"}, {"local_healthy": False}, {"error": -1}, {"image_class": "lead"}):
            transport = Mock(request=Mock(return_value=self.ready_info(**change)))
            factory = Mock(return_value=transport)
            with self.subTest(change=change), self.assertRaises((CampaignError, ValueError)):
                recovery.verify_restored_receiver("selected", IDS[1], self.original, enumerate_ports=self.enumerate,
                                                  receiver_factory=factory)
            factory.assert_called_once()
            self.assertEqual(transport.method_calls, [("request", ("info",), {}), ("close", (), {})])

    def test_final_verification_absence_is_bounded_and_does_not_open_other_board(self):
        from test_pc_campaign import Clock
        clock = Clock()
        other = SimpleNamespace(device="COM20", vid=0x2FE3, serial_number="other")
        factory = Mock()
        with self.assertRaisesRegex(CampaignError, "selected did not become ready"):
            recovery.verify_restored_receiver("selected", IDS[1], self.original, enumerate_ports=lambda: [other],
                                              timeout=1, receiver_factory=factory, clock=clock, sleep=clock.sleep)
        factory.assert_not_called()
        self.assertEqual(clock.now, 1)

    def test_final_verification_duplicate_interfaces_fail_before_any_request(self):
        second = SimpleNamespace(device="COM8", vid=0x2FE3, serial_number="selected")
        factory = Mock()
        with self.assertRaisesRegex(CampaignError, "found 2"):
            recovery.verify_restored_receiver("selected", IDS[1], self.original,
                                              enumerate_ports=lambda: [self.port, second], receiver_factory=factory)
        factory.assert_not_called()

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
