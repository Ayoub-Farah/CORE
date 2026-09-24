"""Execute the actual isolated recovery transaction without MCU or NVS hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]


class RecoveryTests(unittest.TestCase):
    def arm_fixture(self, temporary):
        package = Path.home() / ".platformio/packages/toolchain-gccarmnoneeabi/bin"
        compiler = shutil.which("arm-none-eabi-g++") or str(package / "arm-none-eabi-g++.exe")
        objcopy = shutil.which("arm-none-eabi-objcopy") or str(package / "arm-none-eabi-objcopy.exe")
        self.assertTrue(Path(compiler).is_file(), "PlatformIO ARM compiler required for legacy ABI fixture")
        target = Path(temporary) / "fixture.o"
        command = [compiler, "-std=c++17", "-mcpu=cortex-m4", "-mthumb", "-fshort-enums", "-c",
                   "-I" + str(ROOT / "zephyr/modules/owntech_ota/zephyr/src"),
                   "-I" + str(ROOT / "zephyr/modules/owntech_ota/zephyr/public_api"),
                   str(ROOT / "tests/ota/recovery_arm_fixture.cpp"), "-o", str(target)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        declarations = []
        for section, name, size, crc_offset in (("local", "ARM_JOURNAL", 240, 232),
                                                ("fleet", "ARM_FLEET", 824, 820)):
            binary = Path(temporary) / (section + ".bin")
            copied = subprocess.run([objcopy, "-O", "binary", "--only-section=.fixture_" + section,
                                     str(target), str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(copied.returncode, 0, copied.stdout + copied.stderr)
            data = bytearray(binary.read_bytes())
            self.assertEqual(len(data), size)
            data[crc_offset:crc_offset + 4] = zlib.crc32(data[:crc_offset]).to_bytes(4, "little")
            declarations.append("static const uint8_t " + name + "[]={" +
                                ",".join(str(byte) for byte in data) + "};\n")
        (Path(temporary) / "recovery_arm_fixture.h").write_text("".join(declarations), encoding="ascii")

    def test_guarded_recovery_and_every_interrupted_mutation(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None and os.name == "nt":
            candidate = Path("C:/Program Files/LLVM/bin/clang++.exe")
            if candidate.is_file():
                compiler = str(candidate)
        self.assertIsNotNone(compiler, "C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="ota-recovery-") as temporary:
            self.arm_fixture(temporary)
            target = Path(temporary) / ("recovery.dll" if os.name == "nt" else "recovery")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DOWNTECH_ARM_FIXTURE"]
            includes = [ROOT / "owntech/recovery", Path(temporary)]
            sources = [ROOT / "owntech/recovery/recovery_core.cpp", ROOT / "tests/ota/recovery_test.cpp"]
            if os.name == "nt":
                includes.insert(0, ROOT / "tests/ota/core_shim")
                sources.append(ROOT / "tests/ota/core_shim.cpp")
                command += ["-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe",
                            "-fno-exceptions", "-fno-rtti", "-nostdlib", "-shared", "-fuse-ld=lld",
                            "-DOWNTECH_FREESTANDING_TEST", "-Wl,/noentry", "-Wl,/export:ota_recovery_test_run"]
            command += ["-I" + str(path) for path in includes]
            command += [str(path) for path in sources] + ["-o", str(target)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            run = ([os.sys.executable, "-c", "import ctypes,sys;rc=ctypes.CDLL(sys.argv[1]).ota_recovery_test_run();print(rc);sys.exit(bool(rc))", str(target)]
                   if os.name == "nt" else [str(target)])
            completed = subprocess.run(run, capture_output=True, text=True, timeout=30)
            self.assertEqual(completed.returncode, 0,
                             "recovery_test.cpp failure at source line " + completed.stdout + completed.stderr)


if __name__ == "__main__":
    unittest.main()
