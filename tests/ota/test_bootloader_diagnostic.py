"""Inspect a silent board only after explicit physical entry, without recovery artifacts."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "owntech/tools"))
from lead_update import CampaignError
from ota_workflow import Workflow
from ota_workflow_dialogs import Cancelled
import ota_recovery_workflow as recovery
from smp_transport import CommandError, TransportError


class Clock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class BootloaderDiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.output = self.root / "diagnostic.json"
        self.port = SimpleNamespace(vid=0x2FE3, serial_number="selected", device="COM39")
        self.enumerate = Mock(return_value=[self.port])
        self.clock = Clock()
        self.images = [{"slot": 0, "version": "1.0.0", "hash": b"\x12" * 32,
                        "confirmed": True, "bootable": True},
                       {"slot": 1, "version": "0.0.1", "hash": b"\x34" * 32, "pending": True}]
        self.transport = Mock()
        self.transport.request.side_effect = CommandError("unsupported", response={"rc": 8})
        self.transport.image_state.return_value = {"images": copy.deepcopy(self.images)}
        self.factory = Mock(return_value=self.transport)

    def inspect(self, **kwargs):
        return recovery.inspect_connected_bootloader(
            "selected", self.output, enumerate_ports=self.enumerate, transport_factory=self.factory,
            clock=self.clock, sleep=self.clock.sleep, **kwargs)

    def test_raw_flags_are_saved_without_normalizing_or_mutating_them(self):
        result = self.inspect()
        self.assertEqual(result["image_state"]["images"], self.images)
        record = json.loads(self.output.read_text())
        self.assertEqual(record["usb_serial"], "selected")
        self.assertEqual(record["port"], "COM39")
        self.assertEqual(record["image_state"]["images"][0]["hash"], "12" * 32)
        self.assertNotIn("identity", record)  # No live EUI was read by this service.
        self.assertNotIn("active", record["image_state"]["images"][0])
        self.assertEqual([c[0] for c in self.transport.method_calls], ["request", "image_state", "close"])
        self.transport.request.assert_called_once_with("info")
        text = recovery.bootloader_summary(result)
        self.assertIn("active: not reported", text)
        self.assertIn("pending: true", text)
        self.assertIn("maintenance remain unknown", text)

    def test_empty_image_list_is_preserved_and_never_interpreted_as_empty_flash(self):
        self.transport.image_state.return_value = {"images": []}
        result = self.inspect()
        self.assertEqual(json.loads(self.output.read_text())["image_state"], {"images": []})
        self.assertIn("does not prove that the flash is empty", recovery.bootloader_summary(result))

    def test_waits_for_the_same_serial_after_physical_reset(self):
        self.enumerate.side_effect = [[], [], [self.port], [self.port]]
        self.inspect()
        self.assertEqual(self.factory.call_args.args, ("COM39",))
        self.assertEqual(self.clock.now, 0.5)

    def test_temporary_transport_error_reopens_the_same_board_at_its_new_port(self):
        old = Mock()
        old.request.side_effect = TransportError("USB detached")
        new_port = SimpleNamespace(vid=0x2FE3, serial_number="selected", device="COM40")
        self.enumerate.side_effect = [[self.port], [new_port], [new_port]]
        self.factory.side_effect = [old, self.transport]
        result = self.inspect()
        self.assertEqual(result["port"], "COM40")
        self.assertEqual([c.args[0] for c in self.factory.call_args_list], ["COM39", "COM40"])
        old.close.assert_called_once()
        self.transport.close.assert_called_once()

    def test_a_different_board_is_never_opened_and_absence_wait_is_bounded(self):
        self.enumerate.return_value = [SimpleNamespace(vid=0x2FE3, serial_number="other", device="COM39")]
        with self.assertRaisesRegex(CampaignError, "No bootloader image state"):
            self.inspect(timeout=1)
        self.assertEqual(self.clock.now, 1)
        self.factory.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_persistent_transport_error_is_bounded_and_closes_every_attempt(self):
        self.transport.request.side_effect = TransportError("silent bootloader")
        with self.assertRaisesRegex(CampaignError, "silent bootloader"):
            self.inspect(timeout=1)
        self.assertEqual(self.clock.now, 1)
        self.assertEqual(self.factory.call_count, self.transport.close.call_count)
        self.transport.image_state.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_ambiguous_interfaces_are_refused_before_any_probe(self):
        self.enumerate.return_value = [self.port, SimpleNamespace(vid=0x2FE3, serial_number="selected", device="COM40")]
        with self.assertRaisesRegex(CampaignError, "ambiguous"):
            self.inspect()
        self.factory.assert_not_called()

    def test_live_application_and_unexpected_service_errors_are_not_bootloader_proof(self):
        for error in (None, CommandError("busy", response={"rc": 4})):
            self.transport.reset_mock()
            self.transport.request.side_effect = error
            with self.subTest(error=error), self.assertRaises(CampaignError):
                self.inspect()
            self.transport.image_state.assert_not_called()
            self.transport.close.assert_called_once()
            self.assertFalse(self.output.exists())

    def test_board_change_during_read_cannot_create_a_snapshot(self):
        self.enumerate.side_effect = [[self.port], [SimpleNamespace(vid=0x2FE3, serial_number="other", device="COM39")]]
        with self.assertRaises(CampaignError):
            self.inspect()
        self.transport.close.assert_called_once()
        self.assertFalse(self.output.exists())

    def test_invalid_timeout_never_opens_a_port(self):
        for value in (0, -1, 61, True, float("inf"), float("nan")):
            with self.subTest(value=value), self.assertRaises(CampaignError):
                self.inspect(timeout=value)
        self.factory.assert_not_called()

    def workflow(self):
        ui, runner = Mock(), Mock()
        flow = Workflow(self.root, "OTA_RECOVERY", self.root / "unused-mcumgr", ui,
                        enumerate_ports=self.enumerate, runner=runner)
        flow.info = Mock(side_effect=AssertionError("application must not be required"))
        return flow, ui, runner

    def test_action_needs_no_archive_build_or_application_and_retains_old_operations(self):
        flow, ui, runner = self.workflow()
        archive = flow.operations / "previous" / "operation.json"
        archive.parent.mkdir(parents=True)
        archive.write_text('{"phase":"HELPER_STARTED"}')
        before = archive.read_bytes()
        result = {"usb_serial": "selected", "port": "COM39", "image_state": {"images": []}}
        with patch.object(recovery, "inspect_connected_bootloader", return_value=result) as inspect, \
                patch.object(recovery.RecoveryWorkflow, "choose_operation", side_effect=AssertionError("archive not required")), \
                patch("builtins.print"):
            flow.run("recovery-bootloader-inspect")
        ui.continue_step.assert_called_once()
        self.assertIn("Hold BOOT", ui.continue_step.call_args.args[1])
        inspect.assert_called_once()
        self.assertEqual(inspect.call_args.args[0], "selected")
        self.assertEqual(inspect.call_args.args[1].parent, self.root / "ota-artifacts/diagnostics")
        ui.notice.assert_called_once()
        flow.info.assert_not_called()
        runner.assert_not_called()
        self.assertEqual(archive.read_bytes(), before)

    def test_cancelled_physical_entry_prompt_sends_no_probe(self):
        flow, ui, runner = self.workflow()
        ui.continue_step.side_effect = Cancelled("cancel")
        with patch.object(recovery, "inspect_connected_bootloader") as inspect, self.assertRaises(Cancelled):
            flow.run("recovery-bootloader-inspect")
        inspect.assert_not_called()
        runner.assert_not_called()
        self.assertFalse((self.root / "ota-artifacts").exists())


if __name__ == "__main__":
    unittest.main()
