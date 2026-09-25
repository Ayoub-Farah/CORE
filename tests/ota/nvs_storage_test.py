"""Execute the production NVS adapter with and without OTA write protection."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NvsStorageTests(unittest.TestCase):
    def run_nvs(self, ota):
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "Install clang++/g++ or set CXX")
        with tempfile.TemporaryDirectory(prefix="owntech-nvs-") as tmp:
            output = Path(tmp) / ("nvs.dll" if os.name == "nt" else "nvs")
            args = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror"]
            includes = [ROOT / "tests/ota/nvs_shim",
                        ROOT / "zephyr/modules/owntech_ota/zephyr/public_api"]
            sources = [ROOT / "tests/ota/nvs_storage_test.cpp"]
            if ota:
                args += ["-DCONFIG_OWNTECH_OTA=1"]
            if os.name == "nt":
                includes.append(ROOT / "tests/ota/core_shim")
                sources.append(ROOT / "tests/ota/core_shim.cpp")
                args += ["-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe",
                         "-fno-exceptions", "-fno-rtti", "-nostdlib", "-shared", "-fuse-ld=lld",
                         "-DOWNTECH_FREESTANDING_TEST", "-Wl,/noentry", "-Wl,/export:ota_nvs_test_run"]
            args += [f"-I{p}" for p in includes] + [str(p) for p in sources] + ["-o", str(output)]
            built = subprocess.run(args, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            command = [str(output)]
            if os.name == "nt":
                command = [os.sys.executable, "-c",
                           "import ctypes,sys;lib=ctypes.CDLL(sys.argv[1]);"
                           "rc=lib.ota_nvs_test_run();print(rc);sys.exit(bool(rc))", str(output)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0,
                f"nvs_storage_test.cpp failed at source line {result.stdout.strip()}\n{result.stderr}")

    def test_ota_reserves_nvs_without_application_callback(self):
        self.run_nvs(ota=True)

    def test_non_ota_preserves_legacy_writes_and_clear(self):
        self.run_nvs(ota=False)


if __name__ == "__main__":
    unittest.main()
