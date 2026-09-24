"""Per-board initialization never uses the fleet path or resets a live receiver."""
from contextlib import redirect_stderr, redirect_stdout
from itertools import count
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, Mock, patch

from test_pc_artifact import artifact
from test_pc_smp import Serial, response

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
from lead_update import CampaignError, USBConnection
from bootloader_upload import UploadError
from ota_artifact import inspect_image
from provision_ota import provision, main
from smp_transport import CommandError, ProtocolError, ReceiverProbeTimeout, TransportError


class Receiver:
    def __init__(self, info):
        self.info = info
        self.calls = []

    def request(self, command, payload=None):
        self.calls.append((command, payload))
        if command == "info":
            return dict(self.info)
        if command == "set_role":
            self.info["role"] = payload["role"]
            return {"rc": 0}
        raise AssertionError("provisioning must not call " + command)

    def close(self):
        pass


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.manifest = inspect_image(artifact(), build_id="ota-current")
        self.info = {"service": "owntech-ota", "protocol": 1, "identity": "0000000000000001",
                     "phase": "IDLE", "role": "follower", "available": True,
                     "active_confirmed": True, "slot_available": True,
                     "slot_size": self.manifest["profile"]["slot_size"], "useful_capacity": 221184,
                     **{key: self.manifest[key] for key in ("version", "build_id", "hardware_id",
                        "layout_id", "bootloader_id", "mcuboot_image_hash")}}
        self.receiver = Receiver(self.info)
        self.connection = SimpleNamespace(serial_number="physical-one", transport=self.receiver,
                                          connect=Mock(return_value=self.receiver),
                                          bootstrap=Mock(return_value=self.receiver))

    def run_provision(self, **kwargs):
        return provision(self.connection, Path("exact.mcuboot.bin"), self.manifest, Path("mcumgr"),
                         output=lambda _: None, **kwargs)

    def use_wire_connection(self, lines_by_port):
        """Exercise the real probe stack with deterministic serial reads/time."""
        now = [0.0]

        class WireSerial(Serial):
            def readline(self, size):
                now[0] += 0.2
                return super().readline(size)

        streams = {}
        for device, lines in lines_by_port.items():
            streams[device] = WireSerial()
            streams[device].lines = list(lines)
            streams[device].close = Mock()
        ports = [SimpleNamespace(vid=0x2FE3, serial_number="physical-one", device=device)
                 for device in streams]
        with patch.dict(sys.modules, {"serial.tools.list_ports": SimpleNamespace(comports=lambda: ports)}):
            self.connection = USBConnection()
        self.connection.bootstrap = Mock(return_value=self.receiver)
        factory = Mock(side_effect=lambda port, **kwargs: streams[port])
        patches = [patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=factory)}),
                   patch("smp_transport.time.monotonic", side_effect=lambda: now[0])]
        for replacement in patches:
            replacement.start()
            self.addCleanup(replacement.stop)
        return streams, factory

    def test_same_live_follower_is_read_only_and_reports_verified_identity(self):
        self.info["mcuboot_image_hash"] = bytes.fromhex(self.manifest["mcuboot_image_hash"])
        result = self.run_provision()
        self.assertEqual(result["result"], "ALREADY_INITIALIZED")
        self.assertEqual(result["info"]["identity"], "0000000000000001")
        self.assertEqual(self.receiver.calls, [("info", {})])
        self.connection.bootstrap.assert_not_called()

    def test_same_healthy_idle_lead_can_be_initialized_as_follower_only(self):
        self.info["role"] = "lead"
        self.run_provision()
        self.assertEqual(self.receiver.calls, [("info", {}), ("set_role", {"role": "follower"}), ("info", {})])
        self.connection.bootstrap.assert_not_called()

    def test_live_other_image_cannot_be_overwritten_even_if_same_version(self):
        for field, value in (("build_id", "ota-old"), ("version", "0.9.0+0"), ("mcuboot_image_hash", "00" * 32)):
            with self.subTest(field=field):
                original = self.info[field]
                self.info[field] = value
                with self.assertRaisesRegex(CampaignError, "USB_LEAD lead_update"):
                    self.run_provision()
                self.info[field] = original
        self.connection.bootstrap.assert_not_called()
        self.assertTrue(all(call[0] == "info" for call in self.receiver.calls))

    def test_absence_authorizes_exactly_one_application_bootstrap(self):
        self.connection.connect.side_effect = ReceiverProbeTimeout("no receiver")
        result = self.run_provision()
        self.connection.bootstrap.assert_called_once_with(Path("exact.mcuboot.bin"), Path("mcumgr"))
        self.assertEqual(result["result"], "PROVISIONED")
        self.assertEqual(self.receiver.calls, [("info", {})])

    def test_real_silent_probe_bootstraps_once_and_verifies_follower(self):
        self.info["role"] = "lead"
        streams, factory = self.use_wire_connection({"COM1": []})
        result = self.run_provision()
        self.connection.bootstrap.assert_called_once_with(Path("exact.mcuboot.bin"), Path("mcumgr"))
        self.assertEqual(result["result"], "PROVISIONED")
        self.assertEqual(result["info"]["role"], "follower")
        self.assertEqual(self.receiver.calls, [("info", {}), ("set_role", {"role": "follower"}), ("info", {})])
        streams["COM1"].close.assert_called_once()
        self.assertTrue(streams["COM1"].written)
        self.assertEqual(factory.call_args.kwargs["baudrate"], 115200)

    def test_real_unsupported_ota_probe_delegates_bootstrap_without_1200_touch(self):
        streams, _ = self.use_wire_connection({"COM1": response({"rc": 8})})
        result = self.run_provision()
        self.connection.bootstrap.assert_called_once_with(
            Path("exact.mcuboot.bin"), Path("mcumgr"), enter_bootloader=False)
        self.assertEqual(result["result"], "PROVISIONED")
        streams["COM1"].close.assert_called_once()
        self.assertEqual(self.receiver.calls, [("info", {})])

    def test_real_console_probe_reaches_second_cdc_without_bootstrap(self):
        streams, factory = self.use_wire_connection({
            "COM1": [b"application console\n"],
            "COM2": response(self.info) + response(self.info, sequence=1),
        })
        result = self.run_provision()
        self.assertEqual(result["result"], "ALREADY_INITIALIZED")
        self.assertEqual(self.connection.device, "COM2")
        self.connection.bootstrap.assert_not_called()
        streams["COM1"].close.assert_called_once()
        streams["COM2"].close.assert_not_called()
        self.assertEqual([call.args[0] for call in factory.call_args_list], ["COM1", "COM2"])
        self.assertEqual(len(streams["COM2"].written), 2)
        self.assertEqual(self.receiver.calls, [])

    def test_real_console_write_timeout_or_open_failure_does_not_hide_smp_cdc(self):
        for failure in ("write", "open"):
            for console_first in (True, False):
                with self.subTest(failure=failure, console_first=console_first):
                    lines = {"COM12": [], "COM31": response(self.info) + response(self.info, sequence=1)}
                    if not console_first:
                        lines = dict(reversed(list(lines.items())))
                    streams, factory = self.use_wire_connection(lines)
                    if failure == "write":
                        streams["COM12"].write = Mock(side_effect=OSError("Write timeout"))
                    else:
                        def open_port(port, **kwargs):
                            if port == "COM12":
                                raise OSError("Access denied")
                            return streams[port]
                        factory.side_effect = open_port
                    result = self.run_provision()
                    self.assertEqual(result["result"], "ALREADY_INITIALIZED")
                    self.assertEqual(self.connection.device, "COM31")
                    self.connection.bootstrap.assert_not_called()
                    streams["COM31"].close.assert_not_called()
                    self.assertEqual([call.args[0] for call in factory.call_args_list], list(lines))
                    if failure == "write":
                        streams["COM12"].close.assert_called_once()

    def test_real_cdc_transport_errors_dominate_silence_and_never_bootstrap(self):
        for second_port in ("silent", "open_failure", "write_timeout"):
            with self.subTest(second_port=second_port):
                streams, factory = self.use_wire_connection({"COM12": [], "COM31": []})
                streams["COM12"].write = Mock(side_effect=OSError("Write timeout"))
                if second_port == "write_timeout":
                    streams["COM31"].write = Mock(side_effect=OSError("Write timeout"))
                elif second_port == "open_failure":
                    def open_port(port, **kwargs):
                        if port == "COM31":
                            raise OSError("Access denied")
                        return streams[port]
                    factory.side_effect = open_port
                with self.assertRaisesRegex(TransportError, "receiver absence has not been proven") as failure, \
                     patch("lead_update.upload_image") as upload, patch("lead_update.subprocess.run") as reset:
                    self.run_provision()
                self.assertNotIsInstance(failure.exception, ReceiverProbeTimeout)
                self.assertIn("COM12", str(failure.exception))
                self.assertEqual([call.args[0] for call in factory.call_args_list], ["COM12", "COM31"])
                self.connection.bootstrap.assert_not_called()
                upload.assert_not_called()
                reset.assert_not_called()
                streams["COM12"].close.assert_called_once()
                if second_port != "open_failure":
                    streams["COM31"].close.assert_called_once()

    def test_real_incompatible_or_malformed_cdc_blocks_even_after_a_valid_receiver(self):
        for invalid_lines in (response({"service": "other", "protocol": 1}), [b"\x06\x09!invalid!\n"]):
            with self.subTest(invalid_lines=invalid_lines):
                streams, _ = self.use_wire_connection({"COM31": response(self.info), "COM12": invalid_lines})
                with self.assertRaises(CampaignError):
                    self.run_provision()
                self.connection.bootstrap.assert_not_called()
                streams["COM31"].close.assert_called_once()
                streams["COM12"].close.assert_called_once()

    def test_real_multiple_valid_smp_cdc_interfaces_remain_ambiguous(self):
        streams, _ = self.use_wire_connection({"COM31": response(self.info), "COM12": response(self.info)})
        with self.assertRaisesRegex(CampaignError, "multiple compatible SMP interfaces"):
            self.run_provision()
        self.connection.bootstrap.assert_not_called()
        streams["COM31"].close.assert_called_once()
        streams["COM12"].close.assert_called_once()

    def test_real_partial_smp_response_never_bootstraps(self):
        frames = response(self.info)
        self.assertGreater(len(frames), 1)
        streams, _ = self.use_wire_connection({"COM1": frames[:1]})
        with self.assertRaises((TransportError, CampaignError)) as failure:
            self.run_provision()
        self.assertNotIsInstance(failure.exception, ReceiverProbeTimeout)
        self.connection.bootstrap.assert_not_called()
        streams["COM1"].close.assert_called_once()
        self.assertEqual(self.receiver.calls, [])

    def test_occupied_malformed_and_non_unsupported_errors_never_bootstrap(self):
        for error in (TransportError("busy"), ProtocolError("bad CRC"), CommandError("unsupported group"),
                      CommandError("busy", response={"rc": 5}),
                      CampaignError("multiple matching receivers")):
            with self.subTest(error=error):
                self.connection.connect.side_effect = error
                with self.assertRaises((CampaignError, TransportError)):
                    self.run_provision()
                self.connection.bootstrap.assert_not_called()
        self.assertEqual(self.receiver.calls, [])

    def test_busy_recovery_failed_and_unconfirmed_receiver_never_changes_role(self):
        self.info["role"] = "lead"
        for phase in ("STAGED", "RECOVERY_REQUIRED", "FAILED", "DISCOVERING"):
            self.info["phase"] = phase
            with self.subTest(phase=phase), self.assertRaisesRegex(CampaignError, "not idle"):
                self.run_provision()
        self.info["phase"] = "IDLE"
        self.info["active_confirmed"] = False
        with self.assertRaisesRegex(CampaignError, "before any reset"):
            self.run_provision(clock=Mock(side_effect=[0, 31]))
        self.connection.bootstrap.assert_not_called()
        self.assertTrue(all(call[0] == "info" for call in self.receiver.calls))

    def test_new_boot_snapshot_waits_read_only_then_checks_identity_and_health(self):
        replies = [{"service": "owntech-ota", "protocol": 1, "phase": "BOOT"}, self.info]
        self.receiver.request = Mock(side_effect=replies)
        sleep = Mock()
        self.run_provision(sleep=sleep, clock=Mock(side_effect=[0, 1]))
        sleep.assert_called_once_with(0.25)
        self.assertEqual(self.receiver.request.call_count, 2)
        self.connection.bootstrap.assert_not_called()

    def test_unique_physical_serial_autoselect_and_explicit_selection_never_falls_back(self):
        one = SimpleNamespace(vid=0x2FE3, serial_number="one", device="COM1")
        console = SimpleNamespace(vid=0x2FE3, serial_number="one", device="COM2")
        two = SimpleNamespace(vid=0x2FE3, serial_number="two", device="COM3")
        with patch.dict(sys.modules, {"serial.tools.list_ports": SimpleNamespace(comports=lambda: [one, console])}):
            self.assertEqual(USBConnection().serial_number, "one")
            with self.assertRaises(CampaignError):
                USBConnection("missing")
        with patch.dict(sys.modules, {"serial.tools.list_ports": SimpleNamespace(comports=lambda: [one, two])}):
            with self.assertRaises(CampaignError):
                USBConnection()
            selected = USBConnection("two")
            self.assertEqual(selected.serial_number, "two")
            self.assertEqual(selected.bootstrap_port, "COM3")

    def test_existing_bootloader_chain_only_uploads_application_and_tracks_same_serial(self):
        connection = USBConnection.__new__(USBConnection)
        connection.serial_number = "one"
        connection.bootstrap_port = "COM1"
        connection.transport = None
        connection.timeout = 1
        connection.enumerate = lambda: [SimpleNamespace(vid=0x2FE3, serial_number="one", device="COM1")]
        connection._wait_image_service = Mock(return_value="COM7")
        connection.reconnect = Mock(return_value=self.receiver)
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "existing-mcumgr.exe"
            executable.touch()
            serial = MagicMock()
            with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=serial)}), patch("lead_update.time.sleep"), \
                 patch("lead_update.subprocess.run") as run, patch("lead_update.upload_image") as upload:
                connection.bootstrap(Path("exact.mcuboot.bin"), executable)
            serial.assert_called_once_with("COM1", baudrate=1200, timeout=0.2)
            serial.return_value.__enter__.return_value.setDTR.assert_called_once_with(False)
            connection._wait_image_service.assert_called_once_with()
            base = [str(executable), "--conntype", "serial", "--connstring", "dev=COM7,baud=115200,mtu=128",
                    "--timeout", "10", "--tries", "1"]
            upload.assert_called_once_with(base, Path("exact.mcuboot.bin"))
            run.assert_called_once_with(base + ["reset"], check=True, timeout=15)
            connection.reconnect.assert_called_once()

    def test_image_service_wait_follows_same_serial_to_new_com_and_reads_standard_image_state(self):
        state = {"images": [{"slot": 0, "version": "1.0.0", "hash": b"h" * 32}]}
        streams, factory = self.use_wire_connection({
            "COM7": response({"rc": 8}) + response(state, sequence=1, group=1, operation=1),
            "COM8": response(self.info),
        })
        self.connection.bootstrap_port = "COM1"
        self.connection.enumerate = Mock(side_effect=[[], [
            SimpleNamespace(vid=0x2FE3, serial_number="physical-one", device="COM7"),
            SimpleNamespace(vid=0x2FE3, serial_number="other", device="COM8"),
        ]])
        with patch("lead_update.time.sleep") as sleep:
            self.assertEqual(self.connection._wait_image_service(), "COM7")
        self.assertEqual(self.connection.enumerate.call_count, 2)
        sleep.assert_called_once_with(0.25)
        self.assertEqual([call.args[0] for call in factory.call_args_list], ["COM7"])
        self.assertEqual(len(streams["COM7"].written), 2)
        streams["COM7"].close.assert_called_once()
        self.assertEqual(streams["COM8"].written, [])

    def test_image_service_wait_rejects_current_ota_and_invalid_or_unsupported_image_response(self):
        cases = (
            (response(self.info), CampaignError),
            (response({"rc": 8}) + response({"rc": 0}, sequence=1, group=1, operation=1), ProtocolError),
            (response({"rc": 8}) + response({"rc": 8}, sequence=1, group=1, operation=1), CommandError),
        )
        for lines, error_type in cases:
            with self.subTest(error_type=error_type):
                streams, _ = self.use_wire_connection({"COM1": lines})
                with self.assertRaises(error_type), patch("lead_update.upload_image") as upload, \
                     patch("lead_update.subprocess.run") as reset:
                    self.connection._wait_image_service()
                upload.assert_not_called()
                reset.assert_not_called()
                streams["COM1"].close.assert_called_once()

    def test_silent_busy_or_unresolved_image_service_times_out_without_upload_or_reset(self):
        for failure in (ReceiverProbeTimeout("silent port"), TransportError("busy port"),
                        TransportError("incomplete image response")):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                executable = Path(directory) / "mcumgr.exe"
                executable.touch()
                connection = USBConnection.__new__(USBConnection)
                connection.serial_number = "one"
                connection.timeout = 2
                connection.transport = None
                connection.enumerate = lambda: [SimpleNamespace(vid=0x2FE3, serial_number="one", device="COM7")]
                candidate = Mock()
                if str(failure) == "incomplete image response":
                    candidate.request.side_effect = CommandError("unsupported", response={"rc": 8})
                    candidate.image_state.side_effect = failure
                    factory = Mock(return_value=candidate)
                elif isinstance(failure, ReceiverProbeTimeout):
                    candidate.request.side_effect = failure
                    factory = Mock(return_value=candidate)
                else:
                    factory = Mock(side_effect=failure)
                with patch("lead_update.SerialSMP", factory), patch("lead_update.time.monotonic", side_effect=count(step=0.1)), \
                     patch("lead_update.time.sleep"), patch("lead_update.upload_image") as upload, \
                     patch("lead_update.subprocess.run") as reset, \
                     self.assertRaisesRegex(CampaignError, "image service did not become ready"):
                    connection.bootstrap(Path("exact.mcuboot.bin"), executable, enter_bootloader=False)
                self.assertTrue(factory.called)
                upload.assert_not_called()
                reset.assert_not_called()

    def test_bootloader_already_present_uploads_without_1200_touch_and_failure_never_resets(self):
        for failure in (None, UploadError("upload stopped at zero bytes")):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                executable = Path(directory) / "mcumgr.exe"
                executable.touch()
                connection = USBConnection.__new__(USBConnection)
                connection.serial_number = "one"
                connection.bootstrap_port = None
                connection.transport = None
                connection._wait_image_service = Mock(return_value="COM7")
                connection.reconnect = Mock(return_value=self.receiver)
                serial = Mock()
                with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=serial)}), \
                     patch("lead_update.time.sleep"), patch("lead_update.upload_image", side_effect=failure) as upload, \
                     patch("lead_update.subprocess.run") as reset:
                    if failure:
                        with self.assertRaises(UploadError):
                            connection.bootstrap(Path("exact.mcuboot.bin"), executable, enter_bootloader=False)
                    else:
                        self.assertIs(connection.bootstrap(Path("exact.mcuboot.bin"), executable,
                                                           enter_bootloader=False), self.receiver)
                serial.assert_not_called()
                connection._wait_image_service.assert_called_once_with()
                self.assertEqual(upload.call_count, 1)
                if failure:
                    reset.assert_not_called()
                    connection.reconnect.assert_not_called()
                else:
                    self.assertEqual(reset.call_count, 1)
                    connection.reconnect.assert_called_once_with()

    def test_bootloader_entry_rechecks_serial_before_touching_reused_com_port(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "mcumgr.exe"
            executable.touch()
            connection = USBConnection.__new__(USBConnection)
            connection.serial_number = "one"
            connection.bootstrap_port = "COM1"
            connection.transport = None
            connection.enumerate = lambda: [SimpleNamespace(vid=0x2FE3, serial_number="other", device="COM1")]
            connection._wait_image_service = Mock()
            serial = Mock()
            with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=serial)}), \
                 patch("lead_update.upload_image") as upload, patch("lead_update.subprocess.run") as reset, \
                 self.assertRaises(CampaignError):
                connection.bootstrap(Path("exact.mcuboot.bin"), executable)
            serial.assert_not_called()
            connection._wait_image_service.assert_not_called()
            upload.assert_not_called()
            reset.assert_not_called()

    def test_cli_validates_image_before_usb_and_closes_live_transport(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "firmware.mcuboot.bin"
            image.write_bytes(artifact())
            self.receiver.close = Mock()
            with patch("provision_ota.USBConnection", return_value=self.connection) as connect, \
                 redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(main(["--image", str(image), "--build-id", "ota-current", "--mcumgr", "unused"]), 0)
                connect.assert_called_once_with(None, None, timeout=30)
                self.receiver.close.assert_called_once()
                self.connection.bootstrap.assert_not_called()
                connect.reset_mock()
                image.write_bytes(b"not an image")
                self.assertEqual(main(["--image", str(image), "--mcumgr", "unused"]), 1)
                connect.assert_not_called()


if __name__ == "__main__":
    unittest.main()
