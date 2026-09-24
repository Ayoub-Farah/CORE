"""Compile and execute the actual portable OTA core and storage adapter.

On Windows LLVM builds a freestanding test DLL loaded by ctypes, so neither
MSVC nor an ARM device is required. Linux uses the normal C++ runtime.
"""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OTA = ROOT / "zephyr/modules/owntech_ota/zephyr"


class CoreTests(unittest.TestCase):
    def compile_and_run(self, storage=False, short_enums=False):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "C++ compiler required (LLVM clang++ on Windows)")
        name = "storage" if storage else "core"
        symbol = "ota_storage_test_run" if storage else "ota_test_run"
        with tempfile.TemporaryDirectory(prefix="owntech-ota-") as tmp:
            output = Path(tmp) / (name + (".dll" if os.name == "nt" else ""))
            args = [compiler, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror"]
            if short_enums:
                args += ["-fshort-enums"]
            includes = [OTA / "public_api", OTA / "src"]
            sources = [OTA / "src/ota_protocol.c"]
            if storage:
                includes.insert(0, ROOT / "tests/ota/storage_shim")
                includes += [ROOT / "zephyr/modules/owntech_flash_driver/zephyr/public_api"]
                sources += [ROOT / "tests/ota/storage_test.cpp"]
            else:
                sources += [OTA / "src/ota_participant.cpp", OTA / "src/ota_coordinator.cpp",
                            ROOT / "tests/ota/core_test.cpp"]
            if os.name == "nt":
                includes.insert(0, ROOT / "tests/ota/core_shim")
                sources += [ROOT / "tests/ota/core_shim.cpp"]
                args += ["-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe",
                         "-fno-exceptions", "-fno-rtti", "-nostdlib", "-shared", "-fuse-ld=lld",
                         "-DOWNTECH_FREESTANDING_TEST", "-Wl,/noentry", f"-Wl,/export:{symbol}"]
            args += [f"-I{p}" for p in includes] + [str(p) for p in sources] + ["-o", str(output)]
            built = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            if os.name == "nt":
                # Run DLL in a subprocess so Windows releases it before cleanup.
                result = subprocess.run([os.sys.executable, "-c",
                    "import ctypes,sys;lib=ctypes.CDLL(sys.argv[1]);"
                    "rc=getattr(lib,sys.argv[2])();print(rc);sys.exit(bool(rc))",
                    str(output), symbol], capture_output=True, text=True)
            else:
                result = subprocess.run([str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0,
                f"{name}_test.cpp failed at source line {result.stdout.strip()}\n{result.stderr}")

    def test_protocol_participant_coordinator(self):
        self.compile_and_run()

    def test_storage_and_journal(self):
        self.compile_and_run(storage=True)

    def test_storage_with_arm_enum_layout(self):
        self.compile_and_run(storage=True, short_enums=True)


if __name__ == "__main__":
    unittest.main()
