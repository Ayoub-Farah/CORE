"""Application identity changes with code/config, never deployment or mtimes."""

import copy
import json
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "owntech" / "scripts"))
from ota_build_identity import (effective_settings, fingerprint,
                               generate_for_environment, normalize_version,
                               write_identity)


class FakeBoard:
    def __init__(self, options, updates):
        self.updates = updates
        self.build = {"cpu": "cortex-m4", "zephyr": {
            "cmake_extra_args": options["board_build.zephyr.cmake_extra_args"],
            "bootloader": {"app_version": options.get(
                "board_build.zephyr.bootloader.app_version", "1.0.0")}}}

    def get(self, key, default=None):
        result = {"build": self.build}
        for component in key.split("."):
            if not isinstance(result, dict) or component not in result:
                return default
            result = result[component]
        return result

    def update(self, key, value):
        self.updates[key] = value


class FakeEnv(dict):
    def __init__(self, root, name="OTA"):
        super().__init__(PIOENV=name)
        self.root = root
        self.board_updates = {}
        self.options = {
            "platform": "ststm32@19.0.0", "framework": ["zephyr"], "board": "spin",
            "build_flags": ["-std=c++2a", "-fsingle-precision-constant"],
            "board_build.zephyr.bootloader.app_version": "1.0.0",
            "board_build.zephyr.cmake_extra_args": ["-DBUILD_ENV_NAME=" + name,
                                                    "-DOWNTECH_BUILD_PROFILE=ota"],
        }

    def subst(self, value):
        return str({"$PROJECT_DIR": self.root, "$PROJECT_SRC_DIR": self.root / "src",
                    "$PROJECT_INCLUDE_DIR": self.root / "include",
                    "$PROJECT_LIBDEPS_DIR": self.root / "owntech/lib",
                    "$PROJECT_LIB_DIR": self.root / "owntech/lib",
                    "$PROJECT_WORKSPACE_DIR": self.root / ".pio",
                    "$BUILD_DIR": self.root / ".pio/build" / self["PIOENV"]}.get(value, value))

    def GetProjectOptions(self, as_dict=False):
        return copy.deepcopy(self.options)

    def GetProjectConfig(self):
        return types.SimpleNamespace(get=lambda section, key: str(self.root / "owntech/lib"))

    def BoardConfig(self):
        return FakeBoard(self.options, self.board_updates)

    def PioPlatform(self):
        return types.SimpleNamespace(get_installed_packages=lambda: [
            types.SimpleNamespace(metadata=types.SimpleNamespace(
                name="framework-zephyr", version="3.40000.0")),
            types.SimpleNamespace(metadata=types.SimpleNamespace(
                name="toolchain-gccarmnoneeabi", version="1.120301.0")),
        ])


class IdentityTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.put("src/main.cpp", "int value = 1;\n")
        self.put("src/app.conf", "CONFIG_LED=y\n")
        self.put("include/application.h", "#define APP_VALUE 1\n")
        self.put("zephyr/CMakeLists.txt", "target_sources(app PRIVATE main.cpp)\n")
        self.put("zephyr/profiles/ota.conf", "CONFIG_OWNTECH_OTA=y\n")
        self.put("third_party/sdk/src/can.c", "int sdk = 1;\n")
        self.put("west.yml", "revision: abc123\n")
        for env in ("OTA", "USB_LEAD"):
            self.put("owntech/lib/%s/control/src/loop.cpp" % env, "int gain = 1;\n")

    def put(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(text.encode())

    def test_source_config_and_dependency_edits_change_identity(self):
        env = FakeEnv(self.root)
        before = generate_for_environment(env)["build_id"]
        for file in ("src/main.cpp", "src/app.conf", "include/application.h", "zephyr/profiles/ota.conf",
                     "third_party/sdk/src/can.c", "west.yml",
                     "owntech/lib/OTA/control/src/loop.cpp"):
            with self.subTest(file=file):
                original = (self.root / file).read_bytes()
                (self.root / file).write_bytes(original + b"/* changed without a Git commit */\n")
                self.assertNotEqual(before, generate_for_environment(env)["build_id"])
                (self.root / file).write_bytes(original)
                self.assertEqual(before, generate_for_environment(env)["build_id"])

    def test_effective_flags_board_version_and_overlay_change_identity(self):
        env = FakeEnv(self.root)
        before = generate_for_environment(env)["build_id"]
        for key, value in (("build_flags", ["-Os", "-DAPP_MODE=2"]),
                           ("build_src_flags", "-DSOURCE_MODE=2"),
                           ("board_version", "1_1_0"),
                           ("board_build.zephyr.cmake_extra_args", "-DEXTRA_CONF_FILE=other.conf")):
            original = copy.deepcopy(env.options)
            env.options[key] = value
            self.assertNotEqual(before, generate_for_environment(env)["build_id"])
            env.options = original
        env["BUILD_FLAGS"] = "-DCLI_OVERRIDE=1"
        self.assertNotEqual(before, generate_for_environment(env)["build_id"])

    def test_environments_and_deployment_settings_share_identity(self):
        ota = FakeEnv(self.root)
        lead = FakeEnv(self.root, "USB_LEAD")
        lead.options.update(upload_port="COM42", monitor_speed=9600,
                            custom_ota_expected_count=9, custom_ota_lead_serial="serial",
                            upload_protocol="custom", targets=["usb-lead-update"])
        self.put("src/app.ini", "[env:USB_LEAD]\ncustom_ota_expected_count=9\n")
        self.assertEqual(generate_for_environment(ota), generate_for_environment(lead))
        # A library source difference is significant even when names match.
        self.put("owntech/lib/USB_LEAD/control/src/loop.cpp", "int gain = 2;\n")
        self.assertNotEqual(generate_for_environment(ota)["build_id"],
                            generate_for_environment(lead)["build_id"])

    def test_ignored_outputs_git_docs_and_line_endings_do_not_change_identity(self):
        env = FakeEnv(self.root)
        before = generate_for_environment(env)
        for path in (".git/HEAD", ".pio/build/OTA/firmware.bin", "Idea/a.md",
                     "src/README.md", "third_party/sdk/tests/test.c",
                     "owntech/lib/OTA/control/.git/index"):
            self.put(path, "noise")
        self.put("src/main.cpp", "int value = 1;\r\n")
        self.assertEqual(before, generate_for_environment(env))

    def test_outputs_are_bounded_consistent_and_not_rewritten(self):
        env = FakeEnv(self.root)
        result = generate_for_environment(env)
        output = self.root / ".pio/build/OTA/ota_generated"
        header = output / "owntech_build_info.h"
        old_stat = header.stat().st_mtime_ns
        self.assertEqual(28, len(result["build_id"]))
        self.assertRegex(result["build_id"], r"^ota-[0-9a-f]{24}$")
        self.assertEqual("1.0.0+0", env["OWNTECH_OTA_VERSION"])
        self.assertEqual(result["build_id"], env["OWNTECH_OTA_BUILD_ID"])
        self.assertEqual(result, json.loads(Path(env["OWNTECH_OTA_IDENTITY_FILE"]).read_text()))
        self.assertIn('"%s"' % result["build_id"], header.read_text())
        self.assertEqual(result, generate_for_environment(env))
        self.assertEqual(old_stat, header.stat().st_mtime_ns)

    def test_mcuboot_version_canonicalization_and_bounds(self):
        self.assertEqual("1.2.3+0", normalize_version("01.02.003"))
        self.assertEqual("255.255.65535+4294967295", normalize_version("255.255.65535+4294967295"))
        for bad in ("", "1.0", "v1.0.0", "1.0.0-rc1", "-1.0.0", "256.0.0",
                    "1.256.0", "1.0.65536", "1.0.0+4294967296", "1.0.0+1+2"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                normalize_version(bad)
        env = FakeEnv(self.root)
        before = generate_for_environment(env)
        env.options["board_build.zephyr.bootloader.app_version"] = "01.00.000+000"
        self.assertEqual(before, generate_for_environment(env))
        env.options["board_build.zephyr.bootloader.app_version"] = "1.0.1"
        after = generate_for_environment(env)
        self.assertEqual("1.0.1+0", after["version"])
        self.assertNotEqual(before["build_id"], after["build_id"])

    def test_pre_script_exports_identity_on_each_invocation(self):
        env = FakeEnv(self.root)
        script = REPO / "owntech/scripts/pre_ota_identity.py"
        runpy.run_path(str(script), init_globals={"env": env, "Import": lambda _: None})
        before = env["OWNTECH_OTA_BUILD_ID"]
        self.put("src/main.cpp", "int value = 2;\n")
        runpy.run_path(str(script), init_globals={"env": env, "Import": lambda _: None})
        self.assertNotEqual(before, env["OWNTECH_OTA_BUILD_ID"])

    def test_settings_are_independent_of_checkout_path(self):
        other_root = self.root / "other"
        first = effective_settings({"build_flags": "-I%s/src" % self.root}, project_dir=self.root)
        second = effective_settings({"build_flags": "-I%s/src" % other_root}, project_dir=other_root)
        self.assertEqual(first, second)
        self.assertEqual(["-DEXTRA_CONF_FILE=\"file with spaces.conf\""],
                         effective_settings({"board_build.zephyr.cmake_extra_args": [
                             "-D", "BUILD_ENV_NAME:STRING=USB_LEAD",
                             "'-DEXTRA_CONF_FILE=\"file with spaces.conf\"'"]})[
                                 "options"]["board_build.zephyr.cmake_extra_args"])

    def test_config_changes_invalidate_cache_but_source_contents_remain_incremental(self):
        env = FakeEnv(self.root)
        files = ("zephyr/profiles/ota.conf", "zephyr/profiles/ota_usb.overlay",
                 "zephyr/modules/service/Kconfig", "zephyr/boards/spin.dts",
                 "zephyr/boards/pins.dtsi", "zephyr/modules/service/module.yml",
                 "zephyr/CMakeLists.txt", "zephyr/options.cmake",
                 "owntech/lib/OTA/control/lib.conf", "west.yml")
        for path in files:
            if not (self.root / path).exists():
                self.put(path, "original configuration\n")
        first = generate_for_environment(env)
        cache = self.root / ".pio/build/OTA/CMakeCache.txt"
        stamp = self.root / ".pio/ota-generated/OTA/configuration.sha256"
        original_stamp = stamp.read_bytes()
        cache.write_text("keep cache", encoding="utf-8")
        self.put("src/main.cpp", "int value = 2;\n")
        after = generate_for_environment(env)
        self.assertNotEqual(first["build_id"], after["build_id"])
        self.assertEqual(original_stamp, stamp.read_bytes())
        self.assertEqual("keep cache", cache.read_text())
        self.put("include/application.h", "#define APP_VALUE 2\n")
        generate_for_environment(env)
        self.assertTrue(cache.is_file())
        for path in files:
            with self.subTest(path=path):
                cache.write_text("old configuration", encoding="utf-8")
                original = (self.root / path).read_bytes()
                (self.root / path).write_bytes(original + b"# changed\n")
                generate_for_environment(env)
                self.assertFalse(cache.exists())
                # The persistent stamp prevents another configure after success.
                cache.write_text("new configuration", encoding="utf-8")
                generate_for_environment(env)
                self.assertEqual("new configuration", cache.read_text())

    def test_added_removed_sources_and_effective_flags_refresh_configuration(self):
        env = FakeEnv(self.root)
        generate_for_environment(env)
        cache = self.root / ".pio/build/OTA/CMakeCache.txt"
        cache.write_text("old")
        self.put("src/new_module.cpp", "int another = 1;\n")
        generate_for_environment(env)
        self.assertFalse(cache.exists())
        cache.write_text("new")
        (self.root / "src/new_module.cpp").unlink()
        generate_for_environment(env)
        self.assertFalse(cache.exists())
        cache.write_text("new")
        env.options["custom_ota_serial"] = "another-board"
        env.options["custom_ota_expected_count"] = 5
        generate_for_environment(env)
        self.assertTrue(cache.is_file())
        env.options["build_flags"] = ["-DAPP_MODE=2"]
        generate_for_environment(env)
        self.assertFalse(cache.exists())

    def test_durable_identity_restored_by_real_cmake_after_builder_cleanup(self):
        cmake = shutil.which("cmake")
        if not cmake:
            installed = Path.home() / ".platformio/packages/tool-cmake/bin/cmake.exe"
            cmake = str(installed) if installed.is_file() else None
        if not cmake:
            self.skipTest("CMake is needed to exercise its identity restore helper")
        env = FakeEnv(self.root / "checkout with spaces")
        first = generate_for_environment(env)
        durable = Path(env["OWNTECH_OTA_IDENTITY_DIR"])
        build = Path(env.subst("$BUILD_DIR"))
        original = (build / "ota_generated/owntech_build_info.h").read_bytes()
        args = shlex.split(env.board_updates["build.zephyr.cmake_extra_args"])
        self.assertIn("-DBUILD_ENV_NAME=OTA", args)
        self.assertIn("-DOWNTECH_BUILD_PROFILE=ota", args)
        self.assertIn("-DOWNTECH_OTA_IDENTITY_DIR=" + durable.resolve().as_posix(), args)
        # Model PlatformIO's documented full build cleanup, only in our fixture.
        self.assertIn(self.root.resolve(), build.resolve().parents)
        shutil.rmtree(build)
        self.assertTrue((durable / "configuration.sha256").is_file())
        self.assertEqual(original, (durable / "owntech_build_info.h").read_bytes())
        build.mkdir(parents=True)
        subprocess.run([cmake, "-DOWNTECH_OTA_IDENTITY_DIR=" + durable.resolve().as_posix(),
                        "-P", str(REPO / "zephyr/ota_identity.cmake")], cwd=build,
                       check=True, capture_output=True, text=True)
        self.assertEqual(original, (build / "ota_generated/owntech_build_info.h").read_bytes())
        self.assertEqual(first, json.loads((build / "ota_generated/identity.json").read_text()))
        cache = build / "CMakeCache.txt"
        cache.write_text("successful configure")
        self.assertEqual(first, generate_for_environment(env))
        self.assertTrue(cache.is_file())

    def test_pre_hook_without_file_and_injected_path_do_not_change_identity(self):
        env = FakeEnv(self.root)
        script = REPO / "owntech/scripts/pre_ota_identity.py"
        namespace = {"env": env, "Import": lambda _: None}
        exec(compile(script.read_text(), str(script), "exec"), namespace)
        self.assertNotIn("__file__", namespace)
        first = env["OWNTECH_OTA_BUILD_ID"]
        # Also test a repeated call whose BoardConfig already has the injected
        # argument: it cannot create a self-referential changing fingerprint.
        env.options["board_build.zephyr.cmake_extra_args"] = env.board_updates[
            "build.zephyr.cmake_extra_args"]
        self.assertEqual(first, generate_for_environment(env)["build_id"])
        args = shlex.split(env.board_updates["build.zephyr.cmake_extra_args"])
        self.assertEqual(1, sum(arg.startswith("-DOWNTECH_OTA_IDENTITY_DIR=") for arg in args))


if __name__ == "__main__":
    unittest.main()
