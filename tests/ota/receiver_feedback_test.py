import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReceiverFeedbackTests(unittest.TestCase):
    def test_console_and_led_events(self):
        cc = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(cc, "Install clang++/g++ or set CXX")
        with tempfile.TemporaryDirectory(prefix="ota-wire-") as directory:
            executable = Path(directory) / "wire.exe"
            command = [cc, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
                       "-Wno-missing-field-initializers",
                       "-I", str(ROOT / "tests/ota/runtime_shim"),
                       "-I", str(ROOT / "tests/ota/sdk_shim"),
                       "-I", str(ROOT / "third_party/thingset-zephyr-sdk/include"),
                       "-I", str(ROOT / "zephyr/modules/owntech_ota/zephyr/public_api"),
                       str(ROOT / "tests/ota/receiver_feedback_test.cpp"), "-o", str(executable)]
            build = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], text=True, capture_output=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
