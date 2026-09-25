"""Execute the production CMake admission guard without a Zephyr build."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "zephyr/modules/owntech_ota/zephyr/CMakeLists.txt"


class SchedulerConfigTests(unittest.TestCase):
    def setUp(self):
        self.cmake = shutil.which("cmake")
        self.assertIsNotNone(self.cmake, "CMake required")
        directory = tempfile.TemporaryDirectory(prefix="owntech-ota-scheduler-")
        self.addCleanup(directory.cleanup)
        self.script = Path(directory.name) / "scheduler.cmake"
        # Only build-system actions are stubbed. All configuration checks come
        # from the production module, including their enabled/disabled scope.
        self.script.write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "function(zephyr_include_directories)\nendfunction()\n"
            "function(zephyr_library)\nendfunction()\n"
            "function(zephyr_library_sources)\nendfunction()\n"
            "function(zephyr_library_sources_ifdef)\nendfunction()\n"
            f'include("{MODULE.as_posix()}")\n', encoding="utf-8")

    def configure(self, **changes):
        settings = {
            "CONFIG_OWNTECH_OTA": "y",
            "CONFIG_PREEMPT_ENABLED": "y",
            "CONFIG_MAIN_THREAD_PRIORITY": 12,
            "CONFIG_THINGSET_CAN_THREAD_PRIORITY": 10,
            "CONFIG_THINGSET_SDK_THREAD_PRIORITY": 10,
        }
        settings.update(changes)
        return subprocess.run(
            [self.cmake] + [f"-D{name}={value}" for name, value in settings.items()]
            + ["-P", str(self.script)], capture_output=True, text=True, timeout=30)

    def assert_refused(self, diagnostic, **changes):
        result = self.configure(**changes)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(diagnostic, " ".join(result.stderr.split()))

    def test_main_below_all_services_is_accepted(self):
        for lead in ("n", "y"):
            with self.subTest(lead=lead):
                result = self.configure(CONFIG_OWNTECH_OTA_LEAD=lead)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_main_cannot_preempt_or_equal_ota_worker(self):
        for priority in (0, 7):
            with self.subTest(priority=priority):
                self.assert_refused("OTA worker priority", CONFIG_MAIN_THREAD_PRIORITY=priority)

    def test_main_cannot_equal_can_and_sdk_priorities(self):
        self.assert_refused("CONFIG_THINGSET_CAN_THREAD_PRIORITY", CONFIG_MAIN_THREAD_PRIORITY=10)

    def test_cooperative_main_is_rejected(self):
        self.assert_refused("preemptible main", CONFIG_MAIN_THREAD_PRIORITY=-1)

    def test_can_must_preempt_main(self):
        self.assert_refused("CONFIG_THINGSET_CAN_THREAD_PRIORITY", CONFIG_THINGSET_CAN_THREAD_PRIORITY=13)

    def test_sdk_must_preempt_main(self):
        self.assert_refused("CONFIG_THINGSET_SDK_THREAD_PRIORITY", CONFIG_THINGSET_SDK_THREAD_PRIORITY=13)

    def test_preemption_cannot_be_disabled(self):
        self.assert_refused("CONFIG_PREEMPT_ENABLED=y", CONFIG_PREEMPT_ENABLED="n")

    def test_guard_does_not_constrain_non_ota_builds(self):
        result = self.configure(CONFIG_OWNTECH_OTA="n", CONFIG_PREEMPT_ENABLED="n",
                                CONFIG_MAIN_THREAD_PRIORITY=-1)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
