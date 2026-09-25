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

    def harness(self):
        harness = test_pc_recover.RecoveryUploadTests()
        harness.setUp()
        self.addCleanup(harness.tearDown)
        events = compact_records()
        self.write(harness.journal, events)
        config = generate(harness.journal, harness.config.parent, compact_receiver_only=True)
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


if __name__ == "__main__":
    unittest.main()
