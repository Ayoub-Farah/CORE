"""Exercise workflow sequencing without opening a window or touching hardware."""
from contextlib import redirect_stderr, redirect_stdout
import copy
import io
import json
from pathlib import Path
import shutil
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
import test_pc_transition_usb as fixture_module
from ota_workflow_dialogs import Cancelled
from lead_update import CampaignError, prepare_manifest
from smp_transport import TransportError
import ota_workflow as workflow


class FakeUI:
    def __init__(self):
        self.answers = {}
        self.cancel_title = None
        self.choose = Mock(side_effect=self._choose)
        self.continue_step = Mock(side_effect=self._continue)
        self.file = Mock(side_effect=AssertionError("unexpected file picker"))
        self.folder = Mock(side_effect=AssertionError("unexpected folder picker"))
        self.number = Mock(return_value=2)
        self.notice = Mock()
        self.problem = Mock()
        self.show_status = Mock()
        self.close = Mock()

    def _continue(self, title, message):
        if title == self.cancel_title:
            raise Cancelled(title)

    def _choose(self, title, message, items):
        if title == self.cancel_title:
            raise Cancelled(title)
        if title not in self.answers:
            raise AssertionError("unexpected choice: " + title)
        answer = self.answers[title]
        if isinstance(answer, list):
            answer = answer.pop(0)
        if answer not in [value for value, label in items]:
            raise AssertionError("answer is outside readonly choices: " + str(answer))
        return answer


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixture_module.TransitionTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root.resolve()
        self.ui = FakeUI()
        self.runner = Mock()
        self.enumerate = Mock(return_value=[self.fixture.port])
        self.workflow = workflow.Workflow(self.root, "OTA", self.fixture.mcumgr, self.ui,
                                          enumerate_ports=self.enumerate, runner=self.runner)
        self.output = io.StringIO()
        self.silence = redirect_stdout(self.output)
        self.silence.__enter__()
        self.addCleanup(self.silence.__exit__, None, None, None)
        # A accidental prompt should fail instead of hanging automation.
        self.stdin = patch("builtins.input", side_effect=AssertionError("terminal entry is forbidden"))
        self.stdin.start()
        self.addCleanup(self.stdin.stop)

    def archive(self, phase="PREPARED"):
        path, state = self.workflow.create("selected", "usb-roundtrip")
        state["identity"] = self.fixture.config["identity"]
        workflow.copy_pair(self.fixture.source_path, self.fixture.source_manifest_path, path / "source")
        workflow.copy_pair(self.fixture.helper_path, self.fixture.helper_manifest_path, path / "helper")
        workflow.copy_pair(self.fixture.usb_path, self.fixture.usb_manifest_path, path / "usb")
        (path / "config").mkdir()
        for suffix in ("json", "h"):
            shutil.copyfile(self.fixture.config_path.with_suffix("." + suffix),
                            path / ("config/owntech_ota_recovery_config." + suffix))
        workflow.transition._save(path / "board-info.json", self.fixture.info)
        self.workflow.save(path, state, phase)
        return path, state

    def receipt(self, path):
        workflow.transition._save(path / "cleanup-receipt.json",
                                  workflow.transition.receipt_payload(self.fixture.config, self.fixture.helper,
                                                                        self.fixture.completion()))

    def select_archive(self, path, state):
        self.workflow.choose_operation = Mock(return_value=(path, state))

    def state(self, path):
        return workflow.transition._json(path / "operation.json")

    def test_board_selection_groups_cdc_ports_but_requires_choice_between_boards(self):
        self.enumerate.return_value = [self.fixture.port,
                                      SimpleNamespace(vid=0x2FE3, serial_number="selected", device="COM8")]
        self.assertEqual(self.workflow.board(), "selected")
        self.ui.choose.assert_not_called()
        self.enumerate.return_value += [SimpleNamespace(vid=0x2FE3, serial_number="other", device="COM9"),
                                       SimpleNamespace(vid=0x1234, serial_number="foreign", device="COM10")]
        self.ui.answers["Select board"] = "other"
        self.assertEqual(self.workflow.board(), "other")
        options = self.ui.choose.call_args.args[2]
        self.assertEqual([value for value, label in options], ["other", "selected"])
        self.assertIn("COM7, COM8", options[1][1])

    def test_no_identified_board_and_cancelled_board_choice_do_not_start_actions(self):
        self.enumerate.return_value = [SimpleNamespace(vid=0x2FE3, serial_number=None, device="COM7")]
        with self.assertRaises(CampaignError):
            self.workflow.board()
        self.enumerate.return_value = [self.fixture.port, SimpleNamespace(vid=0x2FE3, serial_number="other", device="COM8")]
        self.ui.cancel_title = "Select board"
        self.workflow.initialize = Mock()
        with self.assertRaises(Cancelled):
            self.workflow.run("initialize")
        self.workflow.initialize.assert_not_called()
        self.runner.assert_not_called()

    def test_archive_selection_rejects_another_serial_kind_and_completed_operation(self):
        path, state = self.archive()
        self.ui.answers["Operation archive"] = "browse"
        self.ui.folder.side_effect = None
        self.ui.folder.return_value = path
        for field, value in (("serial", "different"), ("kind", "can-update"), ("phase", "OTA_READY"), ("schema_version", 99)):
            bad = dict(state, **{field: value})
            workflow.transition._save(path / "operation.json", bad)
            with self.subTest(field=field), self.assertRaises(CampaignError):
                self.workflow.choose_operation("selected")
        self.runner.assert_not_called()

    def test_existing_archives_omit_other_boards_and_completed_or_unprepared_roundtrips(self):
        path, state = self.archive("USB_READY")
        for serial, kind, phase in (("other", "usb-roundtrip", "USB_READY"),
                                    ("selected", "usb-roundtrip", "OTA_READY"),
                                    ("selected", "usb-roundtrip", "CREATED"),
                                    ("selected", "can-update", "UPDATING")):
            other, value = self.workflow.create(serial, kind)
            self.workflow.save(other, value, phase)
        self.assertEqual(self.workflow.existing("selected"), [(path, state)])

    def test_unknown_roundtrip_phase_cannot_upload(self):
        path, state = self.archive("UNRECOGNIZED")
        self.select_archive(path, state)
        with patch.object(workflow.transition, "bootloader_step") as boot, self.assertRaises(CampaignError):
            self.workflow.to_usb("selected")
        boot.assert_not_called()

    def test_snapshot_collision_refuses_to_replace_any_existing_bytes(self):
        destination = self.root / "snapshot"
        workflow.copy_pair(self.fixture.usb_path, self.fixture.usb_manifest_path, destination)
        expected = {file.name: file.read_bytes() for file in destination.iterdir()}
        workflow.copy_pair(self.fixture.usb_path, self.fixture.usb_manifest_path, destination)
        with self.assertRaisesRegex(CampaignError, "cannot be overwritten"):
            workflow.copy_pair(self.fixture.helper_path, self.fixture.helper_manifest_path, destination)
        self.assertEqual({file.name: file.read_bytes() for file in destination.iterdir()}, expected)

    def test_mismatched_snapshot_is_rejected_before_destination_creation(self):
        destination = self.root / "bad-snapshot"
        with self.assertRaisesRegex(CampaignError, "do not match"):
            workflow.copy_pair(self.fixture.helper_path, self.fixture.usb_manifest_path, destination)
        self.assertFalse(destination.exists())

    def test_snapshot_resolves_custom_program_name_and_rejects_filename_escape(self):
        folder = self.root / "ota-artifacts/OTA"
        folder.mkdir(parents=True)
        image = folder / "custom-name.mcuboot.bin"
        manifest = folder / "custom-name.mcuboot.json"
        image.write_bytes(self.fixture.source_data)
        workflow.transition._save(manifest, dict(self.fixture.source, filename=image.name))
        archived, _ = self.workflow.snapshot("OTA", self.root / "custom-snapshot")
        self.assertEqual(archived.read_bytes(), self.fixture.source_data)
        workflow.transition._save(manifest, dict(self.fixture.source, filename="../outside.bin"))
        with self.assertRaises(CampaignError):
            self.workflow.snapshot("OTA", self.root / "escape-snapshot")
        self.assertFalse((self.root / "escape-snapshot").exists())

    def test_nested_build_uses_platformio_interpreter_not_gui_interpreter(self):
        selected = str(self.root / "platformio-python.exe")
        instance = workflow.Workflow(self.root, "OTA", self.fixture.mcumgr, self.ui,
                                      enumerate_ports=self.enumerate, runner=self.runner, pio_python=selected)
        instance.build("OTA_TRANSITION")
        self.runner.assert_called_once_with([selected, "-m", "platformio", "run", "-d", str(self.root),
                                             "-e", "OTA_TRANSITION", "-t", "mcuboot-image"],
                                            check=True, cwd=self.root)

    def test_source_uses_exact_known_history_and_never_scans_pio_sizing_copies(self):
        sizing = self.root / ".pio/mm4/ota-artifacts/OTA"
        sizing.mkdir(parents=True)
        shutil.copyfile(self.fixture.source_path, sizing / "firmware.mcuboot.bin")
        shutil.copyfile(self.fixture.source_manifest_path, sizing / "firmware.mcuboot.json")
        self.workflow.pick_image = Mock(side_effect=Cancelled("no source selected"))
        with self.assertRaises(Cancelled):
            self.workflow.source(self.fixture.info)
        self.workflow.pick_image.assert_called_once()
        history = self.root / "ota-artifacts/history/OTA/known"
        history.mkdir(parents=True)
        shutil.copyfile(self.fixture.source_path, history / "firmware.bin")
        shutil.copyfile(self.fixture.source_manifest_path, history / "firmware.json")
        self.workflow.pick_image.reset_mock()
        self.assertEqual(self.workflow.source(self.fixture.info), (history / "firmware.bin", history / "firmware.json"))
        self.workflow.pick_image.assert_not_called()

    def test_prepare_caches_original_source_before_any_build_and_validates_helper(self):
        path, state = self.workflow.create("selected", "usb-roundtrip")
        self.workflow.source = Mock(return_value=(self.fixture.source_path, self.fixture.source_manifest_path))
        self.workflow.history = Mock(return_value={"no_campaign": True})
        order = []
        def capture(serial, output):
            self.assertEqual(serial, "selected")
            workflow.transition._save(output, self.fixture.info)
        def build(environment):
            order.append(environment)
            self.assertEqual((path / "source/image.bin").read_bytes(), self.fixture.source_data)
            if environment == "OTA_TRANSITION":
                # The archived original must survive a later environment build.
                self.fixture.source_path.write_bytes(b"replaced build output")
        def snapshot(environment, destination):
            if environment == "OTA_TRANSITION":
                return workflow.copy_pair(self.fixture.helper_path, self.fixture.helper_manifest_path, destination)
            return workflow.copy_pair(self.fixture.usb_path, self.fixture.usb_manifest_path, destination)
        self.workflow.build = Mock(side_effect=build)
        self.workflow.snapshot = Mock(side_effect=snapshot)
        with patch.object(workflow.transition, "capture", side_effect=capture):
            self.workflow.prepare(path, state)
        self.assertEqual(order, ["OTA_TRANSITION", "USB"])
        self.assertEqual(self.state(path)["phase"], "PREPARED")
        self.assertEqual(self.workflow.context(path, state)[0]["source_image_sha256"], self.fixture.config["source_image_sha256"])

    def test_cancel_switch_before_preparation_or_physical_boot_never_uploads(self):
        for phase, title in (("CREATED", "Switch to USB"), ("PREPARED", "Install cleanup application")):
            path, state = self.archive(phase)
            self.select_archive(path, state)
            self.ui.cancel_title = title
            with patch.object(self.workflow, "prepare") as prepare, patch.object(workflow.transition, "bootloader_step") as boot:
                with self.subTest(phase=phase), self.assertRaises(Cancelled):
                    self.workflow.to_usb("selected")
                prepare.assert_not_called()
                boot.assert_not_called()
            self.assertEqual(self.state(path)["phase"], phase)

    def test_complete_switch_preserves_stage_before_each_mutation_and_receipt_before_usb(self):
        path, state = self.archive()
        self.select_archive(path, state)
        events = []
        def boot(config, helper, stage, **kwargs):
            self.assertEqual(config["token"], self.fixture.config["token"])
            self.assertEqual(kwargs["log_path"], path / "transition.jsonl")
            self.assertIs(kwargs["enumerate_ports"], self.enumerate)
            events.append((stage, kwargs.get("apply", False)))
            if stage != "install-helper":
                workflow.transition.verify_receipt(kwargs["receipt"], config, helper)
            if kwargs.get("apply"):
                expected = {"install-helper": "HELPER_INSTALLING", "install-usb": "USB_INSTALLING", "verify-usb": "USB_STARTED"}
                self.assertEqual(self.state(path)["phase"], expected[stage])
            return {"result": "RESET_REQUESTED"}
        def collect(config, helper, output, **kwargs):
            self.assertEqual(self.state(path)["phase"], "HELPER_STARTED")
            self.assertEqual(output, path / "cleanup-receipt.json")
            self.receipt(path)
            events.append(("receipt", False))
        with patch.object(workflow.transition, "bootloader_step", side_effect=boot), \
             patch.object(workflow.transition, "collect_receipt", side_effect=collect):
            self.workflow.to_usb("selected")
        self.assertEqual(events, [("install-helper", False), ("install-helper", True), ("receipt", False),
                                  ("install-usb", False), ("install-usb", True), ("verify-usb", True)])
        self.assertEqual(self.state(path)["phase"], "USB_READY")

    def test_helper_upload_ack_loss_preserves_installing_stage_and_never_installs_usb(self):
        path, state = self.archive()
        self.select_archive(path, state)
        def boot(config, helper, stage, **kwargs):
            self.assertEqual(stage, "install-helper")
            if kwargs.get("apply"):
                self.assertEqual(self.state(path)["phase"], "HELPER_INSTALLING")
                raise TransportError("reset reply lost")
        with patch.object(workflow.transition, "bootloader_step", side_effect=boot) as boot_mock, \
             patch.object(workflow.transition, "collect_receipt") as collect:
            with self.assertRaises(TransportError):
                self.workflow.to_usb("selected")
        self.assertEqual(boot_mock.call_count, 2)
        collect.assert_not_called()
        self.assertEqual(self.state(path)["phase"], "HELPER_INSTALLING")

    def test_missing_or_failed_cleanup_receipt_stops_before_usb_install(self):
        path, state = self.archive("HELPER_STARTED")
        self.select_archive(path, state)
        with patch.object(workflow.transition, "collect_receipt", side_effect=ValueError("helper refused cleanup")), \
             patch.object(workflow.transition, "bootloader_step") as boot:
            with self.assertRaises(ValueError):
                self.workflow.to_usb("selected")
        boot.assert_not_called()
        self.assertEqual(self.state(path)["phase"], "HELPER_STARTED")
        self.workflow.save(path, state, "CLEANED")
        # Missing receipt is rejected by the real underlying client before USB.
        with patch.object(workflow.transition, "SerialSMP") as serial, self.assertRaises(OSError):
            self.workflow.to_usb("selected")
        serial.assert_not_called()

    def test_real_sparse_pending_resume_uses_original_operation_log_and_exact_guards(self):
        path, state = self.archive("HELPER_INSTALLING")
        fixture = self.fixture
        fixture.log = path / "transition.jsonl"
        fixture.device.wire_flags = True
        fixture.device.images.append(fixture_module.slot(1, fixture.helper, pending=True))
        fixture.record_upload()
        real_boot = workflow.transition.bootloader_step
        def boot(config, helper, stage, **kwargs):
            return real_boot(config, helper, stage, transport_factory=fixture.factory, uploader=fixture.uploader, **kwargs)
        with patch.object(workflow.transition, "bootloader_step", side_effect=boot) as calls:
            result = self.workflow.install(path, state, "install-helper", fixture.config, fixture.helper,
                                            fixture.helper, fixture.helper_data)
        self.assertEqual(result["result"], "RESET_REQUESTED")
        self.assertEqual([(call.kwargs.get("resume", False), call.kwargs.get("apply", False))
                          for call in calls.call_args_list], [(False, False), (True, False), (True, True)])
        fixture.uploader.assert_not_called()
        self.assertEqual(fixture.mutations(), [(2, 0, 5, {})])

    def test_resume_wrong_pending_image_never_uploads_or_resets(self):
        path, state = self.archive("HELPER_INSTALLING")
        fixture = self.fixture
        fixture.log = path / "transition.jsonl"
        fixture.device.wire_flags = True
        fixture.device.images.append(fixture_module.slot(1, fixture.usb, pending=True))
        fixture.record_upload()
        real_boot = workflow.transition.bootloader_step
        def boot(config, helper, stage, **kwargs):
            return real_boot(config, helper, stage, transport_factory=fixture.factory, uploader=fixture.uploader, **kwargs)
        with patch.object(workflow.transition, "bootloader_step", side_effect=boot), self.assertRaises(ValueError):
            self.workflow.install(path, state, "install-helper", fixture.config, fixture.helper,
                                   fixture.helper, fixture.helper_data)
        fixture.uploader.assert_not_called()
        self.assertEqual(fixture.mutations(), [])

    def test_resume_cannot_borrow_a_different_operations_log(self):
        path, state = self.archive("HELPER_INSTALLING")
        fixture = self.fixture
        # Valid preparation/upload evidence exists, but outside this operation.
        fixture.record_upload()
        fixture.device.wire_flags = True
        fixture.device.images.append(fixture_module.slot(1, fixture.helper, pending=True))
        real_boot = workflow.transition.bootloader_step
        def boot(config, helper, stage, **kwargs):
            return real_boot(config, helper, stage, transport_factory=fixture.factory, uploader=fixture.uploader, **kwargs)
        with patch.object(workflow.transition, "bootloader_step", side_effect=boot), self.assertRaises(ValueError):
            self.workflow.install(path, state, "install-helper", fixture.config, fixture.helper,
                                   fixture.helper, fixture.helper_data)
        fixture.uploader.assert_not_called()
        self.assertEqual(fixture.mutations(), [])

    def test_return_ota_verifies_current_usb_without_reset_and_persists_before_upload(self):
        path, state = self.archive("USB_READY")
        self.receipt(path)
        self.select_archive(path, state)
        self.ui.answers["Current USB application"] = "saved"
        self.workflow.build = Mock()
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        stages = []
        def boot(config, helper, stage, **kwargs):
            self.assertEqual(kwargs["log_path"], path / "transition.jsonl")
            self.assertEqual(kwargs["usb"]["mcuboot_image_hash"], self.fixture.usb["mcuboot_image_hash"])
            workflow.transition.verify_receipt(kwargs["receipt"], config, helper)
            stages.append((stage, kwargs.get("apply", False)))
            if kwargs.get("apply"):
                self.assertEqual(self.state(path)["phase"], "OTA_INSTALLING")
            return {"result": "verified"}
        with patch.object(workflow.transition, "bootloader_step", side_effect=boot), \
             patch.object(workflow.transition, "verify_ota", return_value={"info": self.fixture.info}) as verify:
            self.workflow.to_ota("selected")
        self.assertEqual(stages, [("verify-usb", False), ("return-ota", False), ("return-ota", True)])
        self.assertEqual(self.state(path)["phase"], "OTA_READY")
        self.assertTrue((path / state["current_usb"] / "image.bin").is_file())
        verify.assert_called_once()
        self.ui.show_status.assert_called_once()

    def test_return_missing_receipt_or_cancel_at_boot_has_no_upload(self):
        path, state = self.archive("USB_READY")
        self.select_archive(path, state)
        self.workflow.build = Mock()
        with patch.object(workflow.transition, "bootloader_step") as boot, self.assertRaises(OSError):
            self.workflow.to_ota("selected")
        boot.assert_not_called()
        self.workflow.build.assert_not_called()
        self.receipt(path)
        self.ui.answers["Current USB application"] = "saved"
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        self.ui.cancel_title = "Verify USB and install OTA V2"
        with patch.object(workflow.transition, "bootloader_step") as boot, self.assertRaises(Cancelled):
            self.workflow.to_ota("selected")
        boot.assert_not_called()
        self.assertEqual(self.state(path)["phase"], "RETURN_PREPARED")

    def test_return_ack_loss_retains_exact_saved_image_and_installing_stage(self):
        path, state = self.archive("USB_READY")
        self.receipt(path)
        self.select_archive(path, state)
        self.ui.answers["Current USB application"] = "saved"
        self.workflow.build = Mock()
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        def boot(config, helper, stage, **kwargs):
            if kwargs.get("apply"):
                raise TransportError("reset ACK lost")
        with patch.object(workflow.transition, "bootloader_step", side_effect=boot), \
             patch.object(workflow.transition, "verify_ota") as verify, self.assertRaises(TransportError):
            self.workflow.to_ota("selected")
        self.assertEqual(self.state(path)["phase"], "OTA_INSTALLING")
        self.assertEqual((path / "return/image.bin").read_bytes(), self.fixture.source_data)
        verify.assert_not_called()

    def test_return_rejects_environment_change_and_usb_revision_path_escape(self):
        path, state = self.archive("RETURN_PREPARED")
        self.receipt(path)
        self.select_archive(path, state)
        for fields in ({"return_environment": "USB_LEAD", "current_usb": "usb"},
                       {"return_environment": "OTA", "current_usb": "../../../../outside"}):
            state.update(fields)
            with patch.object(workflow.transition, "bootloader_step") as boot, self.assertRaises(CampaignError):
                self.workflow.to_ota("selected")
            boot.assert_not_called()

    def test_return_preserves_original_receiver_role_before_any_build(self):
        path, state = self.archive("USB_READY")
        self.select_archive(path, state)
        self.workflow.environment = "USB_LEAD"
        self.workflow.build = Mock()
        with patch.object(workflow.transition, "bootloader_step") as boot, self.assertRaisesRegex(CampaignError, "same role"):
            self.workflow.to_ota("selected")
        self.workflow.build.assert_not_called()
        boot.assert_not_called()

    def test_after_return_ack_loss_already_running_ota_is_only_verified(self):
        path, state = self.archive("OTA_INSTALLING")
        self.receipt(path)
        self.select_archive(path, state)
        state.update(current_usb="usb", return_environment="OTA")
        self.workflow.save(path, state)
        workflow.copy_pair(self.fixture.source_path, self.fixture.source_manifest_path, path / "return")
        self.ui.answers["Continue return to OTA"] = "verify"
        self.workflow.build = Mock()
        with patch.object(workflow.transition, "bootloader_step") as boot, \
             patch.object(workflow.transition, "verify_ota", return_value={"info": self.fixture.info}) as verify:
            self.workflow.to_ota("selected")
        boot.assert_not_called()
        self.workflow.build.assert_not_called()
        verify.assert_called_once()
        self.assertEqual(self.state(path)["phase"], "OTA_READY")

    def test_cancel_initialization_never_calls_provision(self):
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        self.ui.cancel_title = "Initialize over USB"
        with patch.object(workflow, "provision_main") as provision, self.assertRaises(Cancelled):
            self.workflow.initialize("selected")
        provision.assert_not_called()

    def test_initialization_preserves_compiled_manifest_build_identity_for_provision(self):
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        self.workflow.info = Mock(return_value=self.fixture.info)
        def provision(args, *, raise_errors=False):
            self.assertTrue(raise_errors)
            image = Path(args[args.index("--image") + 1])
            self.assertEqual(args[args.index("--serial") + 1], "selected")
            self.assertEqual(args[args.index("--image-class") + 1], "receiver")
            self.assertIn("--legacy-console", args)
            self.assertEqual(image.with_suffix(".json").read_bytes(), (image.parent / "manifest.json").read_bytes())
            data, manifest = prepare_manifest(image, image_class="receiver", usb=True)
            self.assertEqual(data, self.fixture.source_data)
            self.assertEqual(manifest["build_id"], "source-v2")
            self.assertEqual(manifest["profile"], self.fixture.source["profile"])
            self.assertEqual(self.state(image.parent.parent)["phase"], "INSTALLING")
            return 0
        with patch.object(workflow, "provision_main", side_effect=provision) as run:
            self.workflow.initialize("selected")
        run.assert_called_once()
        self.ui.show_status.assert_called_once()

    def test_status_receiver_only_reads_info_and_lead_adds_readonly_fleet(self):
        for role in ("receiver", "lead"):
            transport = Mock(request=Mock(return_value=dict(self.fixture.info, image_class=role)))
            connection = Mock(connect=Mock(return_value=transport))
            with patch.object(workflow, "USBConnection", return_value=connection) as factory, \
                 patch.object(workflow, "read_only_status", return_value={"status": {"targets": []}}) as fleet:
                self.workflow.status("selected")
            factory.assert_called_once_with("selected")
            transport.request.assert_called_once_with("info")
            transport.close.assert_called_once()
            if role == "lead":
                fleet.assert_called_once_with(transport)
            else:
                fleet.assert_not_called()
            self.assertEqual(self.ui.show_status.call_args.args[1]["usb_serial"], "selected")

    def test_initialization_offers_one_manual_boot_entry_only_after_preupload_timeout(self):
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        self.workflow.info = Mock(return_value=self.fixture.info)
        with patch.object(workflow, "provision_main", side_effect=[workflow.BootloaderNotReady("COM11: Write timeout"), 0]) as provision:
            self.workflow.initialize("selected")
        self.assertEqual(provision.call_count, 2)
        first, second = [call.args[0] for call in provision.call_args_list]
        self.assertEqual(first[:-1], second[:-1])
        self.assertEqual(first[-1], "--legacy-console")
        self.assertEqual(second[-1], "--bootloader")
        self.assertTrue(all(call.kwargs["raise_errors"] for call in provision.call_args_list))
        prompt = self.ui.continue_step.call_args.args
        self.assertEqual(prompt[0], "Initialize OTA V2 through USB bootloader")
        self.assertIn("No firmware was sent", prompt[1])
        self.assertIn("Hold BOOT", prompt[1])
        self.assertIn("COM11: Write timeout", prompt[1])
        self.ui.show_status.assert_called_once()

    def test_initialization_manual_entry_cancel_or_second_timeout_never_retries_again(self):
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        self.workflow.info = Mock()
        for cancel in (True, False):
            self.ui.cancel_title = "Initialize OTA V2 through USB bootloader" if cancel else None
            failure = workflow.BootloaderNotReady("image service not ready")
            with self.subTest(cancel=cancel), patch.object(workflow, "provision_main", side_effect=failure) as provision, \
                 self.assertRaises(Cancelled if cancel else workflow.BootloaderNotReady):
                self.workflow.initialize("selected")
            self.assertEqual(provision.call_count, 1 if cancel else 2)
        self.workflow.info.assert_not_called()
        self.ui.show_status.assert_not_called()

    def test_initialization_does_not_offer_reset_for_other_refusals_or_upload_errors(self):
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        self.workflow.info = Mock()
        for failure in (CampaignError("different application already running"), TransportError("post-upload disconnect")):
            self.ui.continue_step.reset_mock()
            with self.subTest(failure=failure), patch.object(workflow, "provision_main", side_effect=failure) as provision, \
                 self.assertRaises(type(failure)):
                self.workflow.initialize("selected")
            provision.assert_called_once()
            self.ui.continue_step.assert_called_once()
        self.workflow.info.assert_not_called()

    def test_initialization_dialog_reports_real_failure_instead_of_generic_task_message(self):
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        self.workflow.info = Mock()
        error = OSError("COM11: access denied; close the serial monitor")
        args = ["initialize", "--project", str(self.root), "--environment", "OTA", "--mcumgr", str(self.fixture.mcumgr)]
        with patch.object(workflow, "Dialogs", return_value=self.ui), \
             patch.object(workflow, "Workflow", return_value=self.workflow), \
             patch("provision_ota.USBConnection", side_effect=error) as connection, \
             redirect_stderr(io.StringIO()) as output:
            self.assertEqual(workflow.main(args), 1)
        connection.assert_called_once()
        self.assertIn(str(error), output.getvalue())
        self.assertIn(str(error), self.ui.problem.call_args.args[1])
        self.workflow.info.assert_not_called()
        self.ui.show_status.assert_not_called()
        self.ui.close.assert_called_once()
        states = list(self.workflow.operations.glob("*/operation.json"))
        self.assertEqual(len(states), 1)
        self.assertEqual(workflow.transition._json(states[0])["phase"], "INSTALLING")

    def test_can_update_rejects_receiver_and_cancel_before_build_or_campaign(self):
        self.workflow.info = Mock(return_value=self.fixture.info)
        self.workflow.build = Mock()
        with patch.object(workflow, "campaign_main") as campaign, self.assertRaises(CampaignError):
            self.workflow.can_update("selected")
        campaign.assert_not_called()
        self.ui.number.assert_not_called()
        self.workflow.info.return_value = dict(self.fixture.info, image_class="lead")
        self.ui.cancel_title = "Update CAN receiver boards"
        with patch.object(workflow, "campaign_main") as campaign, self.assertRaises(Cancelled):
            self.workflow.can_update("selected")
        self.workflow.build.assert_not_called()
        campaign.assert_not_called()

    def test_completed_can_journal_is_never_appended_by_reconcile(self):
        path, state = self.workflow.create("selected", "can-update")
        self.workflow.save(path, state, "COMPLETE")
        journal = path / "campaign.jsonl"
        raw = b'{"event":"SUCCESS","campaign":17}\n'
        journal.write_bytes(raw)
        self.workflow.info = Mock(return_value=dict(self.fixture.info, image_class="lead"))
        self.ui.answers["Finish previous CAN update"] = str(journal)
        with patch.object(workflow, "campaign_main") as campaign:
            self.workflow.reconcile("selected")
        campaign.assert_not_called()
        self.assertEqual(journal.read_bytes(), raw)
        self.assertEqual(self.ui.notice.call_args.args[0], "Update already completed")

    def test_can_campaign_receives_archived_bytes_count_and_own_journal_after_persisting(self):
        self.workflow.info = Mock(return_value=dict(self.fixture.info, image_class="lead"))
        self.workflow.build = Mock()
        self.workflow.snapshot = Mock(side_effect=lambda environment, destination, **kwargs: workflow.copy_pair(
            self.fixture.source_path, self.fixture.source_manifest_path, destination))
        def campaign(args, *, raise_errors=False):
            self.assertTrue(raise_errors)
            image = Path(args[args.index("--image") + 1])
            journal = Path(args[args.index("--journal") + 1])
            self.assertEqual(image.parent.parent, journal.parent)
            self.assertEqual(args[args.index("--serial") + 1], "selected")
            self.assertEqual(args[args.index("--expected-count") + 1], "2")
            self.assertEqual(self.state(journal.parent)["phase"], "UPDATING")
            self.assertEqual(image.read_bytes(), self.fixture.source_data)
            return 0
        with patch.object(workflow, "campaign_main", side_effect=campaign) as run:
            self.workflow.can_update("selected")
        run.assert_called_once()
        self.workflow.build.assert_called_once_with("OTA")
        self.assertTrue(self.workflow.snapshot.call_args.kwargs["compact"])

    def test_main_cancel_closes_ui_and_does_not_report_failure_dialog(self):
        args = ["status", "--project", str(self.root), "--environment", "OTA", "--mcumgr", str(self.fixture.mcumgr)]
        with patch.object(workflow, "Dialogs", return_value=self.ui), \
             patch.object(workflow.Workflow, "run", side_effect=Cancelled("stop")):
            self.assertEqual(workflow.main(args), 1)
        self.ui.close.assert_called_once()
        self.ui.problem.assert_not_called()

    def test_unavailable_desktop_error_returns_cleanly_even_if_error_dialog_fails(self):
        args = ["status", "--project", str(self.root), "--environment", "OTA", "--mcumgr", str(self.fixture.mcumgr)]
        self.ui.problem.side_effect = RuntimeError("Tk desktop unavailable")
        with patch.object(workflow, "Dialogs", return_value=self.ui), \
             patch.object(workflow.Workflow, "run", side_effect=RuntimeError("Tk desktop unavailable")), \
             redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(workflow.main(args), 1)
        self.assertIn("Tk desktop unavailable", errors.getvalue())
        self.ui.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
