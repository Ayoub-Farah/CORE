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
            for fd in (False, True):
                with self.subTest(can_fd=fd):
                    executable = Path(directory) / ("sdk-fd.exe" if fd else "sdk-classic.exe")
                    command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                               "-Wno-unused-parameter", "-Wno-unused-function",
                               "-Wno-unused-variable", "-Wno-sign-compare",
                               "-I", str(ROOT / "tests/ota/sdk_shim"),
                               "-I", str(ROOT / "third_party/thingset-zephyr-sdk/include"),
                               str(ROOT / "tests/ota/sdk_transport.c"), "-o", str(executable)]
                    if fd:
                        command += ["-DCONFIG_CAN_FD_MODE=1", "-DCONFIG_THINGSET_CAN_FD_BRS=1"]
                    subprocess.run(command, check=True, capture_output=True, text=True)
                    subprocess.run([str(executable)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
