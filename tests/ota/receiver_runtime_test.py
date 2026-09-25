"""Execute the actual runtime against deterministic queue/storage/network seams."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OTA = ROOT / "zephyr/modules/owntech_ota/zephyr"


class ReceiverRuntimeTests(unittest.TestCase):
    def test_runtime_lifecycle(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="owntech-ota-runtime-") as tmp:
            output = Path(tmp) / ("runtime.dll" if os.name == "nt" else "runtime")
            args = [compiler, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror"]
            includes = [ROOT / "tests/ota/runtime_state_shim", OTA / "public_api", OTA / "src"]
            sources = [OTA / "src/ota_protocol.c", OTA / "src/ota_participant.cpp",
                       ROOT / "tests/ota/receiver_runtime_test.cpp"]
            if os.name == "nt":
                includes.insert(1, ROOT / "tests/ota/core_shim")
                sources += [ROOT / "tests/ota/core_shim.cpp"]
                args += ["-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe",
                         "-fno-exceptions", "-fno-rtti", "-nostdlib", "-shared", "-fuse-ld=lld",
                         "-DOWNTECH_FREESTANDING_TEST", "-Wl,/noentry", "-Wl,/export:ota_receiver_runtime_test_run"]
            args += [f"-I{p}" for p in includes] + [str(p) for p in sources] + ["-o", str(output)]
            built = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            if os.name == "nt":
                result = subprocess.run([os.sys.executable, "-c",
                    "import ctypes,sys;lib=ctypes.CDLL(sys.argv[1]);"
                    "rc=lib.ota_receiver_runtime_test_run();print(rc);sys.exit(bool(rc))", str(output)],
                    capture_output=True, text=True)
            else:
                result = subprocess.run([str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0,
                f"receiver_runtime_test.cpp failed at source line {result.stdout.strip()}\n{result.stderr}")


if __name__ == "__main__":
    unittest.main()
