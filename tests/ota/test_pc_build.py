"""Post-sign inspection runs on ordinary builds before any upload action."""
from contextlib import redirect_stdout
import io
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

from test_pc_artifact import artifact
from ota_artifact import inspect_image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
sys.path.insert(0, str(ROOT / "owntech/scripts"))
from lead_update import CampaignError, prepare_manifest, main


class Environment:
    def __init__(self, build, project=None):
        self.build = build
        self.project = project or build.parent
        self.post = []
        self.tasks = {}
        self.aliases = {}
        self.dependencies = []

    def subst(self, value):
        return value.replace("$PROJECT_DIR", str(self.project)).replace("$BUILD_DIR", str(self.build)).replace("${PROGNAME}", "firmware").replace("$PIOENV", "USB_LEAD")

    def GetProjectOption(self, key, default=None):
        return {}.get(key, default)

    def get(self, key, default=None):
        return {"OWNTECH_OTA_VERSION": "1.2.3+4", "OWNTECH_OTA_BUILD_ID": "build-test"}.get(key, default)

    def BoardConfig(self):
        return {"build.zephyr.bootloader.app_version": "1.2.3+4"}

    def PioPlatform(self):
        return SimpleNamespace(get_package_dir=lambda package: str(self.build / "framework"))

    def AddPostAction(self, target, action):
        self.post.append((target, action))

    def Alias(self, name):
        if name not in self.aliases:
            self.aliases[name] = SimpleNamespace(name=name)
        return [self.aliases[name]]

    def Depends(self, target, dependency):
        self.dependencies.append((target, dependency))

    def AddCustomTarget(self, **kwargs):
        self.task = kwargs
        self.tasks[kwargs["name"]] = kwargs

    def VerboseAction(self, action, message):
        return action

    def Replace(self, **values):
        self.replacements = values


class BuildTests(unittest.TestCase):
    def test_default_journal_survives_removal_of_build_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            build = project / ".pio" / "build" / "USB_LEAD"
            build.mkdir(parents=True)
            image = build / "firmware.mcuboot.bin"
            image.write_bytes(artifact(compact=True))
            image.with_suffix(".json").write_text(json.dumps(inspect_image(image.read_bytes(), build_id="B")))
            connection = SimpleNamespace(connect=lambda: object(), transport=None, serial_number="test-usb",
                                         last_info={"service": "owntech-ota", "protocol": 2}, reconnect=lambda: None)
            original_cwd = Path.cwd()
            try:
                os.chdir(project)
                with patch("lead_update.USBConnection", return_value=connection), \
                     patch("lead_update.Campaign") as campaign, \
                     patch("lead_update.secrets.randbits", return_value=42), redirect_stdout(io.StringIO()):
                    campaign.return_value.run.return_value = "SUCCESS"
                    self.assertEqual(main(["--image", str(image), "--expected-count", "3", "--build-id", "B"]), 0)
                journal = project / "ota-journals" / "campaign-000000000000002a.jsonl"
                self.assertTrue(journal.is_file())
                contents = journal.read_bytes()
                # Remove only this test's known generated build files and then
                # the empty build directory, modelling the PlatformIO clean.
                image.unlink()
                image.with_suffix(".json").unlink()
                build.rmdir()
                self.assertEqual(journal.read_bytes(), contents)
                self.assertIn(b"test-usb", contents)
            finally:
                os.chdir(original_cwd)

    def test_standalone_reuses_validated_build_id_without_overwriting_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "firmware.mcuboot.bin"
            image.write_bytes(artifact(compact=True))
            _, created = prepare_manifest(image, build_id="compiled-build-B", image_class="receiver")
            manifest_path = image.with_suffix(".json")
            original = manifest_path.read_bytes()
            _, reused = prepare_manifest(image)
            self.assertEqual(reused["build_id"], "compiled-build-B")
            self.assertEqual(manifest_path.read_bytes(), original)
            with self.assertRaisesRegex(CampaignError, "contradicts build_id"):
                prepare_manifest(image, build_id="wrong-build-A")
            self.assertEqual(manifest_path.read_bytes(), original)
            image.write_bytes(artifact(body_size=129, compact=True))
            with self.assertRaisesRegex(CampaignError, "contradicts"):
                prepare_manifest(image)
            self.assertEqual(manifest_path.read_bytes(), original)

    def test_ordinary_sign_emits_manifest_and_rejects_corrupt_padded_image(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            build = project / ".pio" / "build" / "USB_LEAD"
            build.mkdir(parents=True)
            env = Environment(build, project)
            script = ModuleType("SCons.Script")
            script.COMMAND_LINE_TARGETS = ["mcuboot-image"]
            with patch.dict(sys.modules, {"SCons": ModuleType("SCons"), "SCons.Script": script}):
                runpy.run_path(str(ROOT / "owntech/scripts/pre_target_usb_lead.py"),
                               init_globals={"env": env, "Import": lambda name: None})
            self.assertEqual(len(env.post), 1)
            target, post = env.post[0]
            self.assertEqual(target, env.Alias("mcuboot-image"))
            image = build / "firmware.mcuboot.bin"
            image.write_bytes(artifact())
            (build / "CMakeCache.txt").write_text("PYTHON_EXECUTABLE:FILEPATH=" + sys.executable + "\n")
            def sign_compact(args, **kwargs):
                self.assertNotIn("--pad", args)
                self.assertTrue(args[-2].endswith("firmware.bin"))
                Path(args[-1]).write_bytes(artifact(compact=True))
            with redirect_stdout(io.StringIO()), patch("ota_pio.subprocess.run", side_effect=sign_compact):
                self.assertEqual(post([], [image], env), 0)
            manifest = json.loads(image.with_suffix(".json").read_text())
            self.assertEqual(manifest["build_id"], "build-test")
            self.assertEqual(manifest["version"], "1.2.3+4")
            self.assertEqual(manifest["artifact_size"], 227328)
            self.assertLess(manifest["useful_size"], 221184)
            self.assertTrue(manifest["signature"]["signing_key"].endswith("root-rsa-2048.pem"))
            snapshot = env.project / "ota-artifacts" / "USB_LEAD" / image.name
            self.assertNotIn(build, snapshot.parents)
            self.assertEqual(snapshot.read_bytes(), image.read_bytes())
            self.assertEqual(snapshot.with_suffix(".json").read_bytes(), image.with_suffix(".json").read_bytes())
            data = bytearray(image.read_bytes())
            data[200000] = 0
            image.write_bytes(data)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(post([], [image], env), 1)
            self.assertEqual(snapshot.read_bytes(), artifact())

    def test_provision_hook_uses_generated_identity_and_only_custom_application_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            env = Environment(build)
            env.GetProjectOption = lambda key, default=None: {
                "board_id": "selected-serial", "upload_port": "COM17", "custom_ota_timeout": "42",
                "custom_ota_build_id": "stale-manual-id", "custom_ota_mcumgr": "existing-mcumgr",
            }.get(key, default)
            script = ModuleType("SCons.Script")
            script.COMMAND_LINE_TARGETS = ["upload"]
            with patch.dict(sys.modules, {"SCons": ModuleType("SCons"), "SCons.Script": script}):
                runpy.run_path(str(ROOT / "owntech/scripts/pre_target_ota.py"),
                               init_globals={"env": env, "Import": lambda name: None})
            self.assertEqual(script.COMMAND_LINE_TARGETS, ["mcuboot-image", "upload"])
            self.assertEqual(env.dependencies, [(env.Alias("upload"), env.Alias("mcuboot-image"))])
            self.assertEqual(len(env.post), 1)
            self.assertEqual(set(env.replacements), {"UPLOADCMD"})
            with patch("provision_ota.main", return_value=0) as provision:
                self.assertEqual(env.replacements["UPLOADCMD"]([], [], env), 0)
            args = provision.call_args.args[0]
            self.assertEqual(args[args.index("--build-id") + 1], "build-test")
            self.assertEqual(args[args.index("--version") + 1], "1.2.3+4")
            self.assertEqual(args[args.index("--serial") + 1], "selected-serial")
            self.assertEqual(args[args.index("--port") + 1], "COM17")
            self.assertEqual(args[args.index("--mcumgr") + 1], "existing-mcumgr")
            self.assertEqual(args[args.index("--timeout") + 1], "42")
            self.assertNotIn("--receiver-absent", args)
            self.assertNotIn("--legacy-console", args)
            self.assertEqual(set(env.tasks), {"ota_init"})

    def test_explicit_usb_init_target_builds_signed_image_for_both_ota_environments(self):
        with tempfile.TemporaryDirectory() as directory:
            for filename in ("pre_target_ota.py", "pre_target_usb_lead.py"):
                with self.subTest(filename=filename):
                    env = Environment(Path(directory) / filename)
                    env.GetProjectOption = lambda key, default=None: {
                        "custom_ota_serial": "selected-serial", "custom_ota_port": "COM17",
                        "custom_ota_mcumgr": "existing-mcumgr", "custom_ota_timeout": "42",
                        "custom_ota_expected_count": "3",
                    }.get(key, default)
                    script = ModuleType("SCons.Script")
                    script.COMMAND_LINE_TARGETS = ["ota_init"]
                    with patch.dict(sys.modules, {"SCons": ModuleType("SCons"), "SCons.Script": script}):
                        runpy.run_path(str(ROOT / "owntech/scripts" / filename),
                                       init_globals={"env": env, "Import": lambda name: None})
                    self.assertEqual(script.COMMAND_LINE_TARGETS, ["mcuboot-image", "ota_init"])
                    task = env.tasks["ota_init"]
                    self.assertEqual(task["title"], "Initialize board over USB")
                    self.assertEqual(task["dependencies"], env.Alias("mcuboot-image"))
                    self.assertTrue(task["always_build"])
                    self.assertEqual(len(env.post), 1)
                    with patch("provision_ota.main", return_value=0) as provision, patch("lead_update.main") as campaign:
                        self.assertEqual(task["actions"][0]([], [], env), 0)
                    campaign.assert_not_called()
                    args = provision.call_args.args[0]
                    self.assertIn("--legacy-console", args)
                    for flag, value in (("--serial", "selected-serial"), ("--port", "COM17"),
                                        ("--mcumgr", "existing-mcumgr"), ("--timeout", "42"),
                                        ("--version", "1.2.3+4"), ("--build-id", "build-test"),
                                        ("--image", str(env.build) + "/firmware.mcuboot.bin")):
                        self.assertEqual(args[args.index(flag) + 1], value)
                    self.assertNotIn("--expected-count", args)
                    self.assertNotIn("--expected-id", args)
                    self.assertNotIn("--receiver-absent", args)

    def test_real_scons_resolves_final_image_and_validates_before_usb_actions(self):
        # Exercise the actual Alias/AddPostAction semantics without invoking
        # PlatformIO, a compiler or USB. PIO starts pre-scripts with "program";
        # its platform builder subsequently chooses the final PROGNAME.
        pio_home = Path(os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio")))
        scons = pio_home / "packages" / "tool-scons" / "scons.py"
        if not scons.is_file():
            self.skipTest("PlatformIO's SCons package is not installed")
        for filename, action_name, corrupt in (
                ("pre_target_ota.py", "ota_init", False),
                ("pre_target_usb_lead.py", "ota_init", False),
                ("pre_target_ota.py", "upload", False),
                ("pre_target_ota.py", "ota_init", True),
                ("pre_target_ota.py", "upload", True)):
            with self.subTest(hook=filename, action=action_name, corrupt=corrupt), tempfile.TemporaryDirectory() as directory:
                project = Path(directory)
                payload = bytearray(artifact())
                if corrupt:
                    payload[200000] = 0
                (project / "input.bin").write_bytes(payload)
                construction = '''from pathlib import Path
import json
import runpy
import sys
from types import SimpleNamespace
from SCons.Script import DefaultEnvironment, AlwaysBuild, Action, COMMAND_LINE_TARGETS
root = Path(ROOT)
sys.path.insert(0, str(root / "owntech/scripts"))
sys.path.insert(0, str(root / "owntech/tools"))
project = Path.cwd()
env = DefaultEnvironment(tools=[], PROJECT_DIR=str(project), BUILD_DIR=str(project / "build"),
                  PIOENV="TEST_USB", PROGNAME="program", OWNTECH_OTA_VERSION="1.2.3+4",
                  OWNTECH_OTA_BUILD_ID="build-test")
env.AddMethod(lambda self, key, default=None: default, "GetProjectOption")
env.AddMethod(lambda self: {}, "BoardConfig")
env.AddMethod(lambda self: SimpleNamespace(get_package_dir=lambda package: str(project)), "PioPlatform")
env.AddMethod(lambda self, action, message: Action(action, message), "VerboseAction")
def add_custom(self, **kwargs):
    result = self.Alias(kwargs["name"], kwargs["dependencies"], kwargs["actions"])
    if kwargs.get("always_build"):
        AlwaysBuild(result)
    return result
env.AddMethod(add_custom, "AddCustomTarget")
def usb_stub(args):
    image = Path(args[args.index("--image") + 1])
    assert image.name == "release-payload.mcuboot.bin", image
    assert image.read_bytes() == (project / "input.bin").read_bytes()
    manifest = json.loads(image.with_suffix(".json").read_text())
    assert manifest["build_id"] == "build-test"
    assert (project / "ota-artifacts/TEST_USB" / image.name).read_bytes() == image.read_bytes()
    with (project / "order.txt").open("a") as output:
        output.write("verified USB action\\n")
    return 0
import provision_ota
import lead_update
provision_ota.main = usb_stub
lead_update.main = usb_stub
runpy.run_path(str(root / "owntech/scripts" / HOOK), init_globals={"env": env, "Import": lambda name: None})
assert "mcuboot-image" in COMMAND_LINE_TARGETS
env.Replace(PROGNAME="release-payload")
def sign(source, target, env):
    image = Path(str(target[0]))
    image.parent.mkdir(parents=True, exist_ok=True)
    image.write_bytes(Path(str(source[0])).read_bytes())
    (project / "order.txt").write_text("signed\\n")
signed = env.Command("$BUILD_DIR/${PROGNAME}.mcuboot.bin", "input.bin", sign)
AlwaysBuild(env.Alias("mcuboot-image", signed))
if REQUESTED == "upload":
    AlwaysBuild(env.Alias("upload", signed, env["UPLOADCMD"]))
'''
                # Literal Python values, never shell interpolation.
                settings = "ROOT = %r\nHOOK = %r\nREQUESTED = %r\n" % (str(ROOT), filename, action_name)
                (project / "SConstruct").write_text(settings + construction, encoding="utf-8")
                result = subprocess.run([sys.executable, str(scons), "-Q", "-j", "2", action_name],
                                        cwd=project, text=True, capture_output=True, timeout=30)
                diagnostic = result.stdout + result.stderr
                if corrupt:
                    self.assertNotEqual(result.returncode, 0, diagnostic)
                    self.assertIn("OTA artifact rejected", diagnostic)
                    self.assertEqual((project / "order.txt").read_text(), "signed\n")
                else:
                    self.assertEqual(result.returncode, 0, diagnostic)
                    self.assertIn("OTA artifact checked", diagnostic)
                    self.assertEqual((project / "order.txt").read_text(), "signed\nverified USB action\n")
                self.assertFalse((project / "build/program.mcuboot.bin").exists())

    def test_fleet_task_builds_receiver_and_never_invokes_lead_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            env = Environment(Path(directory) / "build", ROOT)
            script = ModuleType("SCons.Script")
            script.COMMAND_LINE_TARGETS = ["lead_update"]
            with patch.dict(sys.modules, {"SCons": ModuleType("SCons"), "SCons.Script": script}):
                runpy.run_path(str(ROOT / "owntech/scripts/pre_target_usb_lead.py"),
                               init_globals={"env": env, "Import": lambda _: None})
            task = env.tasks["lead_update"]
            self.assertEqual(task["dependencies"], [])
            with patch("subprocess.run") as build, patch("lead_update.main", return_value=0) as campaign:
                self.assertEqual(task["actions"][0]([], [], env), 0)
            args = build.call_args.args[0]
            self.assertEqual(args[-4:], ["-e", "OTA", "-t", "mcuboot-image"])
            self.assertNotIn("upload", args)
            self.assertEqual(campaign.call_args.args[0][:2],
                             ["--image", str(ROOT / "ota-artifacts/OTA/firmware.can.bin")])

    def test_ota_identity_hook_is_required_instead_of_silent_manual_fallback(self):
        from ota_pio import artifact_options
        with tempfile.TemporaryDirectory() as directory:
            env = Environment(Path(directory))
            env.get = lambda key, default=None: default
            with self.assertRaisesRegex(ValueError, "pre_ota_identity"):
                artifact_options(env)

    def test_recovery_checks_entry_before_artifact_on_shared_signing_alias(self):
        class RecoveryEnvironment(Environment):
            def __setitem__(self, key, value):
                self.values[key] = value

        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            config = project / ".pio/ota-recovery-config"
            config.mkdir(parents=True)
            header = b"/* test recovery guard */\n"
            (config / "owntech_ota_recovery_config.h").write_bytes(header)
            (config / "owntech_ota_recovery_config.json").write_text(json.dumps({
                "header_sha256": hashlib.sha256(header).hexdigest()}))
            env = RecoveryEnvironment(project / "build", project)
            env.values = {}
            board = SimpleNamespace(get=lambda key, default=None: default, update=lambda key, value: None)
            env.BoardConfig = lambda: board
            runpy.run_path(str(ROOT / "owntech/scripts/pre_ota_recovery.py"),
                           init_globals={"env": env, "Import": lambda name: None})
            self.assertEqual([node for node, _ in env.post], [env.Alias("mcuboot-image")] * 2)
            self.assertEqual([action.__name__ for _, action in env.post],
                             ["verify_recovery_entry", "artifact_post_action"])
            verify = env.post[0][1]
            with patch("subprocess.run", return_value=SimpleNamespace(stdout="ota_recovery_run(config)")) as nm:
                self.assertEqual(verify([], [], env), 0)
            self.assertEqual(nm.call_args.args[0][-1], str(env.build) + "/firmware.elf")
            with patch("subprocess.run", return_value=SimpleNamespace(stdout="setup_routine()")):
                with self.assertRaisesRegex(ValueError, "wrong entry point"):
                    verify([], [], env)
            with redirect_stdout(io.StringIO()), patch("subprocess.run") as process:
                self.assertEqual(env.replacements["UPLOADCMD"]([], [], env), 1)
            process.assert_not_called()

    def test_scons_hooks_execute_without_dunder_file(self):
        # Real SConscript deliberately removes __file__ before exec(), unlike
        # runpy. Both hooks must resolve imports through the project env.
        with tempfile.TemporaryDirectory() as directory:
            for filename in ("pre_target_ota.py", "pre_target_usb_lead.py"):
                env = Environment(Path(directory))
                script = ModuleType("SCons.Script")
                script.COMMAND_LINE_TARGETS = []
                namespace = {"env": env, "Import": lambda name: None}
                with patch.dict(sys.modules, {"SCons": ModuleType("SCons"), "SCons.Script": script}):
                    exec(compile((ROOT / "owntech/scripts" / filename).read_text(), filename, "exec"), namespace)
                self.assertNotIn("__file__", namespace)
                self.assertEqual(len(env.post), 1)


if __name__ == "__main__":
    unittest.main()
