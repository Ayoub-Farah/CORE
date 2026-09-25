"""Execute the production C transport against deterministic Zephyr boundary fakes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SdkTransportTests(unittest.TestCase):
    def test_classic_and_fd_transport(self):
        cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
        self.assertIsNotNone(cc, "Install clang/gcc (or set CC) to run the C transport tests")
        with tempfile.TemporaryDirectory(prefix="owntech-sdk-") as directory:
            for fd, receiver in ((False, False), (True, False), (False, True)):
                with self.subTest(can_fd=fd, receiver=receiver):
                    executable = Path(directory) / ("sdk-fd.exe" if fd else "sdk-classic.exe")
                    command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                               "-Wno-unused-parameter", "-Wno-unused-function",
                               "-Wno-unused-variable", "-Wno-sign-compare",
                               "-I", str(ROOT / "tests/ota/sdk_shim"),
                               "-I", str(ROOT / "third_party/thingset-zephyr-sdk/include"),
                               str(ROOT / ("tests/ota/sdk_receiver_transport.c" if receiver else "tests/ota/sdk_transport.c")), "-o", str(executable)]
                    if fd:
                        command += ["-DCONFIG_CAN_FD_MODE=1", "-DCONFIG_THINGSET_CAN_FD_BRS=1"]
                    built = subprocess.run(command, capture_output=True, text=True, timeout=90)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
