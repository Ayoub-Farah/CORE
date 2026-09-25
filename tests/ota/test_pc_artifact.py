import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "owntech" / "scripts"))
from ota_artifact import ArtifactError, BOOT_MAGIC, DEFAULT_PROFILE, inspect_image, inspect_usb_image


def artifact(body_size=128, compact=False, image_class="receiver"):
    protected = b"" if image_class is None else (struct.pack("<HHHH", 0x6908, 8 + len(image_class), 0xA0, len(image_class)) + image_class.encode())
    header = struct.pack("<IIHHIIBBHII", 0x96F3B83D, 0, 512, len(protected), body_size, 0, 1, 2, 3, 4, 0)
    image = header + bytes(512 - len(header)) + bytes([0x43]) * body_size + protected
    entries = b"".join(struct.pack("<HH", tag, len(value)) + value for tag, value in (
        (0x10, hashlib.sha256(image).digest()), (1, bytes(32)), (0x20, bytes(256))))
    image += struct.pack("<HH", 0x6907, len(entries) + 4) + entries
    if compact:
        return image
    return image + b"\xff" * (DEFAULT_PROFILE["slot_size"] - len(image) - 16) + BOOT_MAGIC


class ArtifactTests(unittest.TestCase):
    def test_exact_padded_domains_and_separate_limits(self):
        data = artifact()
        result = inspect_usb_image(data)
        self.assertEqual(result["artifact_size"], 227328)
        self.assertLess(result["useful_size"], 221184)
        self.assertNotEqual(result["artifact_sha256"], result["mcuboot_image_hash"])
        self.assertEqual(result["artifact_sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(result["version"], "1.2.3+4")
        self.assertTrue(result["activation_trailer"])
        self.assertFalse(result["signature"]["verified"])

    def test_compact_v2_and_padded_rejected_for_can(self):
        data = artifact(compact=True)
        result = inspect_image(data)
        self.assertEqual(result["protocol"], 2)
        self.assertEqual(result["artifact_size"], result["useful_size"])
        self.assertEqual(result["image_class"], "receiver")
        self.assertFalse(result["activation_trailer"])
        for bad in (artifact(), data + b"\xff", data + BOOT_MAGIC, data[:-1],
                    artifact(compact=True, image_class="lead"), artifact(compact=True, image_class=None)):
            with self.assertRaises(ArtifactError):
                inspect_image(bad)
        self.assertEqual(inspect_image(artifact(compact=True, image_class="lead"), image_class="lead")["image_class"], "lead")
        with self.assertRaises(ArtifactError):
            inspect_image(data, image_class="unknown")

    def test_corrupt_body_padding_trailer_and_length(self):
        for offset in (512, 200000, -1):
            data = bytearray(artifact())
            data[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ArtifactError):
                inspect_usb_image(data)
        with self.assertRaises(ArtifactError):
            inspect_usb_image(artifact() + b"\xff")

    def test_tlv_capacity_version_and_signing_key(self):
        with self.assertRaises(ArtifactError):
            inspect_image(artifact(221000, compact=True))
        with self.assertRaises(ArtifactError):
            inspect_image(artifact(compact=True), version="2.0.0")
        with self.assertRaises(ArtifactError):
            inspect_image(artifact(compact=True), {"public_key_sha256": "11" * 32})


if __name__ == "__main__":
    unittest.main()
