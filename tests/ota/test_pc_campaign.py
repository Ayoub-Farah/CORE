from pathlib import Path
import sys
import tempfile
import io
import json
from contextlib import redirect_stdout
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "owntech" / "tools"))
from lead_update import (Campaign, CampaignError, Journal, USBConnection, journal_campaign,
                         select_port, ReceiverProbeTimeout, TransportError, read_only_status, main)

IDS = ["0102030405060708", "1112131415161718", "2122232425262728"]
MANIFEST = {"artifact_size": 512, "useful_size": 256, "artifact_sha256": "aa" * 32,
            "mcuboot_image_hash": "bb" * 32, "version": "2.0.0+0", "build_id": "B",
            "protocol": 1, "hardware_id": 1, "layout_id": 1, "bootloader_id": 1,
            "profile": {"slot_size": 512}}


class Clock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        return self.now

    def sleep(self, duration):
        self.now += duration


class Transport:
    def __init__(self):
        self.calls = []
        self.discovered = list(IDS)
        self.postboot = list(IDS)
        self.wrong_hash = False
        self.complete = True
        self.paged = False
        self.release_complete = True
        self.barrier_complete = True
        self.committed = False

    def rows(self, values):
        return [{"identity": value, "address": index + 1, "role": "lead" if index == 0 else "follower",
                 "available": True, "compatible": True, "offset": 512, "queue_depth": 0,
                 "state": "VALIDATED", "flash_complete": self.complete, "validated": True,
                 "version": "2.0.0", "build_id": "B", "healthy": True, "confirmed": True,
                 "mcuboot_image_hash": bytes.fromhex(("cc" if self.wrong_hash else "bb") * 32)}
                for index, value in enumerate(values)]

    def request(self, command, payload):
        self.calls.append((command, payload))
        if command == "info":
            return {"service": "owntech-ota", "protocol": 1, "identity": IDS[0], "role": "follower",
                    "available": True, "active_confirmed": True, "slot_available": True,
                    "slot_size": 512, "useful_capacity": 256}
        if command in ("discover", "status", "reconcile"):
            rows = self.rows(self.postboot if command == "reconcile" else self.discovered)
            phase = ("SUCCESS" if self.release_complete else "POSTBOOT_CHECK") if command == "reconcile" else (
                "ALL_VALIDATED" if self.barrier_complete else "VERIFYING")
            if command == "status" and self.committed:
                phase = "RECOVERY_REQUIRED"
            return {"phase": phase, "target_count": len(rows),
                    "targets": rows[payload.get("index", 0):payload.get("index", 0) + 1] if self.paged else rows}
        if command == "stage_begin":
            self.committed = False
            return {"state": "STAGING"}
        if command == "stage_data":
            return {"offset": payload["offset"] + len(payload["data"])}
        if command == "stage_end":
            return {"state": "STAGED"}
        if command == "commit":
            self.committed = True
        return {"rc": 0}


class CommitTransport(Transport):
    """Device transitions after an asynchronously accepted COMMIT."""
    def __init__(self, events, lost_ack=False):
        super().__init__()
        self.events = list(events)
        self.lost_ack = lost_ack

    def request(self, command, payload):
        response = super().request(command, payload)
        if command == "commit" and self.lost_ack:
            raise TransportError("COMMIT response lost")
        if command == "status" and self.committed and self.events:
            event = self.events.pop(0)
            if isinstance(event, Exception):
                raise event
            response.update(event if isinstance(event, dict) else {"phase": event})
        return response


class CampaignTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.journal = Journal(Path(self.temp.name) / "journal.jsonl", 42)
        self.transport = Transport()
        self.clock = Clock()

    def tearDown(self):
        self.journal.close()
        self.temp.cleanup()

    def client(self, expected=IDS, count=None):
        return Campaign(self.transport, MANIFEST, b"x" * 512, self.journal, expected, count,
                        timeout=1, poll_interval=0.4, clock=self.clock, sleep=self.clock.sleep,
                        output=lambda value: None)

    def test_success_always_stages_and_only_commits_after_validation(self):
        self.assertEqual(self.client().run(), "SUCCESS")
        commands = [command for command, _ in self.transport.calls]
        self.assertEqual(commands.count("stage_data"), 2)
        self.assertLess(commands.index("stage_end"), commands.index("start"))
        self.assertLess(commands.index("status"), commands.index("commit"))
        self.assertNotIn("reset", commands)
        self.assertNotIn("abort", commands)

    def test_paged_inventory_and_status(self):
        self.transport.paged = True
        self.assertEqual(self.client().run(), "SUCCESS")

    def test_failed_commit_keeps_validated_state_and_journal_error_without_reconnect(self):
        # Real two-board failure: all bytes validated, then the collective NVS
        # journal write fails before any participant COMMIT or reboot.
        self.transport = CommitTransport([])
        rows = self.transport.rows(IDS[:2])
        for row in rows:
            row.update(state="VALID", error=-9 if row["identity"] == IDS[0] else 0)
        self.transport.events = [{"phase": "FAILED", "targets": rows, "target_count": 2}]
        client = self.client()
        client.reconnect = Mock()
        with self.assertRaisesRegex(CampaignError, r"commit/reboot failed:.*FAILED.*VALID.*-9"):
            client.run()
        self.assertTrue(client.committed)
        client.reconnect.assert_not_called()
        commands = [command for command, _ in self.transport.calls]
        self.assertEqual(commands.count("commit"), 1)
        self.assertNotIn("reconcile", commands)
        self.assertNotIn("abort", commands)
        self.assertNotIn("reset", commands)
        events = [json.loads(line) for line in self.journal.path.read_text().splitlines()]
        self.assertEqual(events[-1]["event"], "STATUS")
        self.assertEqual(events[-1]["status"]["targets"][0]["error"], -9)

    def test_commit_waits_past_two_seconds_and_reconnects_only_after_usb_disappears(self):
        self.transport = CommitTransport(["COMMITTING"] * 6 + ["REBOOTING",
            TransportError("USB disappeared during reboot"), "BOOT", "RECOVERY_REQUIRED"])
        client = self.client()
        client.timeout = 10

        def reconnect():
            self.assertEqual(len(self.transport.events), 2)
            self.assertGreater(self.clock.now, 2)
            return self.transport

        client.reconnect = Mock(side_effect=reconnect)
        self.assertEqual(client.run(), "SUCCESS")
        client.reconnect.assert_called_once()
        self.assertFalse(self.transport.events)
        commands = [command for command, _ in self.transport.calls]
        self.assertEqual(commands.count("commit"), 1)
        self.assertGreater(commands.index("reconcile"), commands.index("commit") + 9)
        self.assertNotIn("abort", commands)
        self.assertNotIn("reset", commands)

    def test_lost_commit_ack_observes_old_state_then_reboot_without_resending_commit(self):
        self.transport = CommitTransport(["ALL_VALIDATED", "COMMITTING", "REBOOTING",
                                          "RECOVERY_REQUIRED"], lost_ack=True)
        client = self.client()
        client.timeout = 5
        client.reconnect = Mock()
        self.assertEqual(client.run(), "SUCCESS")
        self.assertTrue(client.committed)
        client.reconnect.assert_not_called()
        commands = [command for command, _ in self.transport.calls]
        self.assertEqual(commands.count("commit"), 1)
        self.assertNotIn("abort", commands)

    def test_commit_connection_loss_retries_are_bounded_and_never_abort(self):
        self.transport = CommitTransport([TransportError("disconnected")] * 10, lost_ack=True)
        client = self.client()
        client.reconnect = Mock(side_effect=TransportError("same serial still absent"))
        with self.assertRaisesRegex(CampaignError, "PARTIAL: bounded timeout waiting for commit/reboot"):
            client.run()
        self.assertTrue(client.committed)
        self.assertLessEqual(client.reconnect.call_count, 3)
        commands = [command for command, _ in self.transport.calls]
        self.assertNotIn("reconcile", commands)
        self.assertNotIn("abort", commands)
        self.assertNotIn("reset", commands)

    def test_commit_reconnect_rejects_different_lead_without_reconcile(self):
        self.transport = CommitTransport([TransportError("reboot disconnected USB")])
        client = self.client()
        wrong = Mock()
        wrong.request.return_value = {"identity": IDS[1]}
        client.reconnect = Mock(return_value=wrong)
        with self.assertRaisesRegex(CampaignError, "different Lead after reconnect"):
            client.run()
        self.assertTrue(client.committed)
        wrong.request.assert_called_once_with("info", {})
        self.assertNotIn("reconcile", [command for command, _ in self.transport.calls])
        self.assertNotIn("abort", [command for command, _ in self.transport.calls])

    def test_standalone_initialization_does_not_authorize_fleet_update(self):
        request = self.transport.request

        def standalone(command, payload):
            reply = request(command, payload)
            if command == "info":
                reply.update(phase="WAITING_CAN", available=False, local_healthy=True,
                             healthy=False, can_ready=False, error=0)
            return reply

        self.transport.request = standalone
        with self.assertRaisesRegex(CampaignError, "Lead waiting for CAN"):
            self.client().run()
        self.assertEqual([command for command, _ in self.transport.calls], ["info"])

    def test_missing_identity_and_duplicate_address_block_before_erase(self):
        self.transport.discovered.pop()
        with self.assertRaises(CampaignError):
            self.client().run()
        self.assertNotIn("stage_begin", [command for command, _ in self.transport.calls])

    def test_missing_inventory_requirement(self):
        with self.assertRaises(CampaignError):
            self.client(expected=None).run()
        with self.assertRaises(CampaignError):
            self.client(expected=None, count=4).run()

    def test_buffered_flash_is_not_commit_ready(self):
        self.transport.complete = False
        with self.assertRaises(CampaignError):
            self.client().run()
        commands = [command for command, _ in self.transport.calls]
        self.assertNotIn("commit", commands)
        self.assertIn("abort", commands)

    def test_complete_rows_still_need_coordinator_barrier(self):
        self.transport.barrier_complete = False
        with self.assertRaises(CampaignError):
            self.client().run()
        self.assertNotIn("commit", [command for command, _ in self.transport.calls])

    def test_healthy_rows_still_need_persistent_maintenance_release(self):
        self.transport.release_complete = False
        with self.assertRaisesRegex(CampaignError, "PARTIAL"):
            self.client().run()

    def test_journal_recovers_exact_frozen_inventory(self):
        self.client().run()
        inventory, lead, serial = journal_campaign(self.journal.path)
        self.assertEqual(inventory["targets"], IDS)
        self.assertEqual(inventory["manifest"], MANIFEST)
        self.assertEqual(lead, IDS[0])

    def test_wrong_lead_recovery_probe_cannot_rebind_frozen_journal(self):
        self.journal.emit("USB_SELECTED", usb_serial="original-usb")
        self.client().run()
        # A mistaken recovery attempt is logged before its identity check fails.
        self.journal.emit("USB_SELECTED", usb_serial="wrong-usb")
        self.journal.emit("PROBE_LEAD", IDS[1], info={"identity": IDS[1]})
        inventory, lead, serial = journal_campaign(self.journal.path)
        self.assertEqual(lead, IDS[0])
        self.assertEqual(serial, "original-usb")
        self.assertEqual(inventory["targets"], IDS)

    def test_missing_and_wrong_active_image_are_partial(self):
        for missing in (True, False):
            self.transport.postboot = IDS[:2] if missing else IDS
            self.transport.wrong_hash = not missing
            with self.subTest(missing=missing), self.assertRaisesRegex(CampaignError, "PARTIAL"):
                self.client().run()
        self.assertNotIn("abort", [command for command, _ in self.transport.calls])

    def test_usb_selection_never_falls_back(self):
        ports = [SimpleNamespace(vid=0x2FE3, serial_number="one", device="COM1"),
                 SimpleNamespace(vid=0x2FE3, serial_number="two", device="COM2")]
        self.assertEqual(select_port(ports, "two").device, "COM2")
        for serial in (None, "absent"):
            with self.subTest(serial=serial), self.assertRaises(CampaignError):
                select_port(ports, serial)

    def test_read_only_status_cli_needs_no_inventory_or_artifact(self):
        self.transport.paged = True
        connection = SimpleNamespace(connect=lambda: self.transport, transport=None)
        with patch("lead_update.USBConnection", return_value=connection), redirect_stdout(io.StringIO()) as output:
            self.assertEqual(main(["--status", "--serial", "selected"]), 0)
        self.assertIn(IDS[2], output.getvalue())
        self.assertEqual({command for command, _ in self.transport.calls}, {"info", "status"})

    def test_device_events_survive_missed_polls_reset_timestamps_and_journal_reopen(self):
        client = self.client()
        row = {"campaign": 42, "event_mask": (1 << 0) | (1 << 1) | (1 << 6) | (1 << 11),
               "event_ms": [1000, 1000, 0, 0, 0, 0, 1001, 0, 0, 0, 0, 5],
               "event_order": [1, 2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4]}
        client._device_events(IDS[0], row)
        client._device_events(IDS[0], row)
        events = [json.loads(line) for line in self.journal.path.read_text().splitlines()]
        self.assertEqual([event["event"] for event in events],
                         ["ERASE_BEGIN", "ERASE_END", "FLASH_COMPLETE", "POSTBOOT_CHECK"])
        self.assertEqual([event["device_uptime_ms"] for event in events], [1000, 1000, 1001, 5])
        self.journal.close()
        self.journal = Journal(self.journal.path, 42)
        client = self.client()
        client._device_events(IDS[0], row)
        self.assertEqual(len(self.journal.path.read_text().splitlines()), 4)
        row["campaign"] = 43
        client._device_events(IDS[0], row)
        self.assertEqual(len(self.journal.path.read_text().splitlines()), 8)

    def test_zero_campaign_event_history_is_journaled_before_campaign_aborts(self):
        # Observed just after START: the staged Lead's seven valid USB events
        # survived, but participant status exposed campaign=0 before adoption.
        original_request = self.transport.request
        invalid_response = {}

        def request(command, payload):
            result = original_request(command, payload)
            if command == "status" and any(name == "start" for name, _ in self.transport.calls):
                row = result["targets"][0]
                row.update(campaign=0, image_size=0, state="IDLE", validated=False,
                           event_mask=463, event_ms=[204512, 206995, 206995, 213254, 0, 0,
                                                   213254, 213254, 213371, 0, 0, 0],
                           event_order=[1, 2, 3, 4, 0, 0, 5, 6, 7, 0, 0, 0])
                result.update(campaign=42, phase="PREPARING")
                invalid_response.update(result)
            return result

        self.transport.request = request
        client = self.client()
        with self.assertRaisesRegex(CampaignError, "invalid device event history"):
            client.run()
        records = [json.loads(line) for line in self.journal.path.read_text().splitlines()]
        rejected = [record for record in records if record["event"] == "STATUS_REJECTED"]
        self.assertEqual(len(rejected), 1)
        record = rejected[0]
        self.assertEqual(record["command"], "status")
        self.assertEqual(record["identity"], IDS[0])
        self.assertEqual(record["reason"], "invalid device event history")
        # Journal encodes the public hash bytes as hex, as for normal STATUS.
        expected = json.loads(json.dumps(invalid_response, default=lambda value: value.hex()))
        self.assertEqual(record["response"], expected)
        self.assertEqual(record["rejected_row"], expected["targets"][0])
        self.assertEqual(record["page_responses"], [])
        self.assertLess(records.index(record), next(index for index, item in enumerate(records)
                                                  if item["event"] == "FAILED"))
        self.assertFalse(any("device_event" in item for item in records))
        self.assertNotIn("event_mask", client.rows[IDS[0]])
        self.assertFalse(client.committed)
        commands = [name for name, _ in self.transport.calls]
        self.assertEqual(commands[-2:], ["status", "abort"])
        self.assertNotIn("commit", commands)
        self.assertNotIn("reconcile", commands)

    def test_multiple_cdc_selects_unique_receiver_and_never_bootstraps_busy_port(self):
        connection = USBConnection.__new__(USBConnection)
        ports = [SimpleNamespace(vid=0x2FE3, serial_number="one", device="COM1"),
                 SimpleNamespace(vid=0x2FE3, serial_number="one", device="COM2")]
        connection.enumerate = lambda: ports
        connection.serial_number = "one"
        closed = []

        class Interface:
            def __init__(self, port):
                self.port = port

            def close(self):
                closed.append(self.port)

            def request(self, command):
                if self.port == "COM1":
                    raise ReceiverProbeTimeout("console")
                return {"service": "owntech-ota", "protocol": 1}

        with patch("lead_update.SerialSMP", Interface):
            connection.connect()
        self.assertEqual(connection.device, "COM2")
        self.assertIn("COM1", closed)
        with patch("lead_update.SerialSMP", side_effect=TransportError("occupied")):
            with self.assertRaises(TransportError) as error:
                connection.connect()
            self.assertNotIsInstance(error.exception, ReceiverProbeTimeout)


if __name__ == "__main__":
    unittest.main()
