"""Plain USB artifact and build proof must reject OTA and repair applications."""
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

from test_pc_artifact import artifact

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/scripts"))
from usb_artifact import inspect_plain_usb, verify_usb_build, usb_artifact_post_action
from ota_artifact import ArtifactError


CONFIG = b"CONFIG_BOOTLOADER_MCUBOOT=y\n# CONFIG_OWNTECH_OTA is not set\n"
ELF = b"\x7fELFtest USB application"
SYMBOLS = "08010100 t _img_validation()\n08010200 T main\n"


class Environment(dict):
    def __init__(self, root):
        super().__init__()
        self.project = root
        self.build = root / ".pio/build/USB"
        self.post = []
        self.dependencies = []

    def subst(self, text):
        return (text.replace("$PROJECT_DIR", str(self.project)).replace("$BUILD_DIR", str(self.build))
                .replace("${PROGNAME}", "firmware").replace("$PIOENV", "USB"))

    def GetProjectOption(self, key, default=None):
        return default

    def BoardConfig(self):
        return {}

    def PioPlatform(self):
        return SimpleNamespace(get_package_dir=lambda package: str(self.project / package))

    def Alias(self, name):
        return name

    def AddPostAction(self, target, action):
        self.post.append((target, action))

    def Depends(self, target, dependency):
        self.dependencies.append((target, dependency))


class PlainUSBTests(unittest.TestCase):
    def test_plain_padded_image_has_no_invented_build_proof(self):
        data = artifact(image_class=None)
        result = inspect_plain_usb(data)
        self.assertEqual(result["image_class"], "usb")
        self.assertEqual(result["execution_profile"], "usb")
        self.assertEqual(result["artifact_sha256"], hashlib.sha256(data).hexdigest())
        self.assertTrue(result["activation_trailer"])
        self.assertNotIn("build_proof", result)

    def test_class_tag_compact_and_corrupt_images_are_rejected(self):
        corrupt = bytearray(artifact(image_class=None))
        corrupt[512] ^= 1
        for data in (artifact(image_class="receiver"), artifact(image_class="lead"),
                     artifact(compact=True, image_class=None), bytes(corrupt)):
            with self.subTest(kind=data[:32]), self.assertRaises(ArtifactError):
                inspect_plain_usb(data)

    def test_proof_hashes_actual_config_and_elf(self):
        proof = verify_usb_build(CONFIG, ELF, SYMBOLS)
        self.assertEqual(proof["config_sha256"], hashlib.sha256(CONFIG).hexdigest())
        self.assertEqual(proof["elf_sha256"], hashlib.sha256(ELF).hexdigest())
        self.assertIs(proof["ota_enabled"], False)
        self.assertIs(proof["recovery_enabled"], False)
        self.assertIs(proof["auto_confirmation"], True)

    def test_config_cannot_enable_ota_recovery_transition_or_omit_mcuboot(self):
        invalid = [b"# CONFIG_BOOTLOADER_MCUBOOT is not set\n"]
        invalid += [CONFIG + (name + "=y\n").encode() for name in
                    ("CONFIG_OWNTECH_OTA", "CONFIG_OWNTECH_OTA_RECOVERY", "CONFIG_OWNTECH_OTA_TRANSITION")]
        invalid.append(CONFIG + b"CONFIG_BOOTLOADER_MCUBOOT=n\n")
        for config in invalid:
            with self.subTest(config=config), self.assertRaises(ArtifactError):
                verify_usb_build(config, ELF, SYMBOLS)

    def test_symbols_require_defined_confirmation_and_reject_runtime_entries(self):
        invalid = ["08010200 T main\n", " U _img_validation()\n", "08010100 B _img_validation\n"]
        invalid += [SYMBOLS + "08010300 t " + name + "\n" for name in
                    ("initialize_runtime()", "ota_runtime_command()", "ota_recovery_run()", "ota_transition_run()")]
        for symbols in invalid:
            with self.subTest(symbols=symbols), self.assertRaises(ArtifactError):
                verify_usb_build(CONFIG, ELF, symbols)
        with self.assertRaises(ArtifactError):
            verify_usb_build(CONFIG, b"not an ELF", SYMBOLS)

    def test_post_action_archives_only_a_verified_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            env = Environment(Path(directory))
            (env.build / "zephyr").mkdir(parents=True)
            (env.build / "zephyr/.config").write_bytes(CONFIG)
            (env.build / "firmware.elf").write_bytes(ELF)
            data = artifact(image_class=None)
            (env.build / "firmware.mcuboot.bin").write_bytes(data)
            with patch("usb_artifact.subprocess.run", return_value=SimpleNamespace(stdout=SYMBOLS)), redirect_stdout(io.StringIO()):
                self.assertEqual(usb_artifact_post_action([], [], env), 0)
            path = env.project / "ota-artifacts/USB/firmware.usb.json"
            result = json.loads(path.read_text())
            self.assertEqual(result["image_class"], "usb")
            self.assertEqual(result["build_proof"], verify_usb_build(CONFIG, ELF, SYMBOLS))
            self.assertEqual(result["version"], "1.2.3+4")
            self.assertEqual(result["build_id"], "usb-" + hashlib.sha256(ELF).hexdigest()[:20])
            self.assertEqual(path.with_name("firmware.mcuboot.bin").read_bytes(), data)
            before = path.read_bytes()
            (env.build / "zephyr/.config").write_bytes(CONFIG + b"CONFIG_OWNTECH_OTA=y\n")
            with patch("usb_artifact.subprocess.run", return_value=SimpleNamespace(stdout=SYMBOLS)), redirect_stdout(io.StringIO()):
                self.assertEqual(usb_artifact_post_action([], [], env), 1)
            self.assertEqual(path.read_bytes(), before)

    def test_hook_orders_validation_before_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            env = Environment(Path(directory))
            script = ModuleType("SCons.Script")
            script.COMMAND_LINE_TARGETS = ["upload", "mcuboot-image"]
            with patch.dict(sys.modules, {"SCons": ModuleType("SCons"), "SCons.Script": script}):
                runpy.run_path(str(ROOT / "owntech/scripts/pre_usb_artifact.py"),
                               init_globals={"env": env, "Import": lambda name: None})
            self.assertEqual(env.post, [("mcuboot-image", usb_artifact_post_action)])
            self.assertEqual(env.dependencies, [("upload", "mcuboot-image")])


if __name__ == "__main__":
    unittest.main()
