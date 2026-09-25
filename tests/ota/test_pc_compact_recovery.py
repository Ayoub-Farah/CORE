"""The v2 USB repair is explicitly scoped to receivers before activation."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from test_pc_artifact import artifact
from test_pc_recovery_config import records, IDS
import test_pc_recover
from test_pc_recover import slot
from ota_artifact import inspect_image
from prepare_ota_recovery import generate, recovery_config, render_header


def compact_records():
    legacy = records()
    manifest = inspect_image(artifact(compact=True), build_id="receiver-test")
    inventory = copy.deepcopy(legacy[2])
    inventory["manifest"] = manifest
    inventory["targets"] = [IDS[1]]
    inventory["inventory"] = [dict(inventory["inventory"][1], mcuboot_image_hash="cc" * 32)]
    return [legacy[0], legacy[1], inventory,
            dict(legacy[3], event="PC_SOURCE_OPEN", manifest=manifest),
            dict(legacy[4], targets=[IDS[1]]), legacy[-1]]


def preprepare_records():
    events = compact_records()
    original = events[2]["inventory"][0]
    original.update(image_class="receiver", state="IDLE", campaign=0, event_mask=0, offset=0,
                    validated=False, flash_complete=False, **{"pass": 0})
    failed = dict(original, state="FAILED", campaign=events[0]["campaign"], available=False,
                  error=-7, event_mask=1, event_order=[1] + [0] * 11,
                  image_size=events[2]["manifest"]["artifact_size"], queue_depth=0)
    events.insert(-1, dict(campaign=events[0]["campaign"], event="STATUS", status={
        "phase": "FAILED", "campaign": events[0]["campaign"], "target_count": 1, "targets": [failed]}))
    return events


class CompactRecoveryTests(unittest.TestCase):
    def write(self, path, events):
        path.write_text("".join(json.dumps(event) + "\n" for event in events))

    def test_config_requires_explicit_mode_and_binds_artifact_and_maintenance(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "journal.jsonl"
            self.write(path, compact_records())
            with self.assertRaises(ValueError):
                recovery_config(path)
            config = recovery_config(path, compact_receiver_only=True)
            self.assertEqual([row["identity"] for row in config["targets"]], [IDS[1]])
            self.assertTrue(config["guards"]["maintenance_required"])
            self.assertTrue(config["guards"]["fleet_journal_absent"])
            header = render_header(config)
            self.assertIn(b"COMPACT_RECEIVER_ONLY 1", header)
            self.assertIn(b"ARTIFACT_HASH_BYTES", header)
            self.assertIn(b"USEFUL_CAPACITY 221184U", header)

    def test_commit_intent_or_rejected_activation_snapshot_blocks_config(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "journal.jsonl"
            for marker in ({"event": "COMMIT_REQUEST", "targets": [IDS[1]]},
                           {"event": "STATUS", "status": {"phase": "COMMITTING"}},
                           {"event": "STATUS_REJECTED", "response": {
                               "targets": [{"identity": IDS[1], "state": "COMMIT_INTENT"}]}}):
                events = compact_records()
                events.insert(-1, dict(campaign=events[0]["campaign"], **marker))
                self.write(path, events)
                with self.subTest(marker=marker), self.assertRaises(ValueError):
                    recovery_config(path, compact_receiver_only=True)

    def harness(self, *, preprepare=False):
        harness = test_pc_recover.RecoveryUploadTests()
        harness.setUp()
        self.addCleanup(harness.tearDown)
        events = preprepare_records() if preprepare else compact_records()
        self.write(harness.journal, events)
        mode = {"preprepare_receiver_only": True} if preprepare else {"compact_receiver_only": True}
        config = generate(harness.journal, harness.config.parent, **mode)
        harness.manifest_value["build_id"] = "recovery-" + config["header_sha256"][:20]
        harness.manifest.write_text(json.dumps(harness.manifest_value))
        return harness, events[2]["manifest"]["mcuboot_image_hash"]

    def test_absent_or_exact_nonpending_secondary_can_be_repaired(self):
        for has_secondary in (False, True):
            with self.subTest(has_secondary=has_secondary):
                harness, digest = self.harness()
                harness.device.images = [slot(0, "cc" * 32, active=True, confirmed=True)]
                if has_secondary:
                    harness.device.images.append(slot(1, digest))
                result = harness.run_recovery(IDS[1], compact_receiver_only=True, apply=True)
                self.assertEqual(result["result"], "RESET_REQUESTED")
                erases = [call for call in harness.mutations() if call[1] == 1]
                self.assertEqual(len(erases), int(has_secondary))
                harness.upload.assert_called_once()

    def test_pending_wrong_secondary_or_unconfirmed_primary_never_mutates(self):
        for fault in ("pending", "wrong", "unconfirmed"):
            harness, digest = self.harness()
            harness.device.images = [slot(0, "cc" * 32, active=True, confirmed=fault != "unconfirmed"),
                                     slot(1, "dd" * 32 if fault == "wrong" else digest,
                                          pending=fault == "pending")]
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                harness.run_recovery(IDS[1], compact_receiver_only=True, apply=True)
            self.assertEqual(harness.mutations(), [])
            harness.upload.assert_not_called()

    def test_preprepare_mode_requires_positive_proof_and_generates_distinct_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "journal.jsonl"
            self.write(path, compact_records())
            with self.assertRaises(ValueError):
                recovery_config(path, preprepare_receiver_only=True)
            self.write(path, preprepare_records())
            config = recovery_config(path, preprepare_receiver_only=True)
            self.assertTrue(config["preprepare_receiver_only"])
            self.assertTrue(config["compact_receiver_only"])
            self.assertEqual(config["guards"]["local_journal_state_values"], [9, 10])
            self.assertFalse(config["guards"]["maintenance_required"])
            self.assertTrue(config["guards"]["maintenance_absent_or_valid_false"])
            self.assertTrue(config["guards"]["host_initial_secondary_absent_or_explicit_hash"])
            self.assertIn(b"PREPREPARE_RECEIVER_ONLY 1", render_header(config))
            with self.assertRaisesRegex(ValueError, "mutually exclusive"):
                recovery_config(path, compact_receiver_only=True, preprepare_receiver_only=True)
            with self.assertRaisesRegex(ValueError, "explicit boolean"):
                recovery_config(path, preprepare_receiver_only=1)

    def test_preprepare_rejects_ambiguous_failed_status_or_completed_preparation(self):
        faults = {"state": "READY", "campaign": 9, "image_size": 123, "role": "lead", "image_class": "lead",
                  "mcuboot_image_hash": "dd" * 32, "healthy": False, "confirmed": False, "available": True,
                  "compatible": False, "error": -1, "event_mask": 3, "event_order": [1, 2] + [0] * 10,
                  "offset": 1, "pass": 1, "queue_depth": 1, "validated": True, "flash_complete": True}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "journal.jsonl"
            for key, value in faults.items():
                events = preprepare_records()
                events[-2]["status"]["targets"][0][key] = value
                self.write(path, events)
                with self.subTest(key=key), self.assertRaises(ValueError):
                    recovery_config(path, preprepare_receiver_only=True)
            events = preprepare_records()
            events[-2]["event"] = "STATUS_REJECTED"
            self.write(path, events)
            with self.assertRaisesRegex(ValueError, "positive failed STATUS"):
                recovery_config(path, preprepare_receiver_only=True)

    def test_preprepare_abort_cleanup_keeps_positive_failure_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "journal.jsonl"
            events = preprepare_records()
            aborted = copy.deepcopy(events[-2]["status"]["targets"][0])
            aborted["state"] = "ABORTED"
            cleanup = copy.deepcopy(events[-2])
            cleanup["status"]["targets"] = [aborted]
            events.insert(-1, cleanup)
            events[-1]["event"] = "ABORTED"
            self.write(path, events)
            config = recovery_config(path, preprepare_receiver_only=True)
            self.assertEqual(config["guards"]["local_journal_state_values"], [9, 10])
            # ABORTED by itself never substitutes for the positive failure
            # observation before any transfer/validation evidence.
            del events[-3]
            self.write(path, events)
            with self.assertRaisesRegex(ValueError, "positive failed STATUS"):
                recovery_config(path, preprepare_receiver_only=True)

    def test_preprepare_rejects_transfer_evidence_in_events_and_rejected_pages(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "journal.jsonl"
            for marker in ({"event": "ERASE_END"}, {"event": "CAN_TRANSFER_BEGIN"},
                           {"event": "VERIFY_BEGIN"}, {"event": "STATE", "device_event": 1},
                           {"event": "STATUS_REJECTED", "page_responses": [{"response": {
                               "targets": [{"identity": IDS[1], "state": "PASS_OPEN"}]}}]},
                           {"event": "STATUS_REJECTED", "response": {"phase": "FAILED", "state": "COMMITTED"}},
                           {"event": "STATUS_REJECTED", "response": {"source_length": 32}}):
                events = preprepare_records()
                events.insert(-2, dict(campaign=events[0]["campaign"], **marker))
                self.write(path, events)
                with self.subTest(marker=marker), self.assertRaises(ValueError):
                    recovery_config(path, preprepare_receiver_only=True)

    def test_preprepare_inspection_and_apply_require_absent_secondary_and_explicit_mode(self):
        harness, digest = self.harness(preprepare=True)
        harness.device.images = [slot(0, "cc" * 32, active=True, confirmed=True)]
        with self.assertRaises(ValueError):
            harness.run_recovery(IDS[1], compact_receiver_only=True)
        self.assertEqual(harness.device.calls, [])
        self.assertEqual(harness.run_recovery(IDS[1], preprepare_receiver_only=True)["result"], "INSPECTED")
        self.assertEqual(harness.mutations(), [])
        harness.upload.assert_not_called()
        result = harness.run_recovery(IDS[1], preprepare_receiver_only=True, apply=True)
        self.assertEqual(result["result"], "RESET_REQUESTED")
        self.assertFalse(any(call[1] == 1 for call in harness.mutations()))
        harness.upload.assert_called_once()

    def test_preprepare_occupied_secondary_or_after_revert_never_mutates(self):
        for fault in ("exact", "pending", "wrong", "after-revert", "unconfirmed"):
            harness, digest = self.harness(preprepare=True)
            harness.device.images = [slot(0, "cc" * 32, active=True, confirmed=fault != "unconfirmed")]
            if fault in ("exact", "pending", "wrong"):
                harness.device.images.append(slot(1, "dd" * 32 if fault == "wrong" else digest, pending=fault == "pending"))
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                harness.run_recovery(IDS[1], preprepare_receiver_only=True, apply=True, after_revert=fault == "after-revert")
            self.assertEqual(harness.mutations(), [])
            harness.upload.assert_not_called()

    def test_preprepare_explicit_old_secondary_hash_is_logged_and_rechecked_before_erase(self):
        harness, _ = self.harness(preprepare=True)
        expected = "dd" * 32
        harness.device.images = [slot(0, "cc" * 32, active=True, confirmed=True), slot(1, expected)]
        result = harness.run_recovery(IDS[1], preprepare_receiver_only=True, expected_secondary_hash=expected)
        self.assertEqual(result["result"], "INSPECTED")
        self.assertEqual(harness.mutations(), [])
        harness.upload.assert_not_called()
        result = harness.run_recovery(IDS[1], preprepare_receiver_only=True, expected_secondary_hash=expected, apply=True)
        self.assertEqual(result["result"], "RESET_REQUESTED")
        self.assertEqual(len([call for call in harness.mutations() if call[1] == 1]), 1)
        harness.upload.assert_called_once()
        events = [json.loads(line) for line in harness.log.read_text().splitlines()]
        self.assertEqual(events[0]["expected_secondary_hash"], expected)
        names = [event["event"] for event in events]
        self.assertLess(names.index("RECOVERY_BEFORE_ERASE"), names.index("RECOVERY_ERASE_SECONDARY_REQUEST"))
        before = next(event for event in events if event["event"] == "RECOVERY_BEFORE_ERASE")
        self.assertEqual(before["image_state"]["images"][1]["hash"], expected)
        after = next(event for event in events if event["event"] == "RECOVERY_AFTER_ERASE")
        self.assertEqual(len(after["image_state"]["images"]), 1)

    def test_preprepare_explicit_secondary_requires_exact_nonpending_unconfirmed_image(self):
        for fault in ("wrong", "absent", "pending", "active", "confirmed", "permanent"):
            harness, _ = self.harness(preprepare=True)
            expected = "dd" * 32
            secondary = slot(1, "ee" * 32 if fault == "wrong" else expected)
            if fault in ("pending", "active", "confirmed", "permanent"):
                secondary[fault] = True
            harness.device.images = [slot(0, "cc" * 32, active=True, confirmed=True)]
            if fault != "absent":
                harness.device.images.append(secondary)
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                harness.run_recovery(IDS[1], preprepare_receiver_only=True, expected_secondary_hash=expected, apply=True)
            self.assertEqual(harness.mutations(), [])
            harness.upload.assert_not_called()

    def test_preprepare_secondary_recheck_blocks_any_changed_slot_before_erase(self):
        for fault in ("primary", "secondary", "pending", "absent"):
            harness, _ = self.harness(preprepare=True)
            expected = "dd" * 32
            harness.device.images = [slot(0, "cc" * 32, active=True, confirmed=True), slot(1, expected)]
            original_read = harness.device.image_state
            reads = 0

            def changed_state():
                nonlocal reads
                reads += 1
                if reads == 2:
                    if fault == "primary":
                        harness.device.images[0]["hash"] = bytes.fromhex("ee" * 32)
                    elif fault == "secondary":
                        harness.device.images[1]["hash"] = bytes.fromhex("ee" * 32)
                    elif fault == "pending":
                        harness.device.images[1]["pending"] = True
                    else:
                        harness.device.images.pop()
                return original_read()

            harness.device.image_state = changed_state
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                harness.run_recovery(IDS[1], preprepare_receiver_only=True, expected_secondary_hash=expected, apply=True)
            self.assertEqual(reads, 2)
            self.assertEqual(harness.mutations(), [])
            harness.upload.assert_not_called()

    def test_explicit_secondary_hash_is_restricted_and_validated_before_usb(self):
        for value in ("", "x" * 64, "dd" * 31, "00" * 32, "cc" * 32, 1):
            harness, _ = self.harness(preprepare=True)
            with self.subTest(value=value), self.assertRaises(ValueError):
                harness.run_recovery(IDS[1], preprepare_receiver_only=True, expected_secondary_hash=value, apply=True)
            self.assertEqual(harness.device.calls, [])
        harness, _ = self.harness()
        with self.assertRaisesRegex(ValueError, "only for preprepare"):
            harness.run_recovery(IDS[1], compact_receiver_only=True, expected_secondary_hash="dd" * 32)
        self.assertEqual(harness.device.calls, [])


if __name__ == "__main__":
    unittest.main()
