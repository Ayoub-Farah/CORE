from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "owntech" / "tools"))
from lead_update import (Campaign, CampaignError, Journal, USBConnection, journal_campaign,
                         select_port, ReceiverProbeTimeout, TransportError)

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
            return {"phase": phase, "target_count": len(rows),
                    "targets": rows[payload.get("index", 0):payload.get("index", 0) + 1] if self.paged else rows}
        if command == "stage_begin":
            return {"state": "STAGING"}
        if command == "stage_data":
            return {"offset": payload["offset"] + len(payload["data"])}
        if command == "stage_end":
            return {"state": "STAGED"}
        return {"rc": 0}


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
