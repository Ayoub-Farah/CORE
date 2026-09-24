"""Post-sign inspection runs on ordinary builds before any upload action."""
from contextlib import redirect_stdout
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
sys.path.insert(0, str(ROOT / "owntech/tools"))
from lead_update import CampaignError, prepare_manifest


class Environment:
    def __init__(self, build):
        self.build = build
        self.post = []

    def subst(self, value):
        return value.replace("$PROJECT_DIR", str(ROOT)).replace("$BUILD_DIR", str(self.build)).replace("${PROGNAME}", "firmware")

    def GetProjectOption(self, key, default=None):
        return {"custom_ota_build_id": "build-test"}.get(key, default)

    def BoardConfig(self):
        return {"build.zephyr.bootloader.app_version": "1.2.3+4"}

    def PioPlatform(self):
        return SimpleNamespace(get_package_dir=lambda package: str(self.build / "framework"))

    def AddPostAction(self, target, action):
        self.post.append((target, action))

    def AddCustomTarget(self, **kwargs):
        self.task = kwargs

    def VerboseAction(self, action, message):
        return action


class BuildTests(unittest.TestCase):
    def test_standalone_reuses_validated_build_id_without_overwriting_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "firmware.mcuboot.bin"
            image.write_bytes(artifact())
            _, created = prepare_manifest(image, build_id="compiled-build-B")
            manifest_path = image.with_suffix(".json")
            original = manifest_path.read_bytes()
            _, reused = prepare_manifest(image)
            self.assertEqual(reused["build_id"], "compiled-build-B")
            self.assertEqual(manifest_path.read_bytes(), original)
            with self.assertRaisesRegex(CampaignError, "contradicts build_id"):
                prepare_manifest(image, build_id="wrong-build-A")
            self.assertEqual(manifest_path.read_bytes(), original)
            image.write_bytes(artifact(body_size=129))
            with self.assertRaisesRegex(CampaignError, "contradicts"):
                prepare_manifest(image)
            self.assertEqual(manifest_path.read_bytes(), original)

    def test_ordinary_sign_emits_manifest_and_rejects_corrupt_padded_image(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            env = Environment(build)
            script = ModuleType("SCons.Script")
            script.COMMAND_LINE_TARGETS = ["mcuboot-image"]
            with patch.dict(sys.modules, {"SCons": ModuleType("SCons"), "SCons.Script": script}):
                runpy.run_path(str(ROOT / "owntech/scripts/pre_target_usb_lead.py"),
                               init_globals={"env": env, "Import": lambda name: None})
            self.assertEqual(len(env.post), 1)
            target, post = env.post[0]
            self.assertEqual(target, "$BUILD_DIR/${PROGNAME}.mcuboot.bin")
            image = build / "firmware.mcuboot.bin"
            image.write_bytes(artifact())
            with redirect_stdout(io.StringIO()):
                self.assertEqual(post([], [image], env), 0)
            manifest = json.loads(image.with_suffix(".json").read_text())
            self.assertEqual(manifest["build_id"], "build-test")
            self.assertEqual(manifest["version"], "1.2.3+4")
            self.assertEqual(manifest["artifact_size"], 227328)
            self.assertLess(manifest["useful_size"], 221184)
            self.assertTrue(manifest["signature"]["signing_key"].endswith("root-rsa-2048.pem"))
            data = bytearray(image.read_bytes())
            data[200000] = 0
            image.write_bytes(data)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(post([], [image], env), 1)


if __name__ == "__main__":
    unittest.main()
