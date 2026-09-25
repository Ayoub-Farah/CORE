"""PlatformIO wizard registration and process arguments, without board access."""
import configparser
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/scripts"))
from ota_gui_tasks import GUI_ONLY_TARGETS, gui_python, run_workflow
from test_pc_build import Environment as BuildEnvironment


class Environment(BuildEnvironment):
    def __init__(self, project, name):
        super().__init__(project / ".pio/build" / name, project)
        self.name = name
        self.signature_files = []

    def subst(self, text):
        return super().subst(text.replace("$PIOENV", self.name))

    def GetProjectOption(self, key, default=None):
        return {"custom_ota_mcumgr": "C:/Tool folder/mcumgr.exe",
                "custom_ota_serial": "unused-config-selection",
                "custom_ota_expected_count": "99"}.get(key, default)

    def SConsignFile(self, path):
        self.signature_files.append(path)


def run_script(filename, env, targets):
    script = ModuleType("SCons.Script")
    script.COMMAND_LINE_TARGETS = list(targets)
    with patch.dict(sys.modules, {"SCons": ModuleType("SCons"), "SCons.Script": script}):
        runpy.run_path(str(ROOT / "owntech/scripts" / filename),
                       init_globals={"env": env, "Import": lambda _: None})
    return script.COMMAND_LINE_TARGETS


class PlatformIOGuiTests(unittest.TestCase):
    def test_pio_config_adds_registration_once_to_each_application_environment(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "owntech/pio_extra.ini", encoding="utf-8")
        for environment in ("USB", "OTA", "USB_LEAD"):
            scripts = config["env:" + environment]["extra_scripts"].splitlines()
            self.assertEqual(scripts.count("pre:owntech/scripts/pre_ota_gui.py"), 1)
        self.assertNotIn("pre_ota_gui", config["env:OTA_RECOVERY"]["extra_scripts"])

    def test_registration_has_exact_ui_titles_and_no_build_or_device_side_effects(self):
        base = {"ota_to_usb": "Switch to USB", "ota_board_status": "Check connected board"}
        with tempfile.TemporaryDirectory() as directory:
            for name in ("USB", "OTA", "USB_LEAD"):
                with self.subTest(environment=name):
                    env = Environment(Path(directory), name)
                    expected = dict(base)
                    if name != "USB":
                        expected["ota_to_ota"] = "Return to OTA V2"
                    if name == "USB_LEAD":
                        expected["ota_reconcile"] = "Finish previous CAN update"
                    with patch("ota_gui_tasks.subprocess.run") as process, patch("ota_gui_tasks.run_workflow") as wizard:
                        unchanged = run_script("pre_ota_gui.py", env, ["__idedata"])
                    self.assertEqual(unchanged, ["__idedata"])
                    process.assert_not_called()
                    wizard.assert_not_called()
                    self.assertEqual({key: value["title"] for key, value in env.tasks.items()}, expected)
                    for task in env.tasks.values():
                        self.assertEqual(task["dependencies"], [])
                        self.assertTrue(task["always_build"])
                    self.assertEqual(env.post, [])
                    self.assertEqual(env.dependencies, [])

    def test_each_task_dispatches_its_fixed_action_and_propagates_failure(self):
        actions = {"ota_to_usb": "to-usb", "ota_board_status": "status",
                   "ota_to_ota": "to-ota", "ota_reconcile": "reconcile"}
        with tempfile.TemporaryDirectory() as directory:
            env = Environment(Path(directory), "USB_LEAD")
            run_script("pre_ota_gui.py", env, ["__idedata"])
            for target, action in actions.items():
                with self.subTest(target=target), patch("ota_gui_tasks.run_workflow", return_value=7) as wizard:
                    self.assertEqual(env.tasks[target]["actions"][0]([], [], env), 7)
                    wizard.assert_called_once_with(env, action)

    def test_argument_vector_preserves_spaces_and_does_not_inherit_manual_board_selection(self):
        with tempfile.TemporaryDirectory(prefix="owntech gui ") as directory:
            env = Environment(Path(directory), "OTA")
            with patch("ota_pio.platform.system", return_value="Windows"), \
                    patch("ota_gui_tasks.gui_python", return_value=sys.executable), \
                    patch("ota_gui_tasks.subprocess.run", return_value=SimpleNamespace(returncode=23)) as process:
                self.assertEqual(run_workflow(env, "to-ota"), 23)
            process.assert_called_once_with(
                [sys.executable, str(Path(directory) / "owntech/tools/ota_workflow.py"), "to-ota",
                 "--project", directory, "--environment", "OTA", "--mcumgr", "C:/Tool folder/mcumgr.exe",
                 "--pio-python", sys.executable], cwd=directory, check=False,
                **({"creationflags": 0x08000000} if os.name == "nt" else {}))
            self.assertNotIn("--serial", process.call_args.args[0])
            self.assertNotIn("--expected-count", process.call_args.args[0])

    def test_process_start_error_fails_the_platformio_action(self):
        env = Environment(ROOT, "USB")
        with patch("ota_pio.platform.system", return_value="Windows"), \
                patch("ota_gui_tasks.gui_python", return_value=sys.executable), \
                patch("ota_gui_tasks.subprocess.run", side_effect=OSError("start failed")), redirect_stdout(io.StringIO()) as output:
            self.assertEqual(run_workflow(env, "status"), 1)
        self.assertIn("start failed", output.getvalue())

    def test_gui_python_probes_dependencies_without_creating_a_tk_root(self):
        with patch("ota_gui_tasks.shutil.which", return_value=None), \
                patch("ota_gui_tasks.subprocess.run", return_value=SimpleNamespace(returncode=0, stdout=sys.executable + "\n")) as process:
            self.assertEqual(gui_python(), sys.executable)
        self.assertEqual(process.call_count, 1)
        command = process.call_args.args[0]
        self.assertEqual(command[:2], [sys.executable, "-c"])
        self.assertIn("import tkinter, serial", command[2])
        self.assertNotIn("Tk()", command[2])
        self.assertEqual(process.call_args.kwargs["timeout"], 10)
        self.assertTrue(process.call_args.kwargs["capture_output"])
        if os.name == "nt":
            self.assertEqual(process.call_args.kwargs["creationflags"], 0x08000000)

    def test_gui_python_falls_back_to_installed_launcher_or_path_interpreter(self):
        launcher = "C:/Installed Python/py.exe" if os.name == "nt" else "/installed/python3"
        lookup = {"py" if os.name == "nt" else "python3": launcher}
        failed = SimpleNamespace(returncode=1, stdout="", stderr="No module named tkinter")
        success = SimpleNamespace(returncode=0, stdout=sys.executable + "\n")
        def result(command, **kwargs):
            return success if command[0] == launcher else failed
        with patch("ota_gui_tasks.shutil.which", side_effect=lambda name: lookup.get(name)), \
                patch("ota_gui_tasks.subprocess.run", side_effect=result) as process:
            self.assertEqual(gui_python(), sys.executable)
        expected = [launcher, "-3", "-c"] if os.name == "nt" else [launcher, "-c"]
        self.assertEqual(process.call_args.args[0][:-1], expected)
        self.assertEqual(process.call_args_list[0].args[0][0], sys.executable)

    def test_missing_gui_dependencies_fail_before_workflow_or_board_selection(self):
        env = Environment(ROOT, "USB")
        with patch("ota_gui_tasks.gui_python", return_value=None), \
                patch("ota_gui_tasks.subprocess.run") as process, redirect_stdout(io.StringIO()) as output:
            self.assertEqual(run_workflow(env, "status"), 1)
        process.assert_not_called()
        self.assertIn("Tcl/Tk", output.getvalue())
        self.assertIn("pyserial", output.getvalue())
        self.assertIn("No board action was sent", output.getvalue())

    def test_gui_probe_errors_invalid_output_and_duplicate_candidates_are_bounded(self):
        for result in (SimpleNamespace(returncode=0, stdout="not-an-absolute-executable"),
                       SimpleNamespace(returncode=0, stdout=sys.executable + "\nextra output"),
                       OSError("unavailable")):
            with self.subTest(result=result), patch("ota_gui_tasks.shutil.which", return_value=sys.executable), \
                    patch("ota_gui_tasks.sys._base_executable", sys.executable), \
                    patch("ota_gui_tasks.subprocess.run", side_effect=result if isinstance(result, Exception) else None,
                          return_value=result) as process:
                self.assertIsNone(gui_python())
            # The launcher prefix differs from direct Python even if a test
            # maps both to one file. Repeated direct-path probes are skipped.
            self.assertEqual(process.call_count, 2 if os.name == "nt" else 1)

    def test_readonly_and_transition_tasks_do_not_force_current_firmware_build(self):
        env = Environment(ROOT, "USB")
        for target in GUI_ONLY_TARGETS:
            with self.subTest(target=target):
                self.assertEqual(run_script("pre_bootloader_common.py", env, [target]), [target])
        for targets in ([], ["upload"], ["ota_board_status", "upload"]):
            with self.subTest(targets=targets):
                self.assertEqual(run_script("pre_bootloader_common.py", env, targets), ["mcuboot-image"] + targets)
        self.assertEqual(run_script("pre_bootloader_common.py", env, ["mcuboot-image"]), ["mcuboot-image"])

    def test_gui_outer_signature_database_is_separate_from_nested_build(self):
        for target in GUI_ONLY_TARGETS:
            env = Environment(ROOT, "USB_LEAD")
            run_script("pre_ota_gui.py", env, [target])
            self.assertEqual(env.signature_files, [str(env.build / (".sconsign-gui%d%d" % sys.version_info[:2]))])
        for targets in ([], ["__idedata"], ["mcuboot-image"], ["ota_init"], ["ota_to_usb", "mcuboot-image"]):
            env = Environment(ROOT, "OTA")
            run_script("pre_ota_gui.py", env, targets)
            self.assertEqual(env.signature_files, [])

    def test_real_scons_nested_build_keeps_its_incremental_signature_database(self):
        pio_home = Path(os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio")))
        scons = pio_home / "packages/tool-scons/scons.py"
        if not scons.is_file():
            self.skipTest("PlatformIO's SCons package is not installed")
        construction = '''from pathlib import Path
import runpy
import subprocess
import sys
from SCons.Script import DefaultEnvironment, AlwaysBuild, Action
project = Path.cwd()
sys.path.insert(0, str(Path(ROOT) / "owntech/scripts"))
env = DefaultEnvironment(tools=[], PROJECT_DIR=str(project), BUILD_DIR=str(project / "build"), PIOENV="OTA")
env.SConsignFile(str(project / "build/.sconsign-main"))
env.AddMethod(lambda self, action, message: Action(action, message), "VerboseAction")
def add_custom(self, **kwargs):
    return AlwaysBuild(self.Alias(kwargs["name"], kwargs["dependencies"], kwargs["actions"]))
env.AddMethod(add_custom, "AddCustomTarget")
import ota_gui_tasks
def nested(env, action):
    assert action == "to-ota"
    return subprocess.run([sys.executable, SCONS, "-Q", "mcuboot-image"], cwd=project).returncode
ota_gui_tasks.run_workflow = nested
runpy.run_path(str(Path(ROOT) / "owntech/scripts/pre_ota_gui.py"), init_globals={"env": env, "Import": lambda _: None})
def compile(source, target, env):
    Path(str(target[0])).write_text(Path(str(source[0])).read_text())
    with (project / "compilations.txt").open("a") as stream:
        stream.write("compiled\\n")
image = env.Command("build/image.bin", "source.txt", compile)
env.Alias("mcuboot-image", image)
# Load the outer process's signature database before its child runs. Without
# isolation its final sync can replace the child's freshly saved signatures.
env.File("build/image.bin").get_stored_info()
'''
        with tempfile.TemporaryDirectory(prefix="ota-scons-gui-") as directory:
            project = Path(directory)
            (project / "build").mkdir()
            (project / "source.txt").write_text("test source")
            settings = "ROOT = %r\nSCONS = %r\n" % (str(ROOT), str(scons))
            (project / "SConstruct").write_text(settings + construction, encoding="utf-8")
            for target in ("ota_to_ota", "mcuboot-image"):
                result = subprocess.run([sys.executable, str(scons), "-Q", target], cwd=project,
                                        text=True, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((project / "compilations.txt").read_text(), "compiled\n")
            databases = [path.name for path in (project / "build").glob(".sconsign*")]
            self.assertTrue(any(name.startswith(".sconsign-main") for name in databases))
            self.assertTrue(any(name.startswith(".sconsign-gui") for name in databases))

    def test_utility_metadata_discovery_never_needs_or_fabricates_guard_config(self):
        class MetadataEnvironment(Environment):
            def IsIntegrationDump(self):
                return True

            def AddPlatformTarget(self, **kwargs):
                self.tasks[kwargs["name"]] = kwargs

            def DumpIntegrationData(self, globalenv):
                return {"env_name": self.name, "includes": {"build": []},
                        "targets": [{key: item[key] for key in ("name", "title", "description")}
                                    for item in self.tasks.values()]}

            def Exit(self, status):
                raise SystemExit(status)

        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            for name in ("OTA_RECOVERY", "OTA_TRANSITION"):
                env = MetadataEnvironment(project, name)
                env.build.mkdir(parents=True)
                cache = env.build / "CMakeCache.txt"
                cache.write_text("preserve")
                with self.subTest(environment=name), redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as exited:
                    run_script("pre_ota_recovery.py", env, ["__idedata"])
                self.assertEqual(exited.exception.code, 0)
                self.assertEqual(set(env.tasks), {"mcuboot-image"})
                self.assertEqual(env.tasks["mcuboot-image"]["dependencies"], [])
                self.assertTrue((env.build / "idedata.json").is_file())
                self.assertEqual(cache.read_text(), "preserve")
                self.assertFalse((project / ".pio/ota-recovery-config").exists())
                self.assertFalse((project / ".pio/ota-transition-config").exists())
                self.assertEqual(env.post, [])
                # The same fresh project cannot build a utility unguarded.
                plain = Environment(project, name)
                with self.assertRaisesRegex(ValueError, "scoped configuration"):
                    run_script("pre_ota_recovery.py", plain, ["mcuboot-image"])

    def test_unsupported_environments_do_not_register_actions(self):
        env = Environment(ROOT, "OTA_RECOVERY")
        with self.assertRaises(ValueError):
            run_script("pre_ota_gui.py", env, [])
        self.assertEqual(env.tasks, {})


if __name__ == "__main__":
    unittest.main()
