"""Exact transition guards for unchanged terminal OTA v2 firmware."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch
import zlib

from test_pc_artifact import artifact

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
from ota_artifact import inspect_image, inspect_usb_image
from prepare_ota_transition import (TransitionConfigError, transition_config, generate,
                                    render_header, verify_config, main)
from lead_update import Campaign, Journal
from test_pc_campaign import Clock

LEAD = "eeeeeeeeeeeeeeee"
TARGETS = ["0102030405060708", "1112131415161718"]
CAMPAIGN = 0x12345678ABCDEF01
COMMIT = CAMPAIGN & 0xFFFFFFFF


class CampaignWire:
    """Current USB row shapes, including async commands and postboot counters."""
    def __init__(self, manifest, data, lead_info):
        self.manifest, self.data, self.lead_info = manifest, data, lead_info
        self.phase, self.offset, self.campaign = "IDLE", 0, 0
        self.commit_pending = False
        self.source_requests = []

    def snapshot(self):
        active = self.phase in ("RECOVERY_REQUIRED", "SUCCESS")
        valid = self.phase == "ALL_VALIDATED"
        rows = [{"identity": value, "address": index + 1, "role": "follower",
                 "image_class": "receiver", "available": True, "compatible": True,
                 "state": "VALID" if valid else ("SUCCESS" if active else "IDLE"),
                 "campaign": self.campaign, "error": 0, "queue_depth": 0,
                 "image_size": self.manifest["artifact_size"] if self.campaign else 0,
                 "offset": len(self.data) if valid else 0,
                 "flash_complete": valid, "validated": valid,
                 "healthy": True, "confirmed": True,
                 "version": self.manifest["version"] if active else "0.0.1+0",
                 "build_id": self.manifest["build_id"] if active else "previous-receiver",
                 "mcuboot_image_hash": bytes.fromhex(self.manifest["mcuboot_image_hash"] if active else "cc" * 32)}
                for index, value in enumerate(reversed(TARGETS))]
        result = {"phase": self.phase, "state": self.phase, "campaign": self.campaign,
                  "error": 0, "target_count": len(rows), "targets": rows,
                  "source_length": 0, "source_offset": self.offset,
                  "source_campaign": self.campaign}
        if self.phase == "CAN_TRANSFER":
            result["source_length"] = min(256, len(self.data) - self.offset)
        return result

    def request(self, command, payload):
        if command == "info":
            return dict(self.lead_info)
        if command == "discover":
            return self.snapshot()
        if command == "stage_begin":
            self.phase = "SOURCE_OPEN"
            self.campaign = payload["campaign"]
            return {"state": "ACCEPTED"}
        if command == "stage_end":
            assert self.phase == "SOURCE_OPEN"
            self.phase = "SOURCE_READY"
            return {"state": "ACCEPTED"}
        if command == "start":
            assert payload["targets"] == TARGETS and self.phase == "SOURCE_READY"
            self.phase = "CAN_TRANSFER"
            return {"state": "ACCEPTED"}
        if command == "stage_data":
            length = min(256, len(self.data) - self.offset)
            assert payload == {"campaign": self.campaign, "offset": self.offset,
                               "data": self.data[self.offset:self.offset + length]}
            self.source_requests.append((self.offset, length))
            self.offset += length
            if self.offset == len(self.data):
                self.phase = "ALL_VALIDATED"
            return {"rc": 0, "offset": self.offset}
        if command == "commit":
            assert self.phase == "ALL_VALIDATED"
            self.commit_pending = True
            return {"state": "ACCEPTED"}
        if command == "status":
            result = self.snapshot()
            if self.commit_pending:
                # The USB reply can precede the commit worker's first update.
                self.commit_pending = False
                self.phase = "RECOVERY_REQUIRED"
            return result
        if command == "reconcile":
            assert payload["targets"] == TARGETS and self.phase == "RECOVERY_REQUIRED"
            self.phase = "SUCCESS"
            return self.snapshot()
        raise AssertionError("unexpected command " + command)


def successful_journal(manifest):
    initial = [{"identity": value, "role": "follower", "address": index + 1,
                "confirmed": True, "healthy": True, "available": True, "compatible": True,
                "mcuboot_image_hash": "cc" * 32} for index, value in enumerate(TARGETS)]
    valid = [{"identity": value, "campaign": CAMPAIGN, "state": "VALID", "error": 0,
              "image_size": manifest["artifact_size"], "offset": manifest["artifact_size"],
              "validated": True, "flash_complete": True} for value in TARGETS]
    terminal = [dict(row, state="SUCCESS", healthy=True, confirmed=True,
                     mcuboot_image_hash=manifest["mcuboot_image_hash"], version=manifest["version"],
                     build_id=manifest["build_id"]) for row in valid]
    events = [
        {"event": "USB_SELECTED", "usb_serial": "lead-usb"},
        {"event": "PROBE_LEAD", "identity": LEAD},
        {"event": "DISCOVER", "lead_identity": LEAD, "targets": list(TARGETS),
         "inventory": initial, "manifest": copy.deepcopy(manifest)},
        {"event": "PC_SOURCE_OPEN", "identity": LEAD, "manifest": copy.deepcopy(manifest)},
        {"event": "START_REQUEST", "targets": list(TARGETS)},
        {"event": "STATUS", "status": {"phase": "ALL_VALIDATED", "campaign": CAMPAIGN,
                                         "target_count": len(TARGETS), "targets": valid}},
        {"event": "COMMIT_REQUEST", "targets": list(TARGETS)},
        {"event": "RECONCILE_BEGIN", "targets": list(TARGETS)},
        {"event": "STATUS", "status": {"phase": "SUCCESS", "campaign": CAMPAIGN,
                                         "target_count": len(TARGETS), "targets": terminal}},
        {"event": "SUCCESS", "targets": list(TARGETS)},
    ]
    return [dict(campaign=CAMPAIGN, **event) for event in events]


class TransitionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.image = self.root / "firmware.can.bin"
        self.manifest_path = self.root / "firmware.can.json"
        self.info_path = self.root / "board.json"
        self.journal = self.root / "campaign.jsonl"
        self.output = self.root / "generated"
        self.config_path = self.output / "owntech_ota_recovery_config.json"
        self.campaign_manifest = inspect_image(artifact(compact=True), build_id="receiver-live")
        self.records = successful_journal(self.campaign_manifest)
        self.set_source()

    def write_json(self, path, value):
        path.write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")

    def save(self):
        self.write_json(self.manifest_path, self.manifest)
        self.write_json(self.info_path, self.info)
        self.journal.write_text("".join(json.dumps(record) + "\n" for record in self.records), encoding="utf-8")

    def set_source(self, image_class="receiver", *, usb=False, no_campaign=False, body_size=128):
        data = artifact(body_size=body_size, compact=not usb, image_class=image_class)
        self.image.write_bytes(data)
        inspect = inspect_usb_image if usb else inspect_image
        self.manifest = inspect(data, build_id=image_class + "-live", image_class=image_class)
        self.info = {"service": "owntech-ota", "protocol": 2,
                     "identity": LEAD if image_class == "lead" else TARGETS[0],
                     "usb_serial": "lead-usb" if image_class == "lead" else "receiver-usb",
                     "image_class": image_class, "phase": "IDLE" if no_campaign else "SUCCESS",
                     "active_confirmed": True, "slot_available": True, "local_healthy": True,
                     "healthy": True, "error": 0,
                     "slot_size": self.manifest["profile"]["slot_size"], "useful_capacity": 221184,
                     **{key: self.manifest[key] for key in ("hardware_id", "layout_id", "bootloader_id",
                         "version", "build_id", "mcuboot_image_hash")}}
        self.save()

    def configure(self, *, no_campaign=False):
        return transition_config(self.image, self.manifest_path, self.info_path,
                                 journal_path=None if no_campaign else self.journal, no_campaign=no_campaign)

    def generate(self, *, no_campaign=False):
        return generate(self.image, self.manifest_path, self.info_path, self.output,
                        journal_path=None if no_campaign else self.journal, no_campaign=no_campaign)

    def test_receiver_exact_168_byte_terminal_record_and_crc(self):
        config = self.configure()
        data = bytes.fromhex(config["local_record_hex"])
        self.assertEqual(len(data), config["local_length"])
        self.assertEqual(len(data), 168)
        self.assertEqual(data[:4], b"OTL2")
        self.assertEqual(struct.unpack_from("<HHQII", data, 4),
                         (2, 168, CAMPAIGN, COMMIT, self.campaign_manifest["artifact_size"]))
        self.assertEqual(data[24:28], bytes([12, 2, 1, 0]))
        self.assertEqual(data[28:36].hex(), LEAD)
        self.assertEqual(data[36:68].hex(), self.campaign_manifest["artifact_sha256"])
        self.assertEqual(data[68:100].hex(), self.campaign_manifest["mcuboot_image_hash"])
        self.assertEqual(data[100:132].rstrip(b"\0"), self.campaign_manifest["version"].encode())
        self.assertEqual(data[132:164].rstrip(b"\0"), b"receiver-live")
        self.assertEqual(struct.unpack_from("<I", data, 164)[0], zlib.crc32(data[:164]))
        self.assertEqual(config["fleet_record_hex"], "")
        self.assertEqual(config["role"], 1)

    def test_lead_exact_308_byte_fleet_record_preserves_its_distinct_source(self):
        self.set_source("lead")
        config = self.configure()
        data = bytes.fromhex(config["fleet_record_hex"])
        self.assertEqual(len(data), 308)
        self.assertEqual(data[:4], b"OTA3")
        self.assertEqual(struct.unpack_from("<IQ", data, 4), (308, CAMPAIGN))
        self.assertEqual(data[36], 2)
        self.assertEqual(data[37:69].hex(), self.campaign_manifest["artifact_sha256"])
        self.assertEqual(data[69:101].hex(), self.campaign_manifest["mcuboot_image_hash"])
        self.assertEqual(data[165], 1)
        self.assertEqual(data[166:182].hex(), "".join(TARGETS))
        self.assertEqual(data[182:294], bytes(112))
        self.assertEqual(data[294:300], bytes([2, 0, 12, 0, 0, 0]))
        self.assertEqual(struct.unpack_from("<II", data, 300), (COMMIT, zlib.crc32(data[:304])))
        self.assertEqual(config["local_record_hex"], "")
        self.assertEqual(config["role"], 2)
        self.assertNotEqual(config["original_hash"], self.campaign_manifest["mcuboot_image_hash"])
        self.assertEqual(config["original_hash"], self.manifest["mcuboot_image_hash"])

    def test_source_may_be_exact_padded_usb_or_compact_image(self):
        for role in ("receiver", "lead"):
            for usb in (False, True):
                with self.subTest(role=role, usb=usb):
                    self.set_source(role, usb=usb)
                    config = self.configure()
                    self.assertEqual(config["source_image_sha256"], hashlib.sha256(self.image.read_bytes()).hexdigest())
                    self.assertEqual(config["image_class"], role)

    def test_explicit_no_campaign_uses_absent_records_without_new_runtime_fields(self):
        for role in ("receiver", "lead"):
            self.set_source(role, no_campaign=True)
            self.info.update(phase="WAITING_CAN", healthy=False, local_healthy=True)
            self.save()
            config = self.configure(no_campaign=True)
            self.assertEqual((config["local_length"], config["fleet_length"]), (0, 0))
            self.assertEqual((config["campaign_id"], config["commit_id"]), (0, 0))
            self.assertIsNone(config["journal_sha256"])
            self.assertIn(b"LOCAL_BYTES {0}", render_header(config))
            self.assertIn(b"FLEET_BYTES {0}", render_header(config))
        for kwargs in ({}, {"journal_path": self.journal, "no_campaign": True}):
            with self.assertRaises(TransitionConfigError):
                transition_config(self.image, self.manifest_path, self.info_path, **kwargs)

    def test_optional_new_fields_are_checked_without_becoming_required(self):
        self.info.update(campaign=CAMPAIGN, commit_id=COMMIT, state="SUCCESS", maintenance=False)
        self.records[-2]["status"]["commit_id"] = COMMIT
        for row in self.records[-2]["status"]["targets"]:
            row["commit_id"] = COMMIT
        self.save()
        self.configure()
        for field, value in (("campaign", 0), ("commit_id", 0), ("state", "COMMITTED"), ("maintenance", True)):
            original = self.info[field]
            self.info[field] = value
            self.save()
            with self.subTest(field=field), self.assertRaises(TransitionConfigError):
                self.configure()
            self.info[field] = original
        self.set_source(no_campaign=True)
        self.info["campaign"] = CAMPAIGN
        self.save()
        with self.assertRaises(TransitionConfigError):
            self.configure(no_campaign=True)

    def test_zero_low_campaign_word_uses_nonzero_commit_one(self):
        for row in self.records:
            row["campaign"] = 1 << 32
            status = row.get("status")
            if status:
                status["campaign"] = 1 << 32
                for target in status["targets"]:
                    target["campaign"] = 1 << 32
        self.save()
        self.assertEqual(self.configure()["commit_id"], 1)

    def test_hostile_journal_rejects_missing_barrier_commit_reconcile_or_success(self):
        baseline = copy.deepcopy(self.records)
        for event in ("DISCOVER", "PC_SOURCE_OPEN", "START_REQUEST", "COMMIT_REQUEST", "RECONCILE_BEGIN", "SUCCESS"):
            self.records = [row for row in baseline if row["event"] != event]
            self.save()
            with self.subTest(event=event), self.assertRaises(TransitionConfigError):
                self.configure()
        self.records = [row for row in baseline if row.get("status", {}).get("phase") != "ALL_VALIDATED"]
        self.save()
        with self.assertRaises(TransitionConfigError):
            self.configure()
        self.records = copy.deepcopy(baseline)
        barrier = self.records.pop(5)
        self.records.insert(6, barrier)  # A later status cannot authorize COMMIT.
        self.save()
        with self.assertRaises(TransitionConfigError):
            self.configure()

    def test_hostile_journal_rejects_duplicates_wrong_roster_and_activity_after_success(self):
        baseline = copy.deepcopy(self.records)
        mutations = [lambda rows: rows.append(dict(rows[0])),
                     lambda rows: rows.insert(3, copy.deepcopy(rows[2])),
                     lambda rows: rows[4].update(targets=list(reversed(TARGETS))),
                     lambda rows: rows[-1].update(targets=TARGETS[:1]),
                     lambda rows: rows[0].update(campaign=CAMPAIGN + 1),
                     lambda rows: rows[1].update(identity=TARGETS[0]),
                     lambda rows: rows.insert(2, dict(rows[0], usb_serial="different")),
                     lambda rows: rows[3]["manifest"].update(build_id="wrong")]
        for change in mutations:
            self.records = copy.deepcopy(baseline)
            change(self.records)
            self.save()
            with self.subTest(change=change), self.assertRaises(TransitionConfigError):
                self.configure()

    def test_terminal_snapshot_requires_every_healthy_confirmed_exact_receiver(self):
        baseline = copy.deepcopy(self.records)
        faults = {"state": "RECOVERY_REQUIRED", "healthy": False, "confirmed": False,
                  "mcuboot_image_hash": "dd" * 32, "version": "0.0.0+0", "build_id": "wrong",
                  "campaign": CAMPAIGN - 1, "image_size": 1, "error": -9, "commit_id": COMMIT + 1,
                  "image_class": "lead", "role": "lead"}
        for field, value in faults.items():
            self.records = copy.deepcopy(baseline)
            self.records[-2]["status"]["targets"][0][field] = value
            self.save()
            with self.subTest(field=field), self.assertRaises(TransitionConfigError):
                self.configure()
        self.records = copy.deepcopy(baseline)
        self.records[-2]["status"]["targets"].pop()
        self.save()
        with self.assertRaises(TransitionConfigError):
            self.configure()

    def test_board_and_source_must_match_and_be_idle_confirmed(self):
        baseline = copy.deepcopy(self.info)
        faults = {"identity": "3333333333333333", "usb_serial": "", "protocol": 1,
                  "image_class": "lead", "active_confirmed": False, "slot_available": False,
                  "hardware_id": 1, "mcuboot_image_hash": "dd" * 32, "build_id": "wrong",
                  "phase": "REBOOTING", "error": False, "maintenance": True}
        for field, value in faults.items():
            self.info = dict(baseline, **{field: value})
            self.save()
            with self.subTest(field=field), self.assertRaises(TransitionConfigError):
                self.configure()
        self.info = dict(baseline, healthy=False, local_healthy=False)
        self.save()
        with self.assertRaises(TransitionConfigError):
            self.configure()

    def test_wrong_image_class_unsigned_class_bad_manifest_or_damaged_bytes_rejected(self):
        original = self.image.read_bytes()
        for data in (artifact(compact=True, image_class="lead"), artifact(compact=True, image_class=None),
                     original[:520] + bytes([original[520] ^ 1]) + original[521:]):
            self.image.write_bytes(data)
            with self.subTest(data=data[:32]), self.assertRaises(ValueError):
                self.configure()
        self.image.write_bytes(original)
        self.manifest["artifact_sha256"] = "dd" * 32
        self.save()
        with self.assertRaises(TransitionConfigError):
            self.configure()

    def test_lead_current_image_and_usb_serial_are_separate_from_campaign_payload(self):
        self.set_source("lead")
        self.info["usb_serial"] = "receiver-usb"
        self.save()
        with self.assertRaises(TransitionConfigError):
            self.configure()
        self.info["usb_serial"] = "lead-usb"
        self.info["mcuboot_image_hash"] = self.campaign_manifest["mcuboot_image_hash"]
        self.save()
        with self.assertRaises(TransitionConfigError):
            self.configure()

    def test_duplicate_keys_truncated_journal_and_json_nan_are_refused(self):
        original = self.info_path.read_bytes()
        for raw in (b'{"identity":"' + TARGETS[0].encode() + b'","identity":"' + LEAD.encode() + b'"}', b'{"value":NaN}'):
            self.info_path.write_bytes(raw)
            with self.assertRaises(ValueError):
                self.configure()
        self.info_path.write_bytes(original)
        self.journal.write_bytes(self.journal.read_bytes().rstrip(b"\n"))
        with self.assertRaises(TransitionConfigError):
            self.configure()

    def test_semantic_token_and_generated_header_are_stable_and_verified(self):
        config = self.generate()
        self.assertEqual(config, verify_config(self.config_path))
        self.assertEqual(config["build_id"], "transition-" + config["header_sha256"][:20])
        self.assertEqual(config["header_sha256"], hashlib.sha256(render_header(config)).hexdigest())
        self.assertEqual(config, self.generate())
        self.manifest_path.write_text(json.dumps(self.manifest, indent=4))
        same_semantics = self.configure()
        self.assertEqual(config["token"], same_semantics["token"])
        with self.assertRaisesRegex(TransitionConfigError, "saved inputs"):
            verify_config(self.config_path)

    def test_verification_rederives_config_and_rejects_edited_header_or_inputs(self):
        config = self.generate()
        bad = dict(config, local_record_hex="00" * 168)
        self.write_json(self.config_path, bad)
        with self.assertRaises(TransitionConfigError):
            verify_config(self.config_path)
        self.write_json(self.config_path, config)
        header_path = self.config_path.with_suffix(".h")
        header = header_path.read_bytes()
        header_path.write_bytes(header + b"#define MODIFIED 1\n")
        with self.assertRaisesRegex(TransitionConfigError, "header"):
            verify_config(self.config_path)
        header_path.write_bytes(header)
        self.info["usb_serial"] = "another-receiver"
        self.save()
        with self.assertRaisesRegex(TransitionConfigError, "saved inputs"):
            verify_config(self.config_path)

    def test_toctou_input_changes_are_detected_before_config_is_returned(self):
        original = Path.read_bytes
        for target in (self.image, self.manifest_path, self.info_path, self.journal):
            target = target.resolve()
            reads = [0]

            def read(path):
                raw = original(path)
                if path == target:
                    reads[0] += 1
                    if reads[0] > 1:
                        return raw + b"changed"
                return raw

            with self.subTest(target=target.name), patch.object(Path, "read_bytes", read):
                with self.assertRaisesRegex(TransitionConfigError, "input changed"):
                    self.configure()

    def test_cli_only_writes_local_config_and_does_not_overwrite_on_failure(self):
        args = ["--image", str(self.image), "--manifest", str(self.manifest_path),
                "--board-info", str(self.info_path), "--journal", str(self.journal),
                "--output-dir", str(self.output)]
        with redirect_stdout(io.StringIO()):
            self.assertEqual(main(args), 0)
        original = self.config_path.read_bytes()
        self.info["phase"] = "FAILED"
        self.save()
        with redirect_stderr(io.StringIO()):
            self.assertEqual(main(args), 1)
        self.assertEqual(self.config_path.read_bytes(), original)

    def actual_success_journal(self):
        """Run the production journal writer; do not hand-author its events."""
        lead = inspect_image(artifact(compact=True, image_class="lead"),
                             build_id="lead-live", image_class="lead")
        info = dict(self.info, identity=LEAD, image_class="lead", role="lead",
                    phase="IDLE", available=True, usb_serial="lead-usb",
                    **{key: lead[key] for key in ("mcuboot_image_hash", "version", "build_id")})
        transport = CampaignWire(self.campaign_manifest, artifact(compact=True), info)
        self.journal.unlink()
        journal = Journal(self.journal, CAMPAIGN)
        try:
            journal.emit("USB_SELECTED", usb_serial="lead-usb")
            clock = Clock()
            client = Campaign(transport, self.campaign_manifest, artifact(compact=True), journal,
                              expected_ids=TARGETS, clock=clock, sleep=clock.sleep,
                              timeout=30, output=lambda _: None)
            self.assertEqual(client.run(), "SUCCESS")
        finally:
            journal.close()
        self.assertGreater(len(transport.source_requests), 1)
        raw = self.journal.read_bytes()
        records = [json.loads(line) for line in raw.splitlines()]
        self.assertEqual(records[-2]["event"], "STATUS")
        self.assertEqual(records[-1]["event"], "SUCCESS")
        terminal = records[-2]["status"]["targets"]
        self.assertTrue(all(row["state"] == "SUCCESS" and not row["offset"]
                            and not row["validated"] and "commit_id" not in row for row in terminal))
        self.assertTrue(any(row["event"] == "STATE" for row in records))
        return raw

    def test_real_campaign_journal_accepts_terminal_receiver_and_dedicated_lead(self):
        raw = self.actual_success_journal()
        for role in ("receiver", "lead"):
            with self.subTest(role=role):
                self.set_source(role)
                self.journal.write_bytes(raw)
                config = self.generate()
                self.assertEqual(config["targets"], TARGETS)
                self.assertEqual(config["commit_id"], COMMIT)
                self.assertEqual(config, verify_config(self.config_path))

    def test_generated_header_is_accepted_by_actual_cpp_transition_core(self):
        raw = self.actual_success_journal()
        for role in ("receiver", "lead"):
            for no_campaign in (False, True):
                with self.subTest(role=role, no_campaign=no_campaign):
                    self.set_source(role, no_campaign=no_campaign)
                    self.journal.write_bytes(raw)
                    self.generate(no_campaign=no_campaign)
                    self.compile_fixture("transition_config_fixture")

    def test_generated_record_matches_actual_storage_and_next_boot_has_no_stale_campaign(self):
        # The existing real-storage shim has 1 KiB slots, so use a small signed
        # container while keeping the actual production NVS layouts unchanged.
        self.campaign_manifest = inspect_image(artifact(body_size=16, compact=True), build_id="receiver-live")
        self.records = successful_journal(self.campaign_manifest)
        for role in ("receiver", "lead"):
            with self.subTest(role=role):
                self.set_source(role, body_size=16)
                self.generate()
                self.compile_fixture("transition_storage_fixture", storage=True)

    def compile_fixture(self, fixture, *, storage=False):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None and os.name == "nt":
            candidate = Path("C:/Program Files/LLVM/bin/clang++.exe")
            if candidate.is_file():
                compiler = str(candidate)
        self.assertIsNotNone(compiler, "C++ compiler required")
        symbol = "ota_" + fixture + "_run"
        target = self.root / (fixture + (".dll" if os.name == "nt" else ""))
        command = [compiler, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror"]
        includes = [self.output, ROOT / "owntech/recovery"]
        sources = [ROOT / "owntech/recovery/recovery_core.cpp",
                   ROOT / "owntech/recovery/transition_core.cpp",
                   ROOT / "tests/ota" / (fixture + ".cpp")]
        if storage:
            ota = ROOT / "zephyr/modules/owntech_ota/zephyr"
            includes += [ROOT / "tests/ota/storage_shim", ota / "public_api", ota / "src",
                         ROOT / "zephyr/modules/owntech_flash_driver/zephyr/public_api"]
            sources.append(ota / "src/ota_protocol.c")
            command.append("-fshort-enums")
        if os.name == "nt":
            includes.insert(0, ROOT / "tests/ota/core_shim")
            sources.append(ROOT / "tests/ota/core_shim.cpp")
            command += ["-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe",
                        "-fno-exceptions", "-fno-rtti", "-nostdlib", "-shared", "-fuse-ld=lld",
                        "-DOWNTECH_FREESTANDING_TEST", "-Wl,/noentry", "-Wl,/export:" + symbol]
        command += ["-I" + str(path) for path in includes]
        command += [str(path) for path in sources] + ["-o", str(target)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        run = ([sys.executable, "-c", "import ctypes,sys;rc=getattr(ctypes.CDLL(sys.argv[1]),sys.argv[2])();print(rc);sys.exit(bool(rc))", str(target), symbol]
               if os.name == "nt" else [str(target)])
        completed = subprocess.run(run, capture_output=True, text=True, timeout=30)
        self.assertEqual(completed.returncode, 0,
                         fixture + ".cpp failed at line " + completed.stdout + completed.stderr)


if __name__ == "__main__":
    unittest.main()
