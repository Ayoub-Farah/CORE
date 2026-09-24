import copy
import hashlib
import io
import json
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
from prepare_ota_recovery import RecoveryConfigError, generate, main, recovery_config
from ota_artifact import DEFAULT_PROFILE


IDS = ["0102030405060708", "1112131415161718"]
CAMPAIGN = 0x1020304050607080


def records():
    manifest = {"schema_version": 1, "protocol": 1, "format": "mcuboot-padded", "activation_trailer": True,
                "artifact_sha256": "aa" * 32, "mcuboot_image_hash": "bb" * 32,
                "artifact_size": DEFAULT_PROFILE["slot_size"], "useful_size": 4096,
                "version": "1.0.0+0", "build_id": "ota-test", "hardware_id": DEFAULT_PROFILE["hardware_id"],
                "layout_id": DEFAULT_PROFILE["layout_id"], "bootloader_id": DEFAULT_PROFILE["bootloader_id"],
                "profile": copy.deepcopy(DEFAULT_PROFILE)}
    inventory = [{"identity": value, "address": index + 1, "role": "lead" if index == 0 else "follower",
                  "healthy": True, "confirmed": True, "available": True, "compatible": True,
                  "mcuboot_image_hash": ("cc" if index == 0 else "bb") * 32}
                 for index, value in enumerate(IDS)]
    valid = [{"identity": value, "campaign": CAMPAIGN, "state": "VALID", "validated": True,
              "flash_complete": True, "offset": manifest["artifact_size"], "image_size": manifest["artifact_size"],
              "queue_depth": 0, "error": 0, "event_mask": 1023 if index == 0 else 499}
             for index, value in enumerate(IDS)]
    events = [
        {"event": "USB_SELECTED", "usb_serial": "selected-board"},
        {"event": "PROBE_LEAD", "identity": IDS[0]},
        {"event": "DISCOVER", "targets": IDS.copy(), "lead_identity": IDS[0],
         "inventory": inventory, "manifest": manifest},
        {"event": "USB_STAGE_REQUEST", "identity": IDS[0], "manifest": copy.deepcopy(manifest)},
        {"event": "START_REQUEST", "targets": IDS.copy()},
        {"event": "STATUS", "status": {"phase": "ALL_VALIDATED", "campaign": CAMPAIGN,
                                         "target_count": 2, "targets": valid}},
        {"event": "COMMIT_REQUEST", "targets": IDS.copy()},
        {"event": "FAILED", "error": "commit journal error -9; participants still VALID"},
    ]
    return [{"campaign": CAMPAIGN, **event} for event in events]


class RecoveryConfigTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.journal = self.root / "campaign.jsonl"
        self.output = self.root / "config"

    def tearDown(self):
        self.temp.cleanup()

    def write(self, events):
        self.journal.write_text("".join(json.dumps(record) + "\n" for record in events), encoding="utf-8")

    def test_exact_guards_original_hashes_provenance_and_no_device_access(self):
        self.write(records())
        with patch("lead_update.USBConnection", side_effect=AssertionError("no USB allowed")), \
             patch("lead_update.SerialSMP", side_effect=AssertionError("no UART allowed")):
            config = generate(self.journal, self.output)
        header = (self.output / "owntech_ota_recovery_config.h").read_bytes()
        saved = json.loads((self.output / "owntech_ota_recovery_config.json").read_text())
        self.assertEqual(saved, config)
        self.assertEqual(config["journal_sha256"], hashlib.sha256(self.journal.read_bytes()).hexdigest())
        self.assertEqual(config["header_sha256"], hashlib.sha256(header).hexdigest())
        self.assertEqual(config["targets"], [
            {"identity": IDS[0], "original_active_hash": "cc" * 32},
            {"identity": IDS[1], "original_active_hash": "bb" * 32}])
        self.assertEqual(config["guards"]["local_journal_state"], "VALID")
        self.assertEqual(config["guards"]["local_commit_id"], 0)
        self.assertIn(b"OWNTECH_OTA_RECOVERY_CAMPAIGN_ID UINT64_C(0x1020304050607080)", header)
        self.assertIn(b"OWNTECH_OTA_RECOVERY_TARGET_COUNT 2U", header)
        self.assertIn(b"OWNTECH_OTA_RECOVERY_ORIGINAL_HASHES", header)
        self.assertNotIn(b"selected-board", header)

    def test_roster_must_be_exact_at_discovery_validation_and_commit(self):
        for mutate in (
            lambda events: events[2]["targets"].append(IDS[0]),
            lambda events: events[2]["inventory"].pop(),
            lambda events: events[2].update(lead_identity="2222222222222222"),
            lambda events: events[5]["status"]["targets"].pop(),
            lambda events: events[5]["status"].update(target_count=1),
            lambda events: events[6].update(targets=[IDS[0]]),
        ):
            events = records()
            mutate(events)
            self.write(events)
            with self.subTest(events=events), self.assertRaises((RecoveryConfigError, RuntimeError)):
                generate(self.journal, self.output)
            self.assertFalse(self.output.exists())

    def test_unknown_or_unconfirmed_original_images_are_forbidden(self):
        for field, value in (("mcuboot_image_hash", None), ("mcuboot_image_hash", "00" * 32),
                             ("confirmed", False), ("healthy", False), ("address", 254), ("role", "lead")):
            events = records()
            events[2]["inventory"][1][field] = value
            self.write(events)
            with self.subTest(field=field, value=value), self.assertRaises(RecoveryConfigError):
                recovery_config(self.journal)

    def test_all_frozen_images_must_be_durably_validated_before_commit(self):
        for field, value in (("validated", False), ("flash_complete", False), ("offset", 200),
                             ("image_size", 200), ("queue_depth", 1), ("error", -9),
                             ("state", "VERIFYING"), ("campaign", CAMPAIGN + 1)):
            events = records()
            events[5]["status"]["targets"][1][field] = value
            self.write(events)
            with self.subTest(field=field), self.assertRaises(RecoveryConfigError):
                recovery_config(self.journal)
        events = records()
        events[5], events[6] = events[6], events[5]
        self.write(events)
        with self.assertRaisesRegex(RecoveryConfigError, "preceding complete validation"):
            recovery_config(self.journal)

    def test_commit_or_reboot_evidence_in_any_representation_blocks_recovery(self):
        for extra in (
            {"event": "REBOOTING", "device_event": 10},
            {"event": "STATE", "device_event": 10},
            {"event": "POSTBOOT_CHECK", "device_event": 11},
            {"event": "SUCCESS"},
            {"event": "STATUS", "status": {"phase": "REBOOTING", "targets": []}},
            {"event": "STATUS", "status": {"phase": "COMMITTING", "targets": []}},
            {"event": "STATE", "status": {"identity": IDS[0], "state": "COMMITTED"}},
            {"event": "STATE", "status": {"identity": IDS[1], "event_mask": 1 << 10}},
            {"event": "STATE", "status": {"identity": IDS[1], "event_mask": 1 << 11}},
        ):
            self.write(records() + [{"campaign": CAMPAIGN, **extra}])
            with self.subTest(extra=extra), self.assertRaises(RecoveryConfigError):
                recovery_config(self.journal)

    def test_manifest_cannot_change_and_mixed_campaigns_are_rejected(self):
        for mutate in (
            lambda events: events[3]["manifest"].update(build_id="other"),
            lambda events: events[-1].update(campaign=CAMPAIGN + 1),
            lambda events: events[2]["manifest"].update(mcuboot_image_hash="bad"),
            lambda events: events[2]["manifest"].update(build_id='bad"\n#include "injection"'),
            lambda events: events[2]["manifest"].update(artifact_size=100),
            lambda events: events[2]["manifest"].update(protocol=2),
        ):
            events = records()
            mutate(events)
            self.write(events)
            with self.subTest(events=events), self.assertRaises(ValueError):
                recovery_config(self.journal)

    def test_missing_repeated_or_truncated_campaign_cannot_replace_valid_output(self):
        self.write(records())
        generate(self.journal, self.output)
        before = {path.name: path.read_bytes() for path in self.output.iterdir()}
        for events in (records()[:6], records() + [records()[2]], records() + [records()[6]], records()[1:]):
            self.write(events)
            with self.assertRaises((ValueError, RuntimeError)):
                generate(self.journal, self.output)
            self.assertEqual(before, {path.name: path.read_bytes() for path in self.output.iterdir()})
        self.write(records())
        self.journal.write_bytes(self.journal.read_bytes()[:-1])
        with self.assertRaisesRegex(RecoveryConfigError, "incomplete final record"):
            generate(self.journal, self.output)

    def test_duplicate_json_keys_and_live_journal_change_are_rejected(self):
        self.write(records())
        self.journal.write_bytes(self.journal.read_bytes().replace(b'"event": "FAILED"', b'"event": "SUCCESS", "event": "FAILED"'))
        with self.assertRaisesRegex(RecoveryConfigError, "duplicate JSON key"):
            recovery_config(self.journal)
        self.write(records())
        raw = self.journal.read_bytes()
        with patch.object(Path, "read_bytes", side_effect=[raw, raw + b"\n"]):
            with self.assertRaisesRegex(RecoveryConfigError, "changed while preparing"):
                recovery_config(self.journal)

    def test_cli_default_is_file_generation_and_refusal_returns_nonzero(self):
        self.write(records())
        with redirect_stdout(io.StringIO()) as output:
            self.assertEqual(main(["--journal", str(self.journal), "--output-dir", str(self.output)]), 0)
        self.assertIn("no device accessed", output.getvalue())
        self.write(records() + [{"campaign": CAMPAIGN, "event": "SUCCESS"}])
        with redirect_stderr(io.StringIO()) as error:
            self.assertEqual(main(["--journal", str(self.journal), "--output-dir", str(self.output)]), 1)
        self.assertIn("refused", error.getvalue())


if __name__ == "__main__":
    unittest.main()
