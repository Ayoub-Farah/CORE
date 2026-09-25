"""Run terminal-v2 transition guards and all power-cut transaction boundaries."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TransitionTests(unittest.TestCase):
    def test_terminal_transition_guards_and_power_cuts(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None and os.name == "nt":
            candidate = Path("C:/Program Files/LLVM/bin/clang++.exe")
            if candidate.is_file():
                compiler = str(candidate)
        self.assertIsNotNone(compiler, "C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="ota-transition-") as temporary:
            target = Path(temporary) / ("transition.dll" if os.name == "nt" else "transition")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror"]
            includes = [ROOT / "owntech/recovery"]
            sources = [ROOT / "owntech/recovery/recovery_core.cpp",
                       ROOT / "owntech/recovery/transition_core.cpp",
                       ROOT / "tests/ota/transition_test.cpp"]
            if os.name == "nt":
                includes.insert(0, ROOT / "tests/ota/core_shim")
                sources.append(ROOT / "tests/ota/core_shim.cpp")
                command += ["-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe",
                            "-fno-exceptions", "-fno-rtti", "-nostdlib", "-shared", "-fuse-ld=lld",
                            "-DOWNTECH_FREESTANDING_TEST", "-Wl,/noentry", "-Wl,/export:ota_transition_test_run"]
            command += ["-I" + str(path) for path in includes]
            command += [str(path) for path in sources] + ["-o", str(target)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            run = ([os.sys.executable, "-c", "import ctypes,sys;rc=ctypes.CDLL(sys.argv[1]).ota_transition_test_run();print(rc);sys.exit(bool(rc))", str(target)]
                   if os.name == "nt" else [str(target)])
            completed = subprocess.run(run, capture_output=True, text=True, timeout=30)
            self.assertEqual(completed.returncode, 0,
                             "transition_test.cpp failure at source line " + completed.stdout + completed.stderr)


if __name__ == "__main__":
    unittest.main()
