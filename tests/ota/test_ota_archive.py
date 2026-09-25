"""Immutable build history retains exact artifacts for later GUI transitions."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "owntech/scripts"))
import ota_archive
from ota_archive import ArchiveError, archive_build
from ota_artifact import inspect_image, inspect_usb_image
from usb_artifact import inspect_plain_usb
from test_pc_artifact import artifact


def build(environment="OTA", *, compact=False):
    image_class = {"OTA": "receiver", "USB_LEAD": "lead", "USB": None}[environment]
    data = artifact(image_class=image_class, compact=compact)
    if environment == "USB":
        manifest = inspect_plain_usb(data, build_id="usb-test")
        manifest["build_proof"] = {"config_sha256": "11" * 32, "elf_sha256": "22" * 32,
                                   "ota_enabled": False, "recovery_enabled": False, "auto_confirmation": True}
    else:
        inspect = inspect_image if compact else inspect_usb_image
        manifest = inspect(data, image_class=image_class, build_id="archive-test", require_class=True)
    manifest["filename"] = "firmware.can.bin" if compact else "firmware.mcuboot.bin"
    return data, manifest


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ota-build-history-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name).resolve()

    def archive(self, environment="OTA", *, compact=False):
        data, manifest = build(environment, compact=compact)
        paths = archive_build(self.project, data, manifest, environment)
        return data, manifest, paths

    def test_all_application_classes_and_both_ota_formats(self):
        for environment, compact in (("USB", False), ("OTA", False), ("OTA", True),
                                     ("USB_LEAD", False), ("USB_LEAD", True)):
            with self.subTest(environment=environment, compact=compact):
                data, manifest, (image, metadata) = self.archive(environment, compact=compact)
                self.assertEqual(image.parent, self.project / "ota-artifacts/history" / environment / hashlib.sha256(data).hexdigest())
                self.assertEqual(image.name, "firmware.bin")
                self.assertEqual(metadata.name, "firmware.json")
                self.assertEqual(image.read_bytes(), data)
                saved = json.loads(metadata.read_bytes())
                self.assertEqual(saved, dict(manifest, filename="firmware.bin"))
                self.assertNotEqual(manifest["filename"], "firmware.bin")
                self.assertEqual(list(image.parent.glob("*.tmp")), [])

    def test_deduplicate_and_preserve_first_manifest_paths(self):
        data, manifest, paths = self.archive()
        image, metadata = paths
        old_bytes = metadata.read_bytes()
        old_times = (image.stat().st_mtime_ns, metadata.stat().st_mtime_ns)
        moved = copy.deepcopy(manifest)
        moved["filename"] = "a-new-build-name.bin"
        moved["profile"]["signing_key"] = "D:/another-checkout/key.pem"
        moved["signature"]["signing_key"] = "D:/another-checkout/key.pem"
        with patch.object(ota_archive, "_publish", side_effect=AssertionError("existing archive must not be rewritten")):
            self.assertEqual(archive_build(self.project, data, moved, "OTA"), paths)
        self.assertEqual(metadata.read_bytes(), old_bytes)
        self.assertEqual((image.stat().st_mtime_ns, metadata.stat().st_mtime_ns), old_times)

    def test_same_bytes_different_build_identity_refuses_without_overwrite(self):
        data, manifest, (image, metadata) = self.archive()
        saved = metadata.read_bytes()
        for field, value in (("build_id", "different-build"), ("version", "4.0.0+0"),
                             ("mcuboot_image_hash", "33" * 32), ("artifact_hash_domain", "different-domain")):
            incoming = copy.deepcopy(manifest)
            incoming[field] = value
            with self.subTest(field=field), self.assertRaises(ArchiveError):
                archive_build(self.project, data, incoming, "OTA")
            self.assertEqual(metadata.read_bytes(), saved)
            self.assertEqual(image.read_bytes(), data)
        for section, field, value in (("profile", "max_sectors", 127),
                                       ("signature", "key_sha256", "44" * 32),
                                       ("signature", "verified", True)):
            incoming = copy.deepcopy(manifest)
            incoming[section][field] = value
            with self.subTest(section=section, field=field), self.assertRaises(ArchiveError):
                archive_build(self.project, data, incoming, "OTA")
            self.assertEqual(metadata.read_bytes(), saved)

    def test_usb_build_proof_is_not_silently_replaced(self):
        data, manifest, (_, metadata) = self.archive("USB")
        original = metadata.read_bytes()
        manifest["build_proof"]["elf_sha256"] = "55" * 32
        with self.assertRaises(ArchiveError):
            archive_build(self.project, data, manifest, "USB")
        self.assertEqual(metadata.read_bytes(), original)

    def test_corrupt_image_is_never_repaired(self):
        data, manifest, (image, metadata) = self.archive()
        original_metadata = metadata.read_bytes()
        image.write_bytes(b"partial or corrupt image")
        with self.assertRaises(ArchiveError):
            archive_build(self.project, data, manifest, "OTA")
        self.assertEqual(image.read_bytes(), b"partial or corrupt image")
        self.assertEqual(metadata.read_bytes(), original_metadata)

    def test_corrupt_manifest_or_filename_is_never_repaired(self):
        data, manifest, (_, metadata) = self.archive()
        saved = json.loads(metadata.read_bytes())
        for corrupted in (b"{", b'{"artifact_size":1,"artifact_size":2}', b'{"value":NaN}',
                          json.dumps(dict(saved, filename="../other.bin")).encode(),
                          json.dumps(dict(saved, artifact_sha256="66" * 32)).encode()):
            metadata.write_bytes(corrupted)
            with self.subTest(corrupted=corrupted[:40]), self.assertRaises(ArchiveError):
                archive_build(self.project, data, manifest, "OTA")
            self.assertEqual(metadata.read_bytes(), corrupted)

    def test_interrupted_image_only_archive_completes_exact_bytes(self):
        data, manifest = build()
        original_publish = ota_archive._publish

        def interrupt_manifest(path, content):
            if path.name == "firmware.json":
                raise OSError("power lost between image and manifest publication")
            original_publish(path, content)

        with patch.object(ota_archive, "_publish", side_effect=interrupt_manifest), self.assertRaises(OSError):
            archive_build(self.project, data, manifest, "OTA")
        directory = self.project / "ota-artifacts/history/OTA" / manifest["artifact_sha256"]
        self.assertEqual((directory / "firmware.bin").read_bytes(), data)
        self.assertFalse((directory / "firmware.json").exists())
        image, metadata = archive_build(self.project, data, manifest, "OTA")
        self.assertEqual(image.read_bytes(), data)
        self.assertEqual(json.loads(metadata.read_bytes())["build_id"], manifest["build_id"])

    def test_manifest_without_image_refuses(self):
        data, manifest, (image, metadata) = self.archive()
        image.unlink()
        old_metadata = metadata.read_bytes()
        with self.assertRaises(ArchiveError):
            archive_build(self.project, data, manifest, "OTA")
        self.assertFalse(image.exists())
        self.assertEqual(metadata.read_bytes(), old_metadata)

    def test_publish_failure_leaves_no_partial_final_file_or_temp(self):
        data, manifest = build()
        with patch.object(ota_archive.os, "link", side_effect=OSError("publication failed")), self.assertRaises(OSError):
            archive_build(self.project, data, manifest, "OTA")
        directory = self.project / "ota-artifacts/history/OTA" / manifest["artifact_sha256"]
        self.assertEqual(list(directory.iterdir()), [])

    def test_concurrent_conflict_cannot_overwrite_existing_file(self):
        data, manifest = build()
        original_link = ota_archive.os.link

        def conflicting_writer(source, destination):
            if destination.name == "firmware.bin":
                destination.write_bytes(b"other process published different bytes")
            return original_link(source, destination)

        with patch.object(ota_archive.os, "link", side_effect=conflicting_writer), self.assertRaises(ArchiveError):
            archive_build(self.project, data, manifest, "OTA")
        directory = self.project / "ota-artifacts/history/OTA" / manifest["artifact_sha256"]
        self.assertEqual((directory / "firmware.bin").read_bytes(), b"other process published different bytes")
        self.assertFalse((directory / "firmware.json").exists())

    def test_invalid_inputs_are_rejected_before_creating_history(self):
        data, manifest = build()
        invalid = [dict(manifest, **{field: value}) for field, value in (
            ("artifact_sha256", "../escape"), ("artifact_sha256", "00" * 32),
            ("artifact_sha256", manifest["artifact_sha256"].upper()), ("artifact_size", len(data) - 1),
            ("artifact_size", True), ("image_class", "lead"), ("mcuboot_image_hash", ""),
            ("mcuboot_image_hash", "00" * 32), ("schema_version", 1), ("protocol", True),
            ("profile", None), ("signature", None), ("format", "unknown"),
            ("activation_trailer", 1), ("protected_tlv_size", -1), ("useful_size", len(data) + 1),
            ("build_id", ""), ("extra_metadata", float("nan")))]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ArchiveError):
                archive_build(self.project, data, value, "OTA")
        for environment in ("../OTA", "ota", "OTA_RECOVERY", "OTA_TRANSITION", "USB", "USB_LEAD", "", None, []):
            with self.subTest(environment=environment), self.assertRaises(ArchiveError):
                archive_build(self.project, data, manifest, environment)
        with self.assertRaises(ArchiveError):
            archive_build(self.project, bytearray(data), manifest, "OTA")
        with self.assertRaises(ArchiveError):
            archive_build(self.project, b"", manifest, "OTA")
        self.assertFalse((self.project / "ota-artifacts").exists())

    def test_usb_requires_complete_build_evidence(self):
        data, manifest = build("USB")
        for field, value in (("auto_confirmation", False), ("ota_enabled", True),
                             ("recovery_enabled", True), ("elf_sha256", ""), ("config_sha256", "")):
            incoming = copy.deepcopy(manifest)
            incoming["build_proof"][field] = value
            with self.subTest(field=field), self.assertRaises(ArchiveError):
                archive_build(self.project, data, incoming, "USB")
        self.assertFalse((self.project / "ota-artifacts").exists())

    def test_history_symlink_cannot_escape_project(self):
        data, manifest = build()
        with tempfile.TemporaryDirectory(prefix="ota-history-outside-") as outside:
            link = self.project / "ota-artifacts"
            try:
                link.symlink_to(outside, target_is_directory=True)
            except OSError:
                self.skipTest("creating symbolic links requires additional Windows privileges")
            try:
                with self.assertRaises(ArchiveError):
                    archive_build(self.project, data, manifest, "OTA")
                self.assertEqual(list(Path(outside).iterdir()), [])
            finally:
                link.unlink()


if __name__ == "__main__":
    unittest.main()
